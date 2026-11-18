/*
 * Target configuration of the gbl frontend.
 *
 * The frontend sources in this directory are shared by the consoles the
 * gbl runtime runs on; this header is the one place that tells them which
 * console they are built for.  m0recomp writes GBL_TARGET_POKITTO into
 * gbl_config.h for --target pokitto; without it the build is for the
 * Gamebuino META.  Every console-dependent choice of the shared files is a
 * META_* switch below, tested with #if; the console's own drivers (display,
 * audio, SD card, clock, startup) live in separate files that only its
 * build compiles (platform/meta for the META, platform/pokitto for the
 * Pokitto), behind the same headers (meta_lcd.h, meta_display.h,
 * meta_audio.h, meta_sd.h, meta_clock.h).
 *
 *   Gamebuino META   SAMD21 (Cortex-M0+), 32 KiB RAM, 160x128 ST7735 on
 *                    SPI with DMA, bare metal (no Arduino core), loader
 *                    at 0x0000.
 *   Pokitto          LPC11U68 (Cortex-M0+), 32 + 2 + 2 KiB RAM, 220x176
 *                    ST7775 on a 16-bit parallel bus driven by GPIO, bare
 *                    metal (no mbed), loader at 0x39000.
 */
#ifndef META_TARGET_H
#define META_TARGET_H

#include "gbl_config.h"
#include "gbl_profile.h" /* the console's optimisation switches */

#if defined(GBL_TARGET_ESPBOY) && GBL_TARGET_ESPBOY
#define M0_TARGET_ESPBOY 1
#else
#define M0_TARGET_ESPBOY 0
#endif

/* The META is the default build (GBL_TARGET_POKITTO is written as 0 there):
 * its exclusive blocks (USB, the loader) test the flag's value. */
#if M0_TARGET_ESPBOY || GBL_TARGET_POKITTO || GBL_TARGET_PC || \
    GBL_TARGET_ODROID || GBL_TARGET_AKA
#define M0_TARGET_META 0
#else
#define M0_TARGET_META 1
#endif

#if defined(GBL_TARGET_PC) && GBL_TARGET_PC
#define M0_TARGET_PC 1
#else
#define M0_TARGET_PC 0
#endif

#if defined(GBL_TARGET_ODROID) && GBL_TARGET_ODROID
#define M0_TARGET_ODROID 1
#else
#define M0_TARGET_ODROID 0
#endif

#if defined(GBL_TARGET_AKA) && GBL_TARGET_AKA
#define M0_TARGET_AKA 1
#else
#define M0_TARGET_AKA 0
#endif

#if M0_TARGET_ODROID || M0_TARGET_AKA
#define M0_TARGET_ESP32 1
#else
#define M0_TARGET_ESP32 0
#endif

#if M0_TARGET_ESPBOY
#define M0_TARGET_POKITTO 0
#define M0_TARGET_NAME "espboy"
#define M0_LCD_ASYNC 0
#define M0_SCALED_MODES 1
#define M0_SCREEN_ROWS 128
#define M0_SWAP_AB_DEFAULT 0
#define M0_CLOCK_DEFAULT 160
/* Same byte-swapped RGB565 strips as the META; driver resamples 160->128. */
#define M0_COLOR(rgb) ((uint16_t)( \
    (((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) >> 8) | \
    (((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) << 8)))
#elif defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO

#define M0_TARGET_POKITTO 1
#define M0_TARGET_NAME "pokitto"
#define M0_LCD_ASYNC 0   /* the CPU writes every pixel: no transfer runs behind */
#define M0_SCALED_MODES 0 /* no 144->128 modes: 1:1, or gm0's 200x176 (pokitto_display.c) */
#define M0_SCREEN_ROWS 144 /* rows of the picture the menu draws on */
#define M0_SWAP_AB_DEFAULT 0 /* Pokitto A (right) is Game Boy A already */
/* LPC11U68 : 50 MHz max (datasheet), 48 MHz nominal (PokittoLib, SYSPLLCTRL
 * 0x23 : M = 4, P = 2).  META_OVERCLOCK (gbl_profile.h) démarre à 72 MHz
 * (_OSCT=2, hors spécification mais overclock standard de la console) ; le
 * menu « Processeur » permet la bascule 48 <-> 72 dans tous les cas. */
#define M0_CLOCK_DEFAULT (META_OVERCLOCK ? 72 : 48)
/* Panel colours: native RGB565 on the 16-bit bus. */
#define M0_COLOR(rgb) \
    ((uint16_t)((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)))

#elif defined(GBL_TARGET_PC) && GBL_TARGET_PC

