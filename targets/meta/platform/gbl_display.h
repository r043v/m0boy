/*
 * META display pipeline: maps the DMG's 144 scanlines onto the ST7735's 128
 * rows, skips rows whose inputs did not change, and hands strips of rows to
 * the LCD driver.  Plain C with no Arduino dependency, so the host tests run
 * exactly this code against a simulated panel.
 *
 * Implements gbl_platform_line_check / line_begin / line_end.
 */
#ifndef META_DISPLAY_H
#define META_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "gbl_config.h" /* GBL_TARGET_PC gates the PC's native mode below */

enum {
    META_MODE_SMOOTH, /* every row is two DMG lines weighted by area (9 -> 8) */
    META_MODE_TILE,  /* drop line 7 of each tile row (gm0), keep 63 and 95 */
    META_MODE_BLEND, /* merge lines 8 and 9 of every nine, averaged colours */
    META_MODE_DARK,  /* same merge, darker shade wins */
    META_MODE_CROP,  /* no scaling: DMG lines 8..135 */
    META_MODE_SKIP,  /* drop the ninth line of every nine */
    META_MODE_TBLEND, /* default: TILE's rows, line 7 of each tile row merged into the next (gm0 ESPboy) */
#ifdef GBL_TARGET_PC
    META_MODE_NATIVE, /* the PC window: the DMG's own 144 lines, 160x144 (PC_ROWS) */
#endif
    META_MODE_COUNT
};

/* The menu's palettes: Vert (DMG) first and by default; when m0recomp
 * found the game in the Game Boy Color boot ROM's compatibility table it
 * emits GBL_PALETTE_GBC (the four 0xRRGGBB the boot ROM would have
 * loaded, through gbl_config.h) and that palette comes second.  The 12
 * generic boot-ROM palettes follow in the table (not in the menu): a
 * direction held at power-on, plus A and/or B, picks one
 * (gbl_palette_boot) — they are stored as the boot ROM's 30 base
 * palettes and (OBJ0, OBJ1, BG) triplets (GBL_GBC_BASE, GBL_GBC_COMBOS)
 * for the day sprites get palettes of their own. */
#ifdef GBL_PALETTE_GBC
#define META_PALETTE_COUNT 6
#else
#define META_PALETTE_COUNT 5
#endif
extern const uint32_t gbl_palettes[META_PALETTE_COUNT][4]; /* 0xRRGGBB, lightest first */
extern const char *const gbl_palette_names[META_PALETTE_COUNT];
extern uint16_t gbl_dmg_palette[4]; /* current palette, panel format (M0_COLOR) */
/* The boot ROM's palette for the buttons held at power-on (M0_MB_*
 * bits: a direction, plus A and/or B), or 0 when none — the four colours
 * come straight from GBL_GBC_BASE and GBL_GBC_COMBOS. */
const uint32_t *gbl_palette_boot(uint8_t buttons);
/* Palette of every mode but SMOOTH (compile-time tables, first palette). */
void gbl_display_set_palette(uint8_t index);
/* Any four colours, as the boot combos and the menu's palette choice
 * need; rebuilds the blend tables like set_palette. */
void gbl_display_set_palette_colors(const uint32_t pal[4]);
uint8_t gbl_display_palette(void);
/* The strip buffers, free while emulation is stopped (the option menu):
 * flushes and returns 160-pixel rows of scratch, usable once the display
 * is idle (m0_lcd_wait). */
uint16_t *gbl_display_scratch(unsigned *rows);

void gbl_display_set_mode(uint8_t mode); /* call at VBlank */
uint8_t gbl_display_mode(void);
void gbl_display_flush(void);            /* send the partial strip */

extern uint16_t gbl_display_lcd_waits;    /* renders that waited for a buffer */
extern uint16_t gbl_display_rows_skipped; /* rows left unchanged on screen */

#ifdef __cplusplus
}
#endif

#endif
