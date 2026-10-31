/* PC display, controller and audio for the SNES runtime: the cartridge is
 * embedded in the project (snes_rom), SDL2 supplies the window and the
 * sound.  The window plays at the source's native 256x224; the audio is
 * the S-DSP's own 32000 Hz stereo.  The run fingerprint follows the
 * harness's recipe (snes_hash.h), so a scripted session here prints the
 * harness's run_hash.  Keyboard: arrows, Z = B, X = A, A = Y, S = X,
 * Q/E = L/R, Enter = Start, Shift = Select, Esc quits; the gamepad maps
 * its A/B/X/Y and the shoulders the same way.  --input plays a session
 * script ("<frame> <buttons>", the harness's dialect), --record-input
 * writes one, replayable by the harness untouched. */
#include "pc_frontend.h"
#include "pc_input.h"
#include "snes.h"
#include "snes_hash.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNES_EMBEDDED
#include "snes_config.h"
extern const uint8_t snes_rom[];
#endif

#define AUDIO_RATE 32000u

/* the shared frontend's bits to the controller report's BYsS UDLR AXLR */
static uint16_t snes_report(uint16_t b) {
    return (uint16_t)((b & PC_B ? 0x8000u : 0) |      /* B */
                      (b & PC_C ? 0x4000u : 0) |      /* Y */
                      (b & PC_SELECT ? 0x2000u : 0) | /* select */
                      (b & PC_START ? 0x1000u : 0) |  /* start */
                      (b & PC_UP ? 0x0800u : 0) |
                      (b & PC_DOWN ? 0x0400u : 0) |
                      (b & PC_LEFT ? 0x0200u : 0) |
                      (b & PC_RIGHT ? 0x0100u : 0) |
                      (b & PC_A ? 0x0080u : 0) |      /* A */
                      (b & PC_D ? 0x0040u : 0) |      /* X */
                      (b & PC_L ? 0x0020u : 0) |
                      (b & PC_R ? 0x0010u : 0));
}

static const char *const snes_pad_names[12] = {
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r"
};

