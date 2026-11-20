/* SG-1000 frontend for the Gamebuino META and the Pokitto: recompiled code
 * (sg_gen.c) on the shared runtime, the consoles' LCD, audio, clock and
 * button drivers. */
#include "sg_config.h"
#ifdef M0_ROM_EXTERNAL
#include "m0_rom_ext.h"
#include "sg_rom_id.h"
#endif
#include "sg1000.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_audio.h"
#include "m0_lcd.h"
#if M0_TARGET_ESPBOY
#include "espboy_hw.h"
#elif M0_TARGET_POKITTO
#include "pokitto_hw.h"
#elif M0_TARGET_PC
#include "pc_hw.h"
#include <stdlib.h>
#elif M0_TARGET_ESP32
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif
#else
#include "meta_hw.h"
#endif
#include "sg_frontend.h"
#include "m0_scale.h" /* the vertical plan (m0_row_of) */
#include "m0_rows.h"
#if defined(GG_STATE_NAME83)
#include "m0_card.h"
#endif
extern "C" {
#ifdef M0_ROM_EXTERNAL /* the PC build loads its cartridge at run time (see sg_rom_id.h) */
extern const uint8_t *sg_rom;
#else
extern const uint8_t sg_rom[];
#endif
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#if M0_TARGET_POKITTO && !defined(GG_STATE_NAME83) /* else pokitto_sd.c */
void pokitto_sd_clock_changed(uint32_t) {}
#endif
}
#if SG_CART_RAM
static uint8_t cart_ram[SG_CART_RAM_SIZE] __attribute__((aligned(4)));
#endif
#if M0_TARGET_POKITTO
/* one row, or with save states the 512 bytes of their sector scratch */
#if defined(GG_STATE_NAME83)
static uint16_t line_buffer[256] __attribute__((aligned(4)));
#else
static uint16_t line_buffer[SG_OUT_WIDTH] __attribute__((aligned(4)));
#endif
#else
/* Displayed lines go to the LCD in batches of consecutive rows (one window
 * and one DMA chain per batch); two batches alternate. */
#define SG_BATCH (SG_LOW_RAM ? 1 : 4)
static uint16_t line_buffers[2][SG_BATCH][SG_OUT_WIDTH] __attribute__((aligned(4)));
static m0_rows rows;
static unsigned batch_rows, batch_dest;
#endif
static uint32_t deadline;
#ifndef SG_MAX_SKIP
#define SG_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
#ifdef SG_SELFTEST
extern "C" { volatile unsigned sg_selftest_result; }
#endif
static const char version[] __attribute__((used)) = "m0boy-sg1000-" M0_TARGET_NAME " " GBL_GEN_VERSION;

static int draw_line(void *, unsigned y);
static void row(void *, unsigned y, uint16_t *pixels);
static void frame(void *);
/* The console set up: the runtime inited and the callbacks in place (the
 * option menu's Redemarrer runs it again). */
static void sg_start(void) {
#if SG_CART_RAM
    sg1000_init(sg_rom, sg_rom_size, cart_ram);
#if M0_TARGET_ESPBOY
    espboy_ram_load(cart_ram, SG_CART_RAM_SIZE);
#endif
#else
    sg1000_init(sg_rom, sg_rom_size, 0);
#endif
    machine.lookup = sg_gen_lookup;
    machine.draw_line = draw_line;
    machine.row = row;
#if M0_TARGET_POKITTO
    machine.row_out = line_buffer;
#else
    machine.row_out = line_buffers[0][0];
#endif
    machine.frame = frame;
}
/* ---- option menu (sg_menu.h): HOME held half a second, MENU stays Pause -- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows) {
#if M0_TARGET_POKITTO
    *rows = 1; /* the single line buffer: bands sliced to one row */
    return line_buffer;
#else
    /* the batch buffers, contiguous: the menu's bands of 8 rows are sliced
     * to what fits */
    *rows = 2u * SG_BATCH;
    return line_buffers[0][0];
