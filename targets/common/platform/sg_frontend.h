/* Frontend pieces the SG-1000 and Game Gear builds share: console setup, the
 * pad read, frame pacing and the C stack guard.  The recompiler copies this
 * file next to sg_main.cpp / gg_main.cpp, which include it once sg1000.h
 * (or gg.h) and the consoles' drivers are in scope.
 *
 * These are macros rather than functions on purpose: the two firmwares then
 * see the statements they had before this file existed, so a refactor keeps
 * firmware.bin comparable with the previous sources. */
#ifndef SG_FRONTEND_H
#define SG_FRONTEND_H

extern "C" {
extern char end; /* end of .bss: the C stack grows down to it */
extern char __StackTop;
}

/* NTSC: 59736 T-states at 3.579545 MHz.  The CPU, the PSG and the sprite
 * status keep full precision whatever is skipped.  A frontend may give its
 * own frame before including this file (the NES paces at 50 Hz). */
#ifndef SG_FRAME_US
#define SG_FRAME_US 16688u
#endif
/* Behind by six frames (a stall): restart from now instead of running fast
 * to catch up; smaller debts are paid back.  Signed, so SG_PACE_FRAME's
 * comparison stays the signed one the frontends had. */
#ifndef SG_STALL_US
#define SG_STALL_US 100128
#endif

#if M0_TARGET_ESPBOY
/* ESPboy: the Arduino core has started the chip; the emulation runs in
 * setup() (espboy_hw.cpp). */
#define SG_PLATFORM_INIT() do { \
        m0_clock_init(); \
        m0_clock_set(M0_CLOCK_DEFAULT); \
        m0_lcd_init(); \
        m0_lcd_fill(0); \
    } while (0)
#elif M0_TARGET_PC
/* PC: pc_hw.c starts SDL in m0_lcd_init; main() is the frontend's own. */
#define SG_PLATFORM_INIT() do { \
        m0_clock_init(); \
        m0_lcd_init(); \
        m0_lcd_fill(0); \
    } while (0)
#elif M0_TARGET_ESP32
/* ESP32 consoles: the Arduino core has started the chip; the emulation
 * runs in setup() (the console's own file). */
#define SG_PLATFORM_INIT() do { \
        m0_clock_init(); \
        m0_clock_set(M0_CLOCK_DEFAULT); \
        m0_lcd_init(); \
        m0_lcd_fill(0); \
    } while (0)
#elif M0_TARGET_POKITTO
#define SG_PLATFORM_INIT() do { \
        pokitto_buttons_init(); \
        m0_clock_init(); \
        m0_lcd_init(); \
        m0_lcd_fill(0); \
    } while (0)
#else
/* USB serial port (META_USB, meta_usb.c): the upload tools' 1200-baud touch
 * resets into the bootloader at the next frame. */
#if META_USB_SERIAL
#include "meta_usb.h"
#define SG_USB_INIT() meta_usb_init()
#define SG_USB_POLL() do { \
        if (meta_usb_bootloader_request) { \
            m0_lcd_end_frame(); \
            m0_audio_stop(); \
            meta_reset_to_bootloader(); \
        } \
    } while (0)
#else
#define SG_USB_INIT() do { } while (0)
#define SG_USB_POLL() do { } while (0)
#endif
#define SG_PLATFORM_INIT() do { \
        SG_USB_INIT(); \
        meta_spi_init(); \
        m0_clock_init(); \
        if (M0_CLOCK_DEFAULT != 48) m0_clock_set(M0_CLOCK_DEFAULT); /* META_OVERCLOCK */ \
        m0_lcd_init(); \
        m0_lcd_fill(0); \
    } while (0)
#endif

/* Pad: the library's bit order on the META (META_BUTTONS_SPI_HZ), the
 * Pokitto's own bits through its driver; the META's watchdog is fed while
 * the pad is clocked. */
#if M0_TARGET_ESPBOY
/* M0_MB_* bits, the library's order: what the frontends decode */
#define SG_PAD_RAW() espboy_buttons()
#define SG_PAD_WDT() do { } while (0)
#elif M0_TARGET_PC
/* pc_buttons (pc_hw.h): M0_MB_* bits, SDL keyboard */
#define SG_PAD_RAW() pc_buttons()
#define SG_PAD_WDT() do { } while (0)
#elif M0_TARGET_ESP32
/* esp32_buttons (the console's file): M0_MB_* bits */
#define SG_PAD_RAW() esp32_buttons()
#define SG_PAD_WDT() do { } while (0)
#elif M0_TARGET_POKITTO
#define SG_PAD_RAW() pokitto_buttons()
#define SG_PAD_WDT() do { } while (0)
#else
#define SG_PAD_RAW() meta_pad_read()
#define SG_PAD_WDT() do { \
        m0_watchdog_feed(); \
        SG_USB_POLL(); \
    } while (0)
