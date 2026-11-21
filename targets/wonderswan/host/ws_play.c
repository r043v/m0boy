#include "ws.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *bytes; } Rom;
static uint8_t read_rom(void *ctx, uint32_t offset) { return ((Rom *)ctx)->bytes[offset]; }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: ws_play ROM keys.txt\n"); return 2; }
    FILE *rom_file = fopen(argv[1], "rb"), *keys_file = fopen(argv[2], "r");
    if (!rom_file || !keys_file) { perror("open"); return 2; }
    fseek(rom_file, 0, SEEK_END); long size = ftell(rom_file); rewind(rom_file);
    Rom rom = {malloc((size_t)size)};
    if (!rom.bytes || fread(rom.bytes, 1, (size_t)size, rom_file) != (size_t)size) return 2;
    fclose(rom_file);
    Ws ws;
    if (!ws_boot_model(&ws, (WsRom){read_rom, &rom, (uint32_t)size}, true, NULL)) return 2;
    ws.bizhawk_timing = true;
    ws.line = WS_HEIGHT + 1;
    memcpy(ws.ram + 0x75ac, "A_C1n_c1", 8);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_Window *window = SDL_CreateWindow("m0boy WonderSwan TAS", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, WS_WIDTH * 3, WS_HEIGHT * 3, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, WS_WIDTH, WS_HEIGHT);
    uint8_t pixels[WS_WIDTH * WS_HEIGHT * 3];
    unsigned frame = 0, keys = 0;
    int running = 1;
    while (running && fscanf(keys_file, "%x", &keys) == 1) {
        ws_set_keys(&ws, keys);
        while (ws.frames == frame && ws_step(&ws) == WS_STEP_OK) {
            SDL_Event event; while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) running = 0;
            if (!running) break;
        }
        if (!running) break;
        ++frame;
        for (unsigned y = 0; y < WS_HEIGHT; ++y) {
            uint16_t row[WS_WIDTH]; ws_render_line_rgb(&ws, y, row);
            for (unsigned x = 0; x < WS_WIDTH; ++x) {
                unsigned i = (y * WS_WIDTH + x) * 3;
                pixels[i] = ((row[x] >> 8) & 15) * 17;
                pixels[i + 1] = ((row[x] >> 4) & 15) * 17;
                pixels[i + 2] = (row[x] & 15) * 17;
            }
        }
        SDL_UpdateTexture(texture, NULL, pixels, WS_WIDTH * 3);
        SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL); SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    if (running) {
        SDL_Event event;
        while (SDL_WaitEvent(&event) && event.type != SDL_QUIT) {}
    }
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit(); fclose(keys_file); free(rom.bytes); return 0;
}
