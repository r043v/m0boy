#include "ws.h"
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

static uint8_t rom(void *context, uint32_t offset)
{
    (void)context;
    return offset & 255;
}

static void capture_audio(void *context, uint8_t speaker, int16_t left, int16_t right)
{
    int16_t *samples = context;
    samples[0] = speaker;
    samples[1] = left;
    samples[2] = right;
}

static void count_lines(void *context, const Ws *ws, unsigned line)
{
    unsigned *count = context;
    CHECK(ws->frames == 0 && *count == line);
    ++*count;
}

int main(void)
{
    Ws ws;
    const WsRom cart = {rom, NULL, 65536};
    CHECK(ws_boot(&ws, cart));
    CHECK(ws.internal_eeprom.size == WS_MONO_EEPROM_SIZE);
    ws_out(&ws, 0x60, 0xe0);
    ws_write(&ws, 0xfe00, 0x42);
    CHECK(ws_in(&ws, 0x60) == 0xff && ws_read(&ws, 0xfe00) == 0xff);

    uint8_t bios[WS_COLOR_BIOS_SIZE] = {[0] = 0xa5, [WS_COLOR_BIOS_SIZE - 16] = 0x90};
    CHECK(ws_boot_model(&ws, cart, true, bios));
    CHECK(ws_read(&ws, 0xfe000) == 0xa5 && ws_read(&ws, 0xffff0) == 0x90);
    CHECK(ws_rom_offset(&ws, 0xfe000) == UINT32_MAX);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 1);
    ws_out(&ws, 0xa0, 5);
    CHECK(ws_read(&ws, 0xfe000) == 0 && ws_rom_offset(&ws, 0xfe000) != UINT32_MAX);

    CHECK(ws_boot_model(&ws, cart, true, NULL));
    CHECK(ws.internal_eeprom.size == WS_COLOR_EEPROM_SIZE);
    ws.internal_nv[0x7fe] = 0xad;
    ws.internal_nv[0x7ff] = 0xba;
    ws_out(&ws, 0xbc, 0xff);
    ws_out(&ws, 0xbd, 0x1b);
    ws_out(&ws, 0xbe, 0x10);
    CHECK(ws_in(&ws, 0xba) == 0xad && ws_in(&ws, 0xbb) == 0xba);
    CHECK(ws_in(&ws, 0xa0) == 0x87);
    CHECK(ws_read(&ws, 0xfe00) == 0xff);
    ws_out(&ws, 0x60, 0x80);
    ws_write(&ws, 0xfe02, 0xbc);
    ws_write(&ws, 0xfe03, 0xfa);
    ws_out(&ws, 0, 1);
    ws_out(&ws, 7, 15);
    ws_write(&ws, 0x7801, 0x20); // Screen tile 0 comes from bank 1.
    ws_write(&ws, 0x4000, 0x80);
    uint16_t pixels[WS_WIDTH];
    ws_render_line_rgb(&ws, 0, pixels);
    CHECK(pixels[0] == 0xabc && pixels[1] == 0);

    ws_write(&ws, 0xfe14, 0x34);
    ws_write(&ws, 0xfe15, 0x02);
    ws_out(&ws, 0x60, 0xe0);
    ws_write(&ws, 0x8000, 0xa1);
    ws_render_line_rgb(&ws, 0, pixels);
    CHECK(pixels[0] == 0x234 && pixels[1] == 0xabc);
    ws_out(&ws, 0x60, 0xc0);
    ws_write(&ws, 0x8000, 0x00);
    ws_write(&ws, 0x8001, 0x80);
    ws_write(&ws, 0x8003, 0x80);
    ws_render_line_rgb(&ws, 0, pixels);
    CHECK(pixels[0] == 0x234 && pixels[1] == 0);

    ws_out(&ws, 0x40, 0x25);
    ws_out(&ws, 0x42, 2);
    ws_out(&ws, 0x44, 0x11);
    ws_out(&ws, 0x45, 0x40);
    ws_out(&ws, 0x46, 5);
    ws_out(&ws, 0x48, 0x80);
    ws_dma_run(&ws, NULL, NULL);
    CHECK(ws.line_clock == 9 && ws_in(&ws, 0x46) == 0 && ws_in(&ws, 0x48) == 0);
    CHECK(ws_read(&ws, 0x4010) == 0x24 && ws_read(&ws, 0x4013) == 0x27);
    ws_out(&ws, 0x42, 1); // SRAM cannot supply general DMA.
    ws_out(&ws, 0x46, 2);
    ws_out(&ws, 0x48, 0x80);
    ws_dma_run(&ws, NULL, NULL);
    CHECK(ws.line_clock == 9 && ws_in(&ws, 0x46) == 2);

    CHECK(ws_boot_model(&ws, cart, true, NULL));
    CHECK(ws_in(&ws, 0x52) == 0xff);
    ws_out(&ws, 0x60, 0x80);
    ws_write(&ws, 0xfefe, 0xfe);
    ws_write(&ws, 0xfeff, 0x01);
    ws_out(&ws, 0x4a, 0xff);
    ws_out(&ws, 0x4b, 0xfe);
    ws_out(&ws, 0x4e, 2);
    ws_out(&ws, 0x52, 0xcb); // 24 kHz, descending, repeat.
    ws_audio_clock(&ws, 127);
    CHECK(ws_in(&ws, 0x89) == 0);
    ws_audio_clock(&ws, 1);
    CHECK(ws_in(&ws, 0x89) == 0x01 && ws_in(&ws, 0x4a) == 0xfe);
    ws_audio_clock(&ws, 128);
    CHECK(ws_in(&ws, 0x89) == 0xfe && ws_in(&ws, 0x4a) == 0xff);
    CHECK(ws_in(&ws, 0x4e) == 2);
    ws_out(&ws, 0x52, 0xc7); // Hold writes silence without advancing.
    ws_audio_clock(&ws, 128);
    CHECK(ws_in(&ws, 0x89) == 0 && ws_in(&ws, 0x4e) == 2);

    int16_t samples[3] = {0};
    ws.audio.sink = capture_audio;
    ws.audio.context = samples;
    ws_out(&ws, 0x91, 9);
    ws_out(&ws, 0x6a, 0x88); // Signed 8-bit, full volume, every sample.
    ws_out(&ws, 0x6b, 0x60); // Both channels.
    ws_out(&ws, 0x4e, 1);
    ws_out(&ws, 0x4a, 0xfe);
    ws_out(&ws, 0x52, 0x93);
    ws_audio_clock(&ws, 256);
    CHECK(ws_in(&ws, 0x52) == 0x13 && ws_in(&ws, 0x4e) == 0);
    CHECK(samples[0] == 0 && samples[1] == -512 && samples[2] == -512);

    CHECK(ws_boot(&ws, cart));
    ws_out(&ws, 4, 3);
    ws_write(&ws, 3 * 512, 0xa1);
    ws_write(&ws, 3 * 512 + 2, 0xa2);
    ws_video_clock(&ws, 144 * 256 + 1, NULL, NULL);
    CHECK(ws.sprites[0] == 0xa1 && ws.sprites[2] == 0);
    ws_write(&ws, 3 * 512, 0xb1);
    ws_write(&ws, 3 * 512 + 2, 0xb2);
    ws_video_clock(&ws, 255, NULL, NULL);
    CHECK(ws.sprites[0] == 0xa1 && ws.sprites[2] == 0xb2);

    CHECK(ws_boot(&ws, cart));
    unsigned lines = 0;
    ws.bizhawk_timing = true;
    ws.line = WS_HEIGHT + 1;
    ws_video_clock(&ws, 158 * 256, count_lines, &lines);
    CHECK(lines == WS_HEIGHT && ws.line == WS_HEIGHT && ws.frames == 0);
    ws_video_clock(&ws, 256, count_lines, &lines);
    CHECK(ws.line == WS_HEIGHT + 1 && ws.frames == 1);

    CHECK(ws_boot(&ws, cart));
    ws_out(&ws, 0x18, 0xfe);
    CHECK(ws_in(&ws, 0x18) == 0xff && ws_in(&ws, 2) == 0);
    ws_video_clock(&ws, 256, NULL, NULL);
    CHECK(ws_in(&ws, 2) == 0xfe);
    ws_video_clock(&ws, 512, NULL, NULL);
    CHECK(ws_in(&ws, 2) == 0);
}