int main(int argc, char **argv) {
    unsigned limit = 0;
    const char *ppm = NULL, *input_path = NULL, *record_path = NULL, *wav_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) record_path = argv[++i];
        else {
            fprintf(stderr, "usage: snes_pc [--frames N] [--ppm f] [--wav f] [--input f] [--record-input f]\n");
            return 2;
        }
    }
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, snes_pad_names, 12)) return 2;
    FILE *record = NULL;
    if (record_path) {
        record = fopen(record_path, "w");
        if (!record) { perror(record_path); return 2; }
        fprintf(record, "# SNES input recording: <frame> <buttons>\n");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    PcFrontend pc = {0};
    if (!pc_frontend_open(&pc, "m0boy Super NES", 256, 224)) {
        SDL_Quit();
        return 1;
    }
    /* the sound: the ring's samples queued as 16-bit signed PCM at the
     * DSP's own rate */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    snes_init(snes_rom, SNES_ROM_SIZE, SNES_MAP, SNES_SRAM_BYTES, SNES_DSP1, SNES_SA1, SNES_FX);

    int running = 1;
    static int16_t pcm[4096];
    static uint32_t pixels[224 * 256];
    int16_t *captured = 0;
    size_t captured_n = 0, captured_cap = 0;
    uint64_t deadline = SDL_GetTicks64();
    unsigned frame_fraction = 0;
    while (running && (!limit || machine.frames < limit)) {
        pc_frontend_poll(&pc, 1);
        if (pc.quit) break;
        uint16_t report = snes_report(pc_frontend_buttons(&pc));
        const unsigned keys = pc_input_keys(&script, machine.frames);
        for (unsigned i = 0; i < 12; i++) if (keys & (1u << i)) report |= (uint16_t)(0x8000u >> i);
        snes_set_joypad(report);
        pc_record_buttons(record, machine.frames, keys, snes_pad_names, 12);
        snes_frame();
        /* the ring's rendered samples of the frame: to the device and to
         * the captured buffer (for --wav and the audio_hash); the ring
         * holds interleaved slots, the harness drains the same way */
        const unsigned fill = (unsigned)((machine.dsp.ring_w - machine.dsp.ring_r) & 8191);
        if (captured_n + fill > captured_cap) {
            captured_cap = (captured_n + fill + 8192u) & ~(size_t)8191u;
            captured = realloc(captured, captured_cap * sizeof *captured);
        }
        unsigned n = 0;
        for (; n < fill; n++) {
            const int16_t s = machine.dsp.ring[machine.dsp.ring_r];
            machine.dsp.ring_r = (machine.dsp.ring_r + 1) & 8191;
            captured[captured_n++] = s;
            if (n < sizeof pcm / sizeof pcm[0]) pcm[n] = s;
        }
        if (audio && n) SDL_QueueAudio(audio, pcm, n * sizeof pcm[0]);
        /* the frame the lines displayed, native 256x224 */
        for (unsigned y = 0; y < 224; y++)
            for (unsigned x = 0; x < 256; x++) {
                const uint8_t *p = machine.screen + (y * 256u + x) * 3u;
                pixels[y * 256 + x] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
        if (!pc_frontend_present(&pc, pixels, 256, 224, 256)) { running = 0; break; }
        deadline += 16u;
        frame_fraction += 2u;
        if (frame_fraction >= 3u) { ++deadline; frame_fraction -= 3u; }
        uint64_t now = SDL_GetTicks64();
        if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
        else if (now - deadline > 100u) { deadline = now; frame_fraction = 0; }
    }
    int ok = 1;
    if (ppm) {
        FILE *file = fopen(ppm, "wb");
        ok = file != NULL;
        if (file) {
            fprintf(file, "P6\n256 224\n255\n");
            ok &= fwrite(machine.screen, 1, sizeof machine.screen, file) == sizeof machine.screen;
            ok &= fclose(file) == 0;
        }
    }
    if (wav_path && captured_n) {
        FILE *file = fopen(wav_path, "wb");
        ok = ok && file != NULL;
        if (file) {
            const uint32_t data_bytes = (uint32_t)(captured_n * sizeof *captured);
            const uint8_t header[] = {
                'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                16,0,0,0, 1,0, 2,0, 0x80,0x7d,0,0, 0,0x25,3,0, 4,0, 16,0,
                'd','a','t','a', (uint8_t)data_bytes, (uint8_t)(data_bytes >> 8),
                (uint8_t)(data_bytes >> 16), (uint8_t)(data_bytes >> 24),
            };
            uint8_t hdr[44];
            memcpy(hdr, header, sizeof header);
            const uint32_t riff = data_bytes + 36;
            hdr[4] = (uint8_t)riff; hdr[5] = (uint8_t)(riff >> 8);
            hdr[6] = (uint8_t)(riff >> 16); hdr[7] = (uint8_t)(riff >> 24);
            ok &= fwrite(hdr, 1, 44, file) == 44;
            ok &= fwrite(captured, sizeof *captured, captured_n, file) == captured_n;
            ok &= fclose(file) == 0;
        }
    }
    printf("frames=%u pc=%04x run_hash=%08x image_hash=%08x audio_hash=%08x\n",
           machine.frames, machine.cpu.pc, snes_run_hash(), snes_image_hash(),
           snes_audio_hash(captured, captured_n));
    free(captured);
    if (record) {
        if (fflush(record) | pc_record_close(record))
            perror(record_path);
        else
            fprintf(stderr, "recording: %s (%u frames)\n", record_path, machine.frames);
    }
    if (audio) SDL_CloseAudioDevice(audio);
    pc_frontend_close(&pc);
    SDL_Quit();
    return !ok;
}
