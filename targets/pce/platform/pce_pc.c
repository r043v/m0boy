/* PC display, controller and audio for the PCE runtime: the HuCard is
 * embedded in the project (pce_rom), SDL2 supplies the window and the
 * sound.  The run fingerprint follows the host harness's recipe, so a
 * scripted session here prints the harness's run_hash.  Keyboard:
 * arrows, X = I, Z = II, Enter = RUN, Shift = SELECT, Esc quits.
 * --input plays a session script ("<frame> <buttons>", the harness's
 * dialect), --record-input writes one: a line per frame the game
 * receives buttons on, replayable by the harness untouched. */
#include "pce.h"
#include "pc_frontend.h"
#include "pc_input.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PCE_EMBEDDED
#include "pce_config.h"
extern const uint8_t pce_rom[];
#endif

#define AUDIO_RATE 11025u

static uint16_t rows[262][512];
static void on_row(void *host, unsigned line, const uint16_t *pixels, unsigned width) {
    (void)host;
    if (line < 262 && width <= 512)
        memcpy(rows[line], pixels, width * sizeof rows[0][0]);
}

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

int main(int argc, char **argv) {
    unsigned limit = 0;
    const char *ppm = NULL, *input_path = NULL, *record_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) record_path = argv[++i];
        else {
            fprintf(stderr, "usage: pce_pc [--frames N] [--ppm shot.ppm] [--input f] [--record-input f]\n");
            return 2;
        }
    }
    static const char *const pce_pad_names[8] = {
        "i", "ii", "select", "run", "up", "right", "down", "left"};
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, pce_pad_names, 8)) return 2;
    FILE *record = NULL;
    if (record_path) {
        record = fopen(record_path, "w");
        if (!record) { perror(record_path); return 2; }
        fprintf(record, "# PCE input recording: <frame> <buttons>\n");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    PcFrontend pc = {0};
    if (!pc_frontend_open(&pc, "m0boy PC Engine", 256, 240)) {
        SDL_Quit();
        return 1;
    }
    /* the sound: the frame's samples queued as 16-bit signed PCM */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    pce_init(pce_rom, PCE_ROM_SIZE);
    machine.row = on_row;

    int running = 1;
    unsigned carry = 0;
    static int16_t pcm[4096];
    static uint32_t pixels[262 * 512];
    uint64_t deadline = SDL_GetTicks64();
    unsigned frame_fraction = 0;
    while (running && (!limit || machine.frames < limit)) {
        pc_frontend_poll(&pc, 1);
        if (pc.quit) break;
        const uint16_t b = pc_frontend_buttons(&pc);
        machine.input = (uint8_t)((b & PC_UP ? PCE_BTN_UP : 0) |
            (b & PC_DOWN ? PCE_BTN_DOWN : 0) | (b & PC_LEFT ? PCE_BTN_LEFT : 0) |
            (b & PC_RIGHT ? PCE_BTN_RIGHT : 0) | (b & PC_A ? PCE_BTN_I : 0) |
            (b & PC_B ? PCE_BTN_II : 0) | (b & PC_START ? PCE_BTN_RUN : 0) |
            (b & PC_SELECT ? PCE_BTN_SELECT : 0));
        machine.input |= (uint8_t)pc_input_keys(&script, machine.frames);
        pc_record_buttons(record, machine.frames, machine.input, pce_pad_names, 8);
        pce_frame();
        if (audio) {
            carry += AUDIO_RATE * 119210u;
            unsigned count = carry / 7159090u;
            carry %= 7159090u;
            if (count > sizeof pcm / sizeof pcm[0] / 2)
                count = sizeof pcm / sizeof pcm[0] / 2;
            uint8_t samples[sizeof pcm / sizeof pcm[0]];
            psg_render_stereo(&machine.psg, samples, count, AUDIO_RATE);
            for (unsigned i = 0; i < count * 2; i++)
                pcm[i] = (int16_t)(((int)samples[i] - 128) * 256);
            SDL_QueueAudio(audio, pcm, count * 2 * sizeof pcm[0]);
        }
        if (machine.have_frame) {
            const unsigned w = machine.vdc.active_width, h = machine.vdc.active_height;
            /* the same 8-bit expansion as c * 255 / 31 and c * 255 / 63,
             * read from tables instead of divided per pixel */
            static uint32_t lut_r[32], lut_g[64], lut_b[32];
            if (!lut_b[31]) {
                for (unsigned i = 0; i < 32; i++) {
                    lut_r[i] = 0xff000000u | (i * 255u / 31u) << 16;
                    lut_b[i] = i * 255u / 31u;
                }
                for (unsigned i = 0; i < 64; i++) lut_g[i] = (i * 255u / 63u) << 8;
            }
            for (unsigned y = 0; y < h; y++) {
                const uint16_t *src = rows[machine.first_active_line + y];
                uint32_t *dst = pixels + y * 512;
                for (unsigned x = 0; x < w; x++) {
                    const uint16_t c = src[x];
                    dst[x] = lut_r[c >> 11] | lut_g[(c >> 5) & 63u] | lut_b[c & 31u];
                }
            }
            if (!pc_frontend_present(&pc, pixels, w, h, 512)) { running = 0; break; }
        }
        deadline += 16u;
        frame_fraction += 2u;
        if (frame_fraction >= 3u) { ++deadline; frame_fraction -= 3u; }
        uint64_t now = SDL_GetTicks64();
        if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
        else if (now - deadline > 100u) { deadline = now; frame_fraction = 0; }
    }
    const int saved = !ppm || (machine.have_frame && ({
        FILE *file = fopen(ppm, "wb");
        int ok = file != NULL;
        if (file) {
            fprintf(file, "P6\n%u %u\n255\n", machine.vdc.active_width, machine.vdc.active_height);
            for (unsigned y = 0; y < machine.vdc.active_height && ok; y++)
                for (unsigned x = 0; x < machine.vdc.active_width && ok; x++) {
                    const uint16_t c = rows[machine.first_active_line + y][x];
                    const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8),
                                           (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                    ok = fwrite(px, 1, 3, file) == 3;
                }
            ok &= fclose(file) == 0;
        }
        ok;
    }));
    uint32_t h = 2166136261u;
    const uint8_t regs[12] = {
        machine.cpu.a, machine.cpu.x, machine.cpu.y, machine.cpu.s, machine.cpu.p,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
        (uint8_t)machine.cpu.cycles, (uint8_t)(machine.cpu.cycles >> 8),
        (uint8_t)(machine.cpu.cycles >> 16), (uint8_t)(machine.cpu.cycles >> 24),
        (uint8_t)(machine.frames & 0xFFu),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(machine.wram, sizeof machine.wram, h);
    h = fnv(machine.vdc.vram, sizeof machine.vdc.vram, h);
    h = fnv(machine.vdc.sat, sizeof machine.vdc.sat, h);
    h = fnv(machine.vce_colors, sizeof machine.vce_colors, h);
    if (machine.have_frame)
        for (unsigned y = 0; y < machine.vdc.active_height; y++)
            h = fnv(rows[machine.first_active_line + y],
                    machine.vdc.active_width * sizeof rows[0][0], h);
    printf("frames=%u pc=%04x run_hash=%08x\n", machine.frames, machine.cpu.pc, h);
    if (record) {
        if (fflush(record) | pc_record_close(record))
            perror(record_path);
        else
            fprintf(stderr, "recording: %s (%u frames)\n", record_path, machine.frames);
    }
    if (audio) SDL_CloseAudioDevice(audio);
    pc_frontend_close(&pc);
    SDL_Quit();
    return !saved;
}
