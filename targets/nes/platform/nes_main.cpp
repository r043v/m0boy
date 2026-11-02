/* NES frontend for the Gamebuino META and the Pokitto: the interpreted
 * cartridge (nes_cart.c) on the runtime in ../runtime, the consoles' LCD,
 * audio, clock and button drivers.
 *
 * The 256x240 picture is scaled to the panel by the PPU (ppu.h: the 5-in-8
 * box filter, 256 -> 160; lines blended in pairs, 240 -> 128, or on the
 * Pokitto 240 -> 176, its rows widened to 200 by pokitto_lcd_row_zoom as
 * the Sega frontends do).  Buttons: the d-pad, A = A, B = B, MENU = Start,
 * HOME = Select; HOME held half a second opens the option menu (sg_menu.h,
 * as in the other frontends).  Pokitto, as its Game Boy frontend: C tap =
 * Start, C + B = Select, C held half a second the menu. */
#include "nes_config.h"
#include "nes.h"
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
#include "pc_input.h"
#include <stdio.h>
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
#include "m0_pad.h"
/* the frame paced at 50 Hz: the shared constant is the SG-1000's NTSC
 * frame; the NES panel release follows the 50 Hz displays.  The emulation
 * itself stays NTSC (the frame's 29781 CPU cycles, the audio with it). */
#define SG_FRAME_US 20000u
#define SG_STALL_US (6u * 20000u)
#include "sg_frontend.h"
#include "m0_rows.h"
#if defined(NES_STATE_NAME83)
#include "m0_state.h"
#include "m0_sd.h"
#include "m0_fat.h"
#include "m0_card.h"
#endif
extern "C" {
extern const uint8_t nes_prg[];
#if !NES_CHR_RAM
extern const uint8_t nes_chr[];
#endif
}

#if M0_TARGET_PC
/* The session the way the harness replays it: M0_INPUT plays a script (the
 * --input dialect, names joined with '+'), M0_RECORD_INPUT writes one — a
 * line per frame the game receives buttons on, so the file replays with
 * no gaps.  A restart or a save-state load ends the file there: the
 * harness replays from power-on, what follows could not replay. */
static const char *const nes_pad_names[8] = {
    "a", "b", "select", "start", "up", "down", "left", "right"};
static pc_input_t nes_script;
static FILE *nes_record;
static void nes_record_stop(const char *why) {
    if (!nes_record) return;
    fprintf(stderr, "recording ends at frame %u (%s)\n", (unsigned)machine.frames, why);
    if (fclose(nes_record)) perror("M0_RECORD_INPUT");
    nes_record = NULL;
}
static void nes_record_at_exit(void) { nes_record_stop("exit"); }
#define NES_RECORD_INTERRUPT(why) nes_record_stop(why)
#else
#define NES_RECORD_INTERRUPT(why) do { } while (0)
#endif
/* one panel row at a time: the PPU writes it, the DMA sends it while the
 * next one is built (the Pokitto's CPU writes it at once: one buffer is
 * enough, the second is the menu's) */
static uint16_t line_buffer[2][160] __attribute__((aligned(4)));
#if !M0_TARGET_POKITTO
static m0_rows rows;
#endif
#if M0_TARGET_PC && defined(NES_PPU_REFERENCE)
/* the PC shows the reference renderer's own lines: the NES's 256x240 at
 * PC_COLS/PC_ROWS (nes_config.h), as the harness's PPM captures — the
 * panel plan and its box filter stay off.  screen pixel c of the fetched
 * stream lives at pixels[c + fine x] (the harness does the same). */
static uint16_t native_line[256];
static void native_row(void *host, unsigned line, const uint16_t *pixels) {
    (void)host;
    if (line < PC_ROWS) {
        for (unsigned x = 0; x < PC_COLS; x++) native_line[x] = pixels[x + machine.ppu.x];
        m0_lcd_send_rows(native_line, (uint8_t)line, 1);
    }
}
#endif
static uint32_t deadline;
#ifndef NES_MAX_SKIP
#define NES_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
static const char version[] __attribute__((used)) = "m0boy-nes-" M0_TARGET_NAME " " GBL_GEN_VERSION;

/* The PPU scales the picture itself (ppu.h: 240 lines -> the panel's
 * rows, the 5-in-8 box filter across), straight into the row being filled. */
#if !(M0_TARGET_PC && defined(NES_PPU_REFERENCE))
static void panel_row(void *host, unsigned r) {
    (void)host;
#if M0_TARGET_POKITTO
    m0_rows_zoom(line_buffer[0], r, r + 1); /* 200x176 from x = 10 */
#else
    machine.ppu.panel_out = line_buffer[m0_rows_send(&rows, line_buffer[rows.slot], r, 1)];
#endif
}
#endif

/* ---- option menu (sg_menu.h) ---- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows) {
#if M0_TARGET_PC && defined(NES_PPU_REFERENCE)
    *rows = 1; /* the bands slice to one native-width row */
    return native_line;
#else
    *rows = 2;
    return line_buffer[0];
#endif
}
#define NES_MENU_RESTART() do { \
        NES_RECORD_INTERRUPT("restart"); \
        nes_reset(); \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#if M0_TARGET_ESPBOY
#define NES_MENU_QUIT() do { espboy_restart(); } while (0)
#elif M0_TARGET_POKITTO
#define NES_MENU_QUIT() do { \
        m0_audio_stop(); \
        pokitto_enter_loader(); \
        m0_audio_init(); \
    } while (0)