#endif
}
#if M0_TARGET_POKITTO
static unsigned pause_frames; /* a HOME tap (the Pokitto's C) becomes Pause */
#define SG_MENU_TAP() do { pause_frames = 6; } while (0)
#else
#define SG_MENU_TAP() do { } while (0)
#endif
#define SG_MENU_RESTART() do { \
        sg_start(); /* the cartridge RAM cleared: a power cycle */ \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#if defined(GG_STATE_NAME83)
#define SG_MENU_SD_DESELECT() m0_sd_deselect()
#else
#define SG_MENU_SD_DESELECT() do { } while (0)
#endif
#if M0_TARGET_ESPBOY
#define SG_MENU_QUIT() do { espboy_restart(); } while (0)
#elif M0_TARGET_POKITTO
#define SG_MENU_QUIT() do { \
        m0_audio_stop(); \
        SG_MENU_SD_DESELECT(); \
        pokitto_enter_loader(); \
        m0_audio_init(); \
    } while (0)
#elif M0_TARGET_PC
#define SG_MENU_QUIT() do { exit(0); } while (0)
#elif M0_TARGET_ESP32
#define SG_MENU_QUIT() do { esp32_restart(); } while (0)
#else
#if META_USB_SERIAL
#define SG_MENU_USB_STOP() meta_usb_stop()
#else
#define SG_MENU_USB_STOP() do { } while (0)
#endif
/* The USB bootloader (sam-ba) directly, as the Game Gear frontend does. */
#define SG_MENU_BOOTLOADER() do { \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        SG_MENU_USB_STOP(); \
        SG_MENU_SD_DESELECT(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
#if META_USB_SD
#include "meta_usb_msc.h"
#define SG_MENU_USB_SD() do { \
        m0_lcd_end_frame(); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        unsigned rows; \
        uint8_t *sector = (uint8_t *)m0_menu_scratch(&rows); \
        meta_usb_msc_enter(sector); \
    } while (0)
#endif
/* Hand the console back to the Gamebuino loader in the state it expects
 * (the Game Boy frontend's enter_loader). */