#endif

/* The option menu's "Vitesse" (sg_menu.h): no frame limit, the emulation
 * runs as fast as the console allows — video skipped but for one frame in
 * thirty, which is what makes it faster than the paced mode (drawing every
 * frame adds the LCD bus to every frame). */
static uint8_t sg_menu_fast, sg_fast_draw;

/* Frame pacing: the game itself keeps its speed, a presentation late by less
 * than a frame is skipped (at most max_skip in a row) and the deadline stays
 * put, so the debt is paid back. */
#define SG_PACE_FRAME(deadline, skip_video, skipped_run, max_skip) do { \
        if (sg_menu_fast) { \
            (deadline) = m0_micros(); \
            (skip_video) = ++sg_fast_draw < 30; \
            if (!(skip_video)) sg_fast_draw = 0; \
            (skipped_run) = 0; \
            break; \
        } \
        (deadline) += SG_FRAME_US; \
        (skip_video) = (int32_t)(m0_micros() - (deadline)) > 0 && (skipped_run) < (max_skip); \
        (skipped_run) = (skip_video) ? (skipped_run) + 1 : 0; \
        while ((int32_t)(m0_micros() - (deadline)) < 0) m0_audio_idle(); \
        if ((int32_t)(m0_micros() - (deadline)) > SG_STALL_US) (deadline) = m0_micros(); \
    } while (0)

/* Free RAM painted at boot, from the end of .bss to 128 bytes below main's
 * frame, as the Game Boy frontend does: the words never written since show
 * the deepest stack reached (read over USB, tools/meta-usb.py). */
#define SG_STACK_PAINT_WORD 0xC5AC5AC5u
#if M0_TARGET_ESPBOY
#define SG_STACK_PAINT() do { } while (0)
#define SG_STACK_GUARD() do { sg_stack_limit = espboy_stack_floor() + 2560; } while (0)
#elif M0_TARGET_PC
/* PC: the process stack is the machine's, not the console's; keep the
 * guard out of it by a wide margin (the count still works, the emulator
 * never hits it). */
#define SG_STACK_PAINT() do { } while (0)
#define SG_STACK_GUARD() do { \
        char here_; \
        sg_stack_limit = (uintptr_t)&here_ - 262144u; \
    } while (0)
#elif M0_TARGET_ESP32
/* The Arduino task's stack (grown by the project's build flags): the same
 * margin as the ESPboy's continuation stack. */
#define SG_STACK_PAINT() do { } while (0)
#define SG_STACK_GUARD() do { \
        char here_; \
        sg_stack_limit = (uintptr_t)&here_ - 2560u; \
    } while (0)
#else
#define SG_STACK_PAINT() do { \
        char here_; \
        uint32_t *p_ = (uint32_t *)(((uintptr_t)&end + 3u) & ~(uintptr_t)3u); \
        uint32_t *top_ = (uint32_t *)((uintptr_t)&here_ - 128u); \
        while (p_ < top_) *p_++ = SG_STACK_PAINT_WORD; \
    } while (0)

/* Calls the runtime makes from a region need stack below the guard (sync,
 * line rendering, audio catch-up, LCD/audio drivers and interrupts: about
 * 2 KiB, -fstack-usage); at least 1 KiB is left for nesting when RAM is
 * tight (cartridge RAM). */
#if defined(SG_MSX) && SG_MSX
/* MSX: 3.6 KiB of RAM is left (8 KiB of guest RAM and 16 KiB of VRAM).  Over a
 * ten-minute session the deepest the runtime calls went below the guard was
 * 672 bytes (stack paint, tools/meta-usb.py), so 1400 leaves room to spare and
 * doubles what regions may nest before they unwind to the dispatcher (an
 * unwound region runs interpreted until it returns). */
#define SG_STACK_RESERVE 1400
#else
#define SG_STACK_RESERVE 2560
#endif
#define SG_STACK_GUARD() do { \
        uintptr_t stack = (uintptr_t)&__StackTop - (uintptr_t)&end, reserve = SG_STACK_RESERVE; \
        if (stack < reserve + 1024) reserve = stack > 2048 ? stack - 1024 : stack / 2; \
        sg_stack_limit = (uintptr_t)&end + reserve; \
    } while (0)
#endif

#endif
