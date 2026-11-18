/* PC display, controller and audio for the Neo Geo Pocket runtime: the
 * cartridge is embedded in the project (ngp_rom_data), SDL2 supplies the
 * window and the sound.  The run fingerprint follows the host harness's
 * recipe, so a scripted session here prints the harness's run_hash.
 * Keyboard: arrows, X = A, Z = B, Enter = Option, Esc quits.
 * --input plays a session script ("<frame> <buttons>", the harness's
 * dialect), --record-input writes one. */
#include "ngp.h"
#include "ngp_hash.h"
#include "pc_frontend.h"
#include "pc_input.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NGP_EMBEDDED
extern const uint8_t ngp_rom_data[];
extern const unsigned ngp_rom_size;
#endif

#define AUDIO_RATE 44100u

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
            fprintf(stderr, "usage: ngp_pc [--frames N] [--ppm shot.ppm] [--input f] [--record-input f]\n");
            return 2;
        }
    }
    static const char *const ngp_pad_names[7] = {
        "up", "down", "left", "right", "a", "b", "option"};
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, ngp_pad_names, 7)) return 2;
    FILE *record = NULL;
    if (record_path) {
        record = fopen(record_path, "w");
        if (!record) { perror(record_path); return 2; }
        fprintf(record, "# NGP input recording: <frame> <buttons>\n");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    PcFrontend pc = {0};
    if (!pc_frontend_open(&pc, "m0boy Neo Geo Pocket", NGP_SCREEN_W, NGP_SCREEN_H)) {
        SDL_Quit();
        return 1;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    {
        uint8_t *rom = malloc(ngp_rom_size);
        memcpy(rom, ngp_rom_data, ngp_rom_size); /* the flash writes land here */
        machine.bios = malloc(NGP_BIOS_BYTES);
        ngp_bios_install();
        ngp_load_rom(rom, ngp_rom_size);
        ngp_reset();
    }

    int running = 1;
    static int16_t pcm[44100 / 50 * 4];
    static uint32_t pixels[NGP_SCREEN_H * NGP_SCREEN_W];
    /* the 4-bit channels spread with *17, the 5-bit frame word widened */
    static uint32_t lut_r[32], lut_g[32], lut_b[32];
    if (!lut_b[31]) {
        for (unsigned i = 0; i < 32; i++) {
            lut_r[i] = 0xff000000u | (i * 255u / 31u) << 16;
            lut_g[i] = (i * 255u / 31u) << 8;
            lut_b[i] = i * 255u / 31u;
        }
    }
    uint64_t deadline = SDL_GetTicks64();
    while (running && (!limit || machine.frames < limit)) {
        pc_frontend_poll(&pc, 1);
        if (pc.quit) break;
        const uint16_t b = pc_frontend_buttons(&pc);
        machine.buttons = (uint8_t)((b & PC_UP ? NGP_BTN_UP : 0) |
            (b & PC_DOWN ? NGP_BTN_DOWN : 0) | (b & PC_LEFT ? NGP_BTN_LEFT : 0) |
            (b & PC_RIGHT ? NGP_BTN_RIGHT : 0) | (b & PC_B ? NGP_BTN_A : 0) |
            (b & PC_A ? NGP_BTN_B : 0) | (b & PC_START ? NGP_BTN_OPTION : 0));
        machine.buttons |= (uint8_t)pc_input_keys(&script, machine.frames);
        if (record) pc_record_buttons(record, machine.frames, machine.buttons, ngp_pad_names, 7);
        ngp_run_frame();
        if (audio) {
            unsigned count = ngp_audio(pcm, sizeof pcm / sizeof pcm[0] / 2, AUDIO_RATE);
            SDL_QueueAudio(audio, pcm, count * 2 * sizeof pcm[0]);
        }
        {
            const uint16_t *src = ngp_frame();
            for (unsigned i = 0; i < NGP_SCREEN_W * NGP_SCREEN_H; i++) {
                const uint16_t c = src[i];
                pixels[i] = lut_r[c >> 11] | lut_g[(c >> 5) & 31u] | lut_b[c & 31u];
            }
            if (!pc_frontend_present(&pc, pixels, NGP_SCREEN_W, NGP_SCREEN_H, NGP_SCREEN_W)) {
                running = 0;
                break;
            }
        }
        deadline += 1000u / 60u; /* ~60.2 Hz: 198 lines of 515 ticks at 6.144 MHz */
        uint64_t now = SDL_GetTicks64();
        if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
        else if (now - deadline > 100u) deadline = now;
    }
    const int saved = !ppm || ({
        FILE *file = fopen(ppm, "wb");
        int ok = file != NULL;
        if (file) {
            fprintf(file, "P6\n%u %u\n255\n", NGP_SCREEN_W, NGP_SCREEN_H);
            const uint16_t *src = ngp_frame();
            for (unsigned i = 0; i < NGP_SCREEN_W * NGP_SCREEN_H && ok; i++) {
                const uint16_t c = src[i];
                const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8),
                                       (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                ok = fwrite(px, 1, 3, file) == 3;
            }
            ok &= fclose(file) == 0;
        }
        ok;
    });
    printf("frames=%u pc=%06x run_hash=%08x\n", machine.frames, machine.cpu.pc,
           ngp_run_hash(ngp_frame()));
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