#define SG_MENU_QUIT() do { \
        m0_lcd_end_frame(); \
        m0_clock_set_lcd_spi(24); /* the loader expects the core's clocks */ \
        m0_clock_set(48); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        SG_MENU_USB_STOP(); \
        SG_MENU_SD_DESELECT(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#endif
/* The game draws 120 (Pokitto 150) of the screen's rows: the menu's
 * leftovers on the others are cleared. */
#define SG_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#if defined(GG_STATE_SLOTS)
/* Save states (sg_menu.h's Etat lines): the card's state file, in the
 * folder of the game's .BIN on the META (ESPboy: the flash,
 * espboy_flash.cpp); the line buffers as the sector scratch (the menu has
 * closed). */
#if M0_TARGET_POKITTO
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffer)
#else
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffers)
#endif
#define SG_MENU_STATE_SLOTS GG_STATE_SLOTS
#define SG_MENU_STATE_LOADED() \
    do { \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#endif
#if defined(GG_STATE_NAME83)
static int sg_card_open(void) {
    if (!m0_sd_init()) return 0;
#if M0_TARGET_META
    m0_fat_dir = m0_fat_game_dir(m0_card_sector, SG_MENU_STATE_BUF(), GG_BIN_NAME83, META_APP_START);
#endif
    return 1;
}
#endif
#include "sg_menu.h"

static unsigned display_row(unsigned y) {
#if M0_TARGET_POKITTO
    return m0_row_of(y, 32, 25); /* 192 -> 150 */
#elif M0_TARGET_PC
    return y; /* the window is the TMS9918A's own 256x192 */
#else
    return m0_row_of(y, 8, 5); /* 192 -> 120 */
#endif
}
/* Where the drawn rows start on the panel: the 120-row consoles crop the
 * TMS9918A's border (rows 4..123), the PC shows all 192 rows from 0, and
 * the ESP32 panel zooms the 120 visible rows x2 from row 0
 * (M0_PANEL_SRC_ROWS=120). */
#if M0_TARGET_PC || M0_TARGET_ESP32
#define SG_PANEL_ROW0(row) (row)
#else
#define SG_PANEL_ROW0(row) ((row) + 4)
#endif
/* Every source line counts: two lines that land on the same screen row are
 * drawn together as one row (the renderer averages them), so no line is
 * dropped (192 -> 120: rows of 2,2,1,2,1 lines; 192 -> 150 alike). */
static int draw_line(void *, unsigned y) {
    if (skip_video) return 0;
    if (y + 1 < SG_HEIGHT && display_row(y + 1) == display_row(y)) return 0; /* first of a pair */
    return y && display_row(y - 1) == display_row(y) ? 2 : 1;
}
/* Lines are rendered straight into the buffer machine.row_out points to. */
#if M0_TARGET_POKITTO
static void row(void *, unsigned y, uint16_t *pixels) {
    m0_rows_zoom(pixels, display_row(y) + 13, display_row(y) + 14);
}
#else
static void flush_batch(void) {
    if (!batch_rows) return;
    const unsigned slot = m0_rows_send(&rows, line_buffers[rows.slot], SG_PANEL_ROW0(batch_dest), batch_rows);
    batch_rows = 0;
    machine.row_out = line_buffers[slot][0];
}
static void row(void *, unsigned y, uint16_t *) {
    if (!batch_rows) batch_dest = display_row(y);
    if (++batch_rows == SG_BATCH) flush_batch();
    else machine.row_out = line_buffers[rows.slot][batch_rows];
}
#endif
static void frame(void *) {
#if !M0_TARGET_POKITTO
    flush_batch();
#endif
    m0_audio_frame();
    m0_lcd_end_frame();
    uint8_t keys = 0, pause = 0;
    const uint8_t mb = M0_PAD(SG_PAD_RAW());
    /* the Sega pad: 1 up, 2 down, 4 left, 8 right, 16 button 1, 32 button 2 */
    keys = ((mb & M0_MB_UP) ? 1 : 0) | ((mb & M0_MB_DOWN) ? 2 : 0) | ((mb & M0_MB_LEFT) ? 4 : 0) |
           ((mb & M0_MB_RIGHT) ? 8 : 0) | (mb & (M0_MB_A | M0_MB_B));
#if M0_TARGET_POKITTO
    if (pause_frames) { pause = 1; pause_frames--; }
#else
    pause = mb & M0_MB_MENU;
    SG_PAD_WDT();
#endif
#if M0_TARGET_ESPBOY && SG_CART_RAM
    espboy_ram_tick(cart_ram, SG_CART_RAM_SIZE, machine.frames);
#endif
    sg1000_buttons(0, keys);
    sg1000_pause(pause);
    sg_menu_frame();
    sg_menu_poll(mb);
    sg_menu_tick();
    SG_PACE_FRAME(deadline, skip_video, skipped_run, SG_MAX_SKIP);
}
#if M0_TARGET_ESPBOY
int espboy_main() {
#elif M0_TARGET_ESP32
extern "C" int m0boy_console_main(void); /* the console file's setup() */
int m0boy_console_main() {
#else
int main() {
#endif
    SG_STACK_PAINT();
#ifdef M0_ROM_EXTERNAL
    if (!(sg_rom = m0_romx_load(SG_ROM_EXPECT_SIZE, SG_ROM_EXPECT_CRC32, SG_ROM_NAME))) return 2; /* the cartridge, from its file */
#endif
    SG_PLATFORM_INIT();
#if defined(GG_STATE_NAME83)
    m0_state_init(sg_card_open(), SG_MENU_STATE_BUF());
#endif
    sg_start();
#if M0_TARGET_ESPBOY && defined(GG_STATE_SLOTS)
    m0_state_init(1, SG_MENU_STATE_BUF()); /* the slots follow the cartridge RAM's copies */
#endif
    SG_STACK_GUARD();
#ifdef SG_SELFTEST
    sg_selftest_result = sg1000_render_selftest() + 1; /* read back by a debugger or gm0 */
    for (;;) {}
#endif
    m0_audio_init();
    deadline = m0_micros();
    /* Keep an inspectable source/target version stamp in the firmware. */
    __asm__ volatile("" : : "r"(version));
    for (;;) sg1000_run();
}