#define M0_TARGET_POKITTO 0
#define M0_TARGET_PC 1
#define M0_TARGET_NAME "pc"
#define M0_LCD_ASYNC 0    /* rows land in a texture: nothing runs behind */
/* The Game Boy picture goes through the META's 144->128 modes unchanged
 * (gbl_display.c is built around them); the Sega frontends show their own
 * native resolution (PC_COLS/PC_ROWS, sg_config.h). */
#define M0_SCALED_MODES 1
#ifndef M0_SCREEN_ROWS /* the Sega frontends' own panel: 192 or 144 rows */
#define M0_SCREEN_ROWS 128
#endif
#define M0_SWAP_AB_DEFAULT 0 /* X is A on the keyboard already */
/* The window has no clock to set; one rate. */
#define M0_CLOCK_DEFAULT 240
#ifndef PC_COLS
#define PC_COLS 160
#endif
#ifndef PC_ROWS
#define PC_ROWS 128
#endif
/* Panel colours: native RGB565 (the texture conversion is the driver's). */
#define M0_COLOR(rgb) \
    ((uint16_t)((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)))

#elif M0_TARGET_ESP32

/* ODROID-GO (ESP32-WROVER, ILI9341 320x240, SD, 16 MiB flash, 4 MiB PSRAM)
 * and Gamebuino AKA (ESP32-S3, 320x240, SD, 8 MiB PSRAM): the shared ESP32
 * core (targets/common/consoles/esp32) plays the console, each with its own
 * pins and buttons.  The panel doubles the 160-column rows; the ODROID's
 * RGB565 is byte-swapped in memory like the META's strips, the AKA's native. */
#define M0_TARGET_POKITTO 0
#if M0_TARGET_ODROID
#define M0_TARGET_NAME "odroid"
#else
#define M0_TARGET_NAME "aka"
#endif
#define M0_LCD_ASYNC 0
#define M0_SCALED_MODES 1
#ifndef M0_SCREEN_ROWS /* the Sega frontends' own panel: 192 or 144 rows */
#define M0_SCREEN_ROWS 128
#endif
#define M0_SWAP_AB_DEFAULT 0 /* the console's A is the right-hand button */
#define M0_CLOCK_DEFAULT 240 /* 80 and 160 are menu choices */
#if M0_TARGET_AKA
/* Panel colours: native RGB565 — the AKA's i80 bus swaps each pixel's bytes
 * in hardware (swap_color_bytes, aka_hw.cpp). */
#define M0_COLOR(rgb) \
    ((uint16_t)((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)))
#else
/* Panel colours: RGB565 byte-swapped (the SPI sends the high byte first). */
#define M0_COLOR(rgb) ((uint16_t)( \
    (((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) >> 8) | \
    (((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) << 8)))
#endif

#else

#define M0_TARGET_POKITTO 0
#define M0_TARGET_NAME "meta"
#define M0_LCD_ASYNC 1   /* strips go out by DMA while emulation continues */
#define M0_SCALED_MODES 1 /* 144 lines onto 128: TBLEND, SMOOTH, ... */
#define M0_SCREEN_ROWS 128
#define M0_SWAP_AB_DEFAULT 1 /* the META's A is the right-hand button */
/* Where the loader copies the game's .BIN (gamebuino_meta.ld): its first
 * sector identifies the game's folder on the card (m0_fat_game_dir). */
#define META_APP_START ((const uint8_t *)0x4000u)
/* Debug: count what is still busy on the shared SPI bus when the pad or
 * the card takes it (meta_bus_violations, meta_lcd.cpp); off by default. */
#ifndef M0_DEBUG_BUS
#define M0_DEBUG_BUS 0
#endif
#ifdef __cplusplus
extern "C"
#endif
void m0_lcd_bus_check(void);
/* The SAMD21 starts at 48 MHz; META_OVERCLOCK asks for 72 once the clocks are
 * set up (meta_clock.cpp), past its rating. */
#define M0_CLOCK_DEFAULT (META_OVERCLOCK ? 72 : 48)
#if META_LCD_12BPP
/* Panel colours: RGB444 in the low 12 bits (0x0RGB); the ST7735 driver packs
 * two of them into three bytes as each row goes out (meta_lcd_pack.h). */
#define M0_COLOR(rgb) ((uint16_t)((((rgb) >> 12) & 0xF00u) | (((rgb) >> 8) & 0x0F0u) | (((rgb) >> 4) & 0x00Fu)))
#else
/* Panel colours: RGB565 byte-swapped (the SPI sends the high byte first);
 * the glass's BGR wiring is corrected by the MADCTL BGR bit of the LCD init. */
#define M0_COLOR(rgb)                                                                          \
    ((uint16_t)(((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) >> 8 | \
                ((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) << 8))
#endif

#endif

#endif