#elif M0_TARGET_PC
#define NES_MENU_QUIT() do { exit(0); } while (0)
#elif M0_TARGET_ESP32
#define NES_MENU_QUIT() do { esp32_restart(); } while (0)
#else
#if META_USB_SERIAL
#define NES_MENU_USB_STOP() meta_usb_stop()
#else
#define NES_MENU_USB_STOP() do { } while (0)
#endif
#define NES_MENU_BOOTLOADER() do { \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        NES_MENU_USB_STOP(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
#define NES_MENU_QUIT() do { \
        m0_lcd_end_frame(); \
        m0_clock_set_lcd_spi(24); \
        m0_clock_set(48); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        NES_MENU_USB_STOP(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#endif
#define NES_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#define SG_MENU_RESTART() NES_MENU_RESTART()
#define SG_MENU_QUIT() NES_MENU_QUIT()
#define SG_MENU_RESUME() NES_MENU_RESUME()
#if defined(NES_STATE_NAME83)
#define SG_MENU_STATE_SLOTS NES_STATE_SLOTS
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffer)
#define SG_MENU_STATE_LOADED() do { \
        NES_RECORD_INTERRUPT("state load"); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
static int nes_card_open(void) {
    if (!m0_sd_init()) return 0;
#if M0_TARGET_META
    m0_fat_dir = m0_fat_game_dir(m0_card_sector, SG_MENU_STATE_BUF(), NES_BIN_NAME83, META_APP_START);
#endif
    return 1;
}
#endif
#ifdef NES_MENU_BOOTLOADER /* the META's USB bootloader */
#define SG_MENU_BOOTLOADER() NES_MENU_BOOTLOADER()
#endif
#if defined(M0_TARGET_META) && M0_TARGET_META && META_USB_SERIAL
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
#if M0_TARGET_POKITTO
static unsigned start_frames; /* a C tap becomes a Start press */
#define SG_MENU_TAP() do { start_frames = 6; } while (0)
#endif
#ifndef SG_MENU_TAP
#define SG_MENU_TAP() do { } while (0)
#endif
#include "sg_menu.h"

static void frame(void *) {
    m0_audio_frame();
    m0_lcd_end_frame();
    const uint8_t mb = M0_PAD(SG_PAD_RAW());
    uint8_t keys = ((mb & M0_MB_UP) ? NES_UP : 0) | ((mb & M0_MB_DOWN) ? NES_DOWN : 0) |
                   ((mb & M0_MB_LEFT) ? NES_LEFT : 0) | ((mb & M0_MB_RIGHT) ? NES_RIGHT : 0) |
                   ((mb & M0_MB_A) ? NES_A : 0) | ((mb & M0_MB_B) ? NES_B : 0);
#if M0_TARGET_POKITTO
    /* C (HOME) with B is Select, B itself then not pressed; sg_menu_poll
     * keeps the menu and the tap off while another key joins C */
    if ((mb & M0_MB_HOME) && (mb & M0_MB_B)) keys = (uint8_t)((keys & ~NES_B) | NES_SELECT);
    if (start_frames) { keys |= NES_START; start_frames--; }
#else
    keys |= ((mb & M0_MB_MENU) ? NES_START : 0) | ((mb & M0_MB_HOME) ? NES_SELECT : 0);
#endif
#if M0_TARGET_PC
    keys |= (uint8_t)pc_input_keys(&nes_script, machine.frames); /* the script's frame is this one */
#endif
    machine.input = keys;
#if M0_TARGET_PC
    pc_record_buttons(nes_record, machine.frames, keys, nes_pad_names, 8);
#endif
    SG_PAD_WDT();
    sg_menu_frame();
    sg_menu_poll(mb);
    sg_menu_tick();
    SG_PACE_FRAME(deadline, skip_video, skipped_run, NES_MAX_SKIP);
}

int main() {
    SG_PLATFORM_INIT();
#if NES_CHR_RAM
    nes_init(nes_prg, NES_PRG_SIZE, 0, 0, NES_VERTICAL_MIRROR); /* the game uploads its CHR */
#else
    nes_init(nes_prg, NES_PRG_SIZE, nes_chr, NES_CHR_SIZE, NES_VERTICAL_MIRROR);
#endif
#if M0_TARGET_PC && defined(NES_PPU_REFERENCE)
    machine.row = native_row;
#else
    machine.ppu.panel = panel_row;
    machine.ppu.panel_out = line_buffer[0];
#endif
#if M0_TARGET_POKITTO
    nes_ppu_set_rows(&machine.ppu, 176);
#endif
#if defined(NES_STATE_NAME83)
    m0_state_init(nes_card_open(), SG_MENU_STATE_BUF());
#endif
#if M0_TARGET_PC
    if (const char *path = getenv("M0_INPUT")) {
        if (!pc_input_load(&nes_script, path, nes_pad_names, 8)) return 1;
    }
    if (const char *path = getenv("M0_RECORD_INPUT")) {
        nes_record = fopen(path, "w");
        if (!nes_record) { perror(path); return 1; }
        fprintf(nes_record, "# NES input recording: <frame> <buttons>\n");
        atexit(nes_record_at_exit);
    }
    if (nes_script.count) { /* the first frame latches before the first pad read */
        machine.input = (uint8_t)pc_input_keys(&nes_script, 0);
        pc_record_buttons(nes_record, 0, machine.input, nes_pad_names, 8);
    }
#endif
    m0_audio_init();
    deadline = m0_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        machine.ppu.skip = (uint8_t)skip_video;
        nes_frame();
        frame(0);
    }
}
