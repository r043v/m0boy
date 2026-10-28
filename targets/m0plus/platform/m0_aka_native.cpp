/* META firmware on the Gamebuino AKA: the translated Thumb code and the
 * SAMD21 board model (samd21.c) run on the ESP32-S3; no emulator. */
#include <Arduino.h>
extern "C" {
#include "samd21.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
}
#include "aka_hw.h"
#include "m0_target.h"
extern "C" const uint8_t m0_rom[];
extern "C" const uint32_t m0_rom_size;
extern "C" int m0_native_step(M0Cpu *);
static Samd21 board;                                    /* guest SRAM and the peripherals: internal RAM */
static uint16_t pixels[SAMD21_W * SAMD21_H];
static uint16_t rows[8][SAMD21_W] __attribute__((aligned(64)));
extern "C" void md_platform_audio_rate(unsigned rate) { samd21_audio_rate(&board, rate); }
extern "C" void md_platform_audio_render(uint8_t *out, unsigned n) { samd21_audio(&board, out, n); }
extern "C" int m0boy_console_main(void) {
    Serial.begin(115200);
    m0_lcd_init();
    if (samd21_init(&board, m0_rom, m0_rom_size, M0_FLASH_BASE, pixels)) {
        m0_lcd_fill(M0_COLOR(0x00800000));
        for (;;) delay(1000);
    }
    m0_audio_init();
    uint32_t next = m0_micros();
    /* Profiling: prints, once a second on the serial port (115200), where the
     * time goes (guest emulation / audio / screen), the frame rate reached and
     * the guest cycles per frame. */
    uint32_t t_emu = 0, t_aud = 0, t_lcd = 0, frames = 0, t_rep = m0_micros();
    uint64_t c_rep = board.m.cpu.cycles;
    for (;;) {
        uint32_t t0 = m0_micros();
        samd21_buttons(&board, esp32_buttons());
        samd21_frame(&board, m0_native_step);
        uint32_t t1 = m0_micros();
        m0_audio_frame();
        uint32_t t2 = m0_micros();
        for (unsigned y0 = 0; y0 < SAMD21_H; y0 += 8) {
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < SAMD21_W; ++x) {
                    /* the board keeps the META's BGR565, which is what this panel shows
                     * right with MADCTL's bit 3 clear (tester: swapped to RGB it came
                     * out BGR); the i80 bus swaps the bytes, aka_hw.cpp */
                    rows[y][x] = pixels[(y0 + y) * SAMD21_W + x];
                }
            }
            m0_lcd_send_rows(rows, (uint8_t)y0, 8);
        }
        uint32_t t3 = m0_micros();
        t_emu += t1 - t0; t_aud += t2 - t1; t_lcd += t3 - t2; ++frames;
        if (t3 - t_rep >= 1000000u) {
            Serial.printf("[perf] %u fps | per frame: emu %u us, audio %u us, lcd %u us | "
                          "%u Mcycles/s guest, idle skipped %u Mc\n",
                          frames, t_emu / frames, t_aud / frames, t_lcd / frames,
                          (unsigned)((board.m.cpu.cycles - c_rep) / 1000000u),
                          (unsigned)(board.idle_skipped / 1000000u));
            t_emu = t_aud = t_lcd = frames = 0; t_rep = t3; c_rep = board.m.cpu.cycles;
        }
        next += 16743u;
        const uint32_t now = m0_micros();
        if ((int32_t)(next - now) > 0) delay((next - now) / 1000u);
        else next = now;
        m0_watchdog_feed();
    }
}
