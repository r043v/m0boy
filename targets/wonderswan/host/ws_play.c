#include "ws.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *bytes; } Rom;
static uint8_t read_rom(void *ctx, uint32_t offset) { return ((Rom *)ctx)->bytes[offset]; }

// The frame buffer is filled line by line, at line entry — the BizHawk sampling point of
// the bizhawk-timing replay, so the window shows exactly the movie's aligned frame.
static uint8_t framebuffer[WS_HEIGHT][WS_WIDTH * 3];

static void capture_line(void *context, const Ws *ws, unsigned line)
{
    (void)context;
    uint16_t row[WS_WIDTH];
    ws_render_line_rgb(ws, line, row);
    for (unsigned x = 0; x < WS_WIDTH; ++x) {
        framebuffer[line][3 * x] = ((row[x] >> 8) & 15) * 17;
        framebuffer[line][3 * x + 1] = ((row[x] >> 4) & 15) * 17;
        framebuffer[line][3 * x + 2] = (row[x] & 15) * 17;
    }
}

// Pumps the window's event queue; returns 0 once the user closed the window.
// Every long-running path calls this, so the window manager never sees a dead window.
static int pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT)
            return 0;
    return 1;
}

static void present_frame(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_UpdateTexture(texture, NULL, framebuffer, WS_WIDTH * 3);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

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
    // Battery SRAM must back the cartridge's SRAM window: a game whose boot probes it
    // (Rockman & Forte) parks at FFFF:FEFF with a blank LCD when reads answer FF forever.
    uint8_t *sram = NULL;
    if (ws.cart_sram_size) {
        sram = calloc(1, ws.cart_sram_size);
        if (!sram || !ws_attach_sram(&ws, sram, ws.cart_sram_size)) {
            fprintf(stderr, "Could not allocate %u KiB of cartridge SRAM\n", ws.cart_sram_size / 1024);
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_Window *window = SDL_CreateWindow("m0boy WonderSwan TAS", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, WS_WIDTH * 3, WS_HEIGHT * 3, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 2; }
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, WS_WIDTH, WS_HEIGHT);
    if (!texture) { fprintf(stderr, "SDL texture: %s\n", SDL_GetError()); return 2; }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    unsigned frame = 0, keys = 0;
    int running = 1;
    // Why the replay stopped (movie over, rejected input, core stop): the window keeps
    // rendering and stays responsive, the reason shows in the title and on stderr.
    const char *stopped = NULL;
    while (running) {
        running = pump_events();
        if (!running)
            break;
        if (!stopped) {
            if (fscanf(keys_file, "%x", &keys) != 1) {
                stopped = "movie over";
            } else if (keys & ~0xffeu) {
                // Not a ws_set_keys mask: most likely raw BizHawk buttons, not the host keys file.
                fprintf(stderr, "Invalid input 0x%0*x at frame %u (expected a 3-hex-digit ws_host keys file)\n",
                    keys > 0xfff ? 8 : 3, keys, frame + 1);
                stopped = "bad input";
            } else {
                ws_set_keys(&ws, keys);
            }
        }
        if (!stopped) {
            unsigned steps = 0;
            while (ws.frames == frame) {
                WsStep step = ws_step_with_video(&ws, capture_line, NULL);
                if (step != WS_STEP_OK) {
                    stopped = step == WS_STEP_HALTED ? "halted" : "unsupported opcode";
                    fprintf(stderr, "Replay stopped at frame %u: %s at %04X:%04X\n",
                        frame + 1, stopped, ws.cs, ws.ip);
                    break;
                }
                if ((++steps & 1023u) == 0 && !pump_events()) {
                    running = 0;
                    break;
                }
            }
            if (!stopped)
                ++frame;
        }
        if (!running)
            break;
        char title[112];
        if (stopped)
            snprintf(title, sizeof(title), "m0boy WonderSwan TAS — frame %u — %s", frame, stopped);
        else
            snprintf(title, sizeof(title), "m0boy WonderSwan TAS — frame %u", frame);
        SDL_SetWindowTitle(window, title);
        present_frame(renderer, texture);
        SDL_Delay(16);
    }
    if (running) {
        SDL_Event event;
        while (SDL_WaitEvent(&event) && event.type != SDL_QUIT) {}
    }
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit(); fclose(keys_file); free(sram); free(rom.bytes); return 0;
}
