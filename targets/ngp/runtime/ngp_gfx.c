/* The K2GE: the Neo Geo Pocket's video.  Two 32x32 scroll planes, 64
 * hardware sprites of 2bpp 8x8 tiles (chaining makes bigger ones), a
 * hardware window, per-pixel depth ordering, a 12-bit colour palette or the
 * four-tone grey of the monochrome pocket.  The line work follows the
 * lineage's model: settings land with a line's delay, the H-INT fires on
 * lines 0-150 and the last line, the V-INT moment is entering line 152. */
#ifndef M0_TL
#define M0_TL
#endif
#include "ngp.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { ZDEPTH_BACKGROUND_SCROLL = 3, ZDEPTH_FOREGROUND_SCROLL = 5 };

static M0_TL uint16_t frame[NGP_SCREEN_H][NGP_SCREEN_W];
static M0_TL uint8_t zbuffer[NGP_SCREEN_W];

/* the delayed copies of the registers (they land when a line starts) */
static M0_TL struct {
    uint8_t winx, winy, winw, winh;
    uint8_t scroll1x, scroll1y, scroll2x, scroll2y;
    uint8_t spx, spy;
    uint8_t plane_swap, bgc, oowc, negative;
} view;

const uint16_t *ngp_frame(void) {
    return &frame[0][0];
}

static uint16_t color12_to_rgb565(uint16_t c) {
    const unsigned r = (c & 0xF) * 17, g = ((c >> 4) & 0xF) * 17, b = ((c >> 8) & 0xF) * 17;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t palette_entry(unsigned byte_offset) {
    uint16_t c = (uint16_t)(machine.gfx.palette_ram[byte_offset & 0x1FF] |
                            (machine.gfx.palette_ram[(byte_offset + 1) & 0x1FF] << 8));
    if (view.negative) c = (uint16_t)(~c & 0xFFF);
    return color12_to_rgb565(c);
}

/* K1GE-compat mode on the K2GE: a pixel resolves in two hops -- a 3-bit
 * level out of the plane's LUT (the spplt/scrp1plt/scrp2plt registers),
 * then a 12-bit colour out of the plane's compat palette (sprite 0x8380,
 * scroll1 0x83A0, scroll2 0x83C0), indexed P*8 + level.  This is what
 * tints mono cartridges on a colour console; the BIOS fills the tables
 * with a grey ramp at boot. */
static uint16_t mono_compat(unsigned compat_base, unsigned pal_hi, unsigned level) {
    return palette_entry(compat_base + (pal_hi * 8 + level) * 2);
}

/* one 2bpp tile row (16 bits, pixel 0 = the top two bits) into the zbuffer */
static void draw_pattern(int scanline, uint8_t screenx, uint16_t tile, uint8_t row,
                         int mirror, unsigned palette_base, uint8_t pal, uint8_t depth) {
    uint16_t index;
    int left, right, xx;
    int x = screenx;
    if (x > 0xF8) x -= 256;
    if (x >= NGP_SCREEN_W) return;
    {
        const uint8_t *p = machine.gfx.character_ram + (tile & 0x1FF) * 16 + row * 2;
        index = (uint16_t)(p[0] | (p[1] << 8));
    }
    if (mirror) { /* reverse the 16 bits */
        uint16_t m = 0;
        for (unsigned b = 0; b < 16; b++)
            if (index & (1u << b)) m |= (uint16_t)(0x8000u >> b);
        index = m;
    }
    left = x > view.winx ? x : view.winx;
    right = x + 7;
    {
        const int highmark = (view.winw + view.winx < NGP_SCREEN_W ? view.winw + view.winx : NGP_SCREEN_W) - 1;
        if (right > highmark) {
            index >>= (unsigned)(right - highmark) * 2;
            right = highmark;
        }
    }
    for (xx = right; xx >= left; xx--, index >>= 2) {
        uint16_t c;
        const unsigned entry = palette_base + pal * 8 + (index & 3) * 2;
        if (depth <= zbuffer[xx] || (index & 3) == 0) continue;
        zbuffer[xx] = depth;
        c = palette_entry(entry);
        if (scanline < NGP_SCREEN_H) frame[scanline][xx] = c;
    }
}

/* one 2bpp tile row, mono mode; the LUT byte names a compat palette level */
static void draw_pattern_mono(int scanline, uint8_t screenx, uint16_t tile, uint8_t row,
                              int mirror, const uint8_t *lut, unsigned compat_base,
                              unsigned pal_hi, uint8_t depth) {
    uint16_t index;
    int left, right, xx;
    int x = screenx;
    if (x > 0xF8) x -= 256;
    if (x >= NGP_SCREEN_W) return;
    {
        const uint8_t *p = machine.gfx.character_ram + (tile & 0x1FF) * 16 + row * 2;
        index = (uint16_t)(p[0] | (p[1] << 8));
    }
    if (mirror) {
        uint16_t m = 0;
        for (unsigned b = 0; b < 16; b++)
            if (index & (1u << b)) m |= (uint16_t)(0x8000u >> b);
        index = m;
    }
    left = x > view.winx ? x : view.winx;
    right = x + 7;
    {
        const int highmark = (view.winw + view.winx < NGP_SCREEN_W ? view.winw + view.winx : NGP_SCREEN_W) - 1;
        if (right > highmark) {
            index >>= (unsigned)(right - highmark) * 2;
            right = highmark;
        }
    }
    for (xx = right; xx >= left; xx--, index >>= 2) {
        const unsigned px = index & 3; /* bits 1-0 = the row's rightmost pixel */
        if (depth <= zbuffer[xx] || px == 0) continue;
        zbuffer[xx] = depth;
        if (scanline < NGP_SCREEN_H) {
            const unsigned level = lut[(pal_hi ? 3 : 0) + px - 1] & 7;
            frame[scanline][xx] = mono_compat(compat_base, pal_hi, level);
        }
    }
}


static void draw_scroll(int scanline, int plane, uint8_t depth) {
    const uint8_t sx = plane == 1 ? view.scroll1x : view.scroll2x;
    const uint8_t sy = plane == 1 ? view.scroll1y : view.scroll2y;
    const uint8_t *map = machine.gfx.scroll_vram + (plane == 1 ? 0 : 0x800);
    const unsigned line = (unsigned)(scanline + sy);
    const uint8_t row = (uint8_t)(line & 7);
    for (unsigned i = 0; i < 32; i++) {
        const uint8_t *p = map + ((i + ((line >> 3) << 5)) & 0x3FF) * 2;
        const uint16_t data = (uint16_t)(p[0] | (p[1] << 8));
        if (machine.gfx.k2ge_mode)
            draw_pattern_mono(scanline, (uint8_t)(i * 8 - sx), data & 0x1FF,
                              (data & 0x4000) ? (uint8_t)(7 - row) : row, data & 0x8000,
                              plane == 1 ? machine.gfx.scrp1plt : machine.gfx.scrp2plt,
                              plane == 1 ? 0x1A0 : 0x1C0,
                              data & 0x2000, depth);
        else
            draw_pattern(scanline, (uint8_t)(i * 8 - sx), data & 0x1FF,
                         (data & 0x4000) ? (uint8_t)(7 - row) : row, data & 0x8000,
                         plane == 1 ? 0x80 : 0x100, (uint8_t)((data >> 9) & 0xF), depth);
    }
}

static void draw_sprites(int scanline) {
    int last_x = 0, last_y = 0;
    for (unsigned spr = 0; spr < 64; spr++) {
        const uint8_t *p = machine.gfx.sprite_vram + spr * 4;
        const uint16_t data = (uint16_t)(p[0] | (p[1] << 8));
        const uint8_t priority = (uint8_t)((data >> 11) & 3);
        int x = p[2], y = p[3];
        if (data & 0x0400) x = last_x + x; /* horizontal chain */
        if (data & 0x0200) y = last_y + y; /* vertical chain */
        last_x = x;
        last_y = y;
        if (!priority) continue;
        x += view.spx;
        y += view.spy;
        if (x > 248 && x < 256) x -= 256; else x &= 0xFF;
        if (y > 248 && y < 256) y -= 256; else y &= 0xFF;
        if (scanline >= y && scanline <= y + 7) {
            const uint8_t row = (uint8_t)((scanline - y) & 7);
            const uint8_t flipped = (data & 0x4000) ? (uint8_t)(7 - row) : row;
            if (machine.gfx.k2ge_mode)
                draw_pattern_mono(scanline, (uint8_t)x, data & 0x1FF, flipped, data & 0x8000,
                                  machine.gfx.spplt, 0x180, data & 0x2000, (uint8_t)(priority << 1));
            else
                draw_pattern(scanline, (uint8_t)x, data & 0x1FF, flipped, data & 0x8000,
                             0, machine.gfx.sprite_pal[spr] & 0xF, (uint8_t)(priority << 1));
        }
    }
}

static void draw_scanline(int scanline) {
    int x = 0;
    memset(zbuffer, 0, sizeof zbuffer);
    /* the window colour fills what the window does not cover (its 8-entry
     * palette at 0x83F0 serves both the colour and the compat mode) */
    {
        const uint16_t wcol = palette_entry(0x1F0 + view.oowc * 2);
        uint16_t *scan = frame[scanline];
        if (scanline >= view.winy && scanline < view.winy + view.winh) {
            for (; x < view.winx && x < NGP_SCREEN_W; x++) scan[x] = wcol;
            x = view.winw + view.winx < NGP_SCREEN_W ? view.winw + view.winx : NGP_SCREEN_W;
        }
        for (; x < NGP_SCREEN_W; x++) scan[x] = wcol;
    }
    if (scanline < view.winy || scanline >= view.winy + view.winh) return;
    /* the background colour across the window (BGC on = bit 7, bit 6 clear;
     * its 8-entry palette sits at 0x83E0) */
    {
        uint16_t bcol = 0;
        if ((view.bgc & 0xC0) == 0x80)
            bcol = palette_entry(0x1E0 + (view.bgc & 7) * 2);
        for (x = view.winx; x < (view.winw + view.winx < NGP_SCREEN_W ? view.winw + view.winx : NGP_SCREEN_W); x++)
            frame[scanline][x] = bcol;
    }
    /* the scroll planes: plane 2 behind, plane 1 in front (or swapped) */
    if (view.plane_swap) {
        draw_scroll(scanline, 1, ZDEPTH_BACKGROUND_SCROLL);
        draw_scroll(scanline, 2, ZDEPTH_FOREGROUND_SCROLL);
    } else {
        draw_scroll(scanline, 2, ZDEPTH_BACKGROUND_SCROLL);
        draw_scroll(scanline, 1, ZDEPTH_FOREGROUND_SCROLL);
    }
    draw_sprites(scanline);
}

static void delayed_settings(void) {
    view.winx = machine.gfx.wba_h;
    view.winy = machine.gfx.wba_v;
    view.winw = machine.gfx.wsi_h;
    view.winh = machine.gfx.wsi_v;
    view.scroll1x = machine.gfx.s1so_h;
    view.scroll1y = machine.gfx.s1so_v;
    view.scroll2x = machine.gfx.s2so_h;
    view.scroll2y = machine.gfx.s2so_v;
    view.spx = machine.gfx.po_h;
    view.spy = machine.gfx.po_v;
    view.plane_swap = machine.gfx.p_f & 0x80;
    view.bgc = machine.gfx.bg_col;
    view.oowc = machine.gfx.control_2d & 7;
    view.negative = machine.gfx.control_2d & 0x80;
}

/* the line work the tick loop calls once per 515 ticks: draws the line,
 * raises the H-INT (returned) and the V-INT at line 152 */
int ngp_gfx_line(void) {
    int h_int = 0;
    const uint16_t r = machine.gfx.raster_line;
    if (r < NGP_SCREEN_H - 1 || r == machine.gfx.screen_period) {
        delayed_settings();
        h_int = machine.gfx.control_int & 0x40;
    }
    if (r < NGP_SCREEN_H) draw_scanline(r);
    machine.gfx.raster_line++;
    if (machine.gfx.raster_line == NGP_SCREEN_H) {
        machine.gfx.blink = 1;
        machine.v_int = 1; /* the frame boundary, whatever the IRQ enable */
        if (machine.gfx.control_int & 0x80)
            ngp_test_int_hdma(NGP_INT_VBLANK, 0x0B);
    }
    if (machine.gfx.raster_line == (uint16_t)(machine.gfx.screen_period + 1)) {
        machine.gfx.raster_line = 0;
        machine.gfx.c_ovr = 0;
        machine.gfx.blink = 0;
    }
    return h_int;
}

/* ---- the register window ------------------------------------------------------- */

uint8_t ngp_gfx_read(uint32_t addr) {
    if (addr >= 0x9000 && addr < 0xA000) return machine.gfx.scroll_vram[addr - 0x9000];
    if (addr >= 0xA000 && addr < 0xC000) return machine.gfx.character_ram[addr - 0xA000];
    if (addr >= 0x8800 && addr < 0x8900) return machine.gfx.sprite_vram[addr - 0x8800];
    if (addr >= 0x8C00 && addr < 0x8C40) return machine.gfx.sprite_pal[addr - 0x8C00];
    if (addr >= 0x8200 && addr < 0x8400) return machine.gfx.palette_ram[addr - 0x8200];
    switch (addr) {
        case 0x8000: return machine.gfx.control_int;
        case 0x8002: return machine.gfx.wba_h;
        case 0x8003: return machine.gfx.wba_v;
        case 0x8004: return machine.gfx.wsi_h;
        case 0x8005: return machine.gfx.wsi_v;
        case 0x8006: return machine.gfx.screen_period;
        case 0x8008: /* RAS.H: the horizontal raster position, as modelled */
            return (uint8_t)(((NGP_LINE_TICKS - machine.timer_hint) & 0xFFFF) >> 2);
        case 0x8009: return (uint8_t)machine.gfx.raster_line;
        case 0x8010: return (uint8_t)((machine.gfx.c_ovr ? 0x80 : 0) | (machine.gfx.blink ? 0x40 : 0));
        case 0x8012: return machine.gfx.control_2d;
        case 0x8020: return machine.gfx.po_h;
        case 0x8021: return machine.gfx.po_v;
        case 0x8030: return machine.gfx.p_f;
        case 0x8032: return machine.gfx.s1so_h;
        case 0x8033: return machine.gfx.s1so_v;
        case 0x8034: return machine.gfx.s2so_h;
        case 0x8035: return machine.gfx.s2so_v;
        case 0x8101: return machine.gfx.spplt[0];
        case 0x8102: return machine.gfx.spplt[1];
        case 0x8103: return machine.gfx.spplt[2];
        case 0x8105: return machine.gfx.spplt[3];
        case 0x8106: return machine.gfx.spplt[4];
        case 0x8107: return machine.gfx.spplt[5];
        case 0x8109: return machine.gfx.scrp1plt[0];
        case 0x810A: return machine.gfx.scrp1plt[1];
        case 0x810B: return machine.gfx.scrp1plt[2];
        case 0x810D: return machine.gfx.scrp1plt[3];
        case 0x810E: return machine.gfx.scrp1plt[4];
        case 0x810F: return machine.gfx.scrp1plt[5];
        case 0x8111: return machine.gfx.scrp2plt[0];
        case 0x8112: return machine.gfx.scrp2plt[1];
        case 0x8113: return machine.gfx.scrp2plt[2];
        case 0x8115: return machine.gfx.scrp2plt[3];
        case 0x8116: return machine.gfx.scrp2plt[4];
        case 0x8117: return machine.gfx.scrp2plt[5];
        case 0x8118: return machine.gfx.bg_col;
        case 0x87E2: return machine.gfx.k2ge_mode;
        default: return 0;
    }
}

void ngp_gfx_write(uint32_t addr, uint8_t v) {
    if (addr >= 0x9000 && addr < 0xA000) { machine.gfx.scroll_vram[addr - 0x9000] = v; return; }
    if (addr >= 0xA000 && addr < 0xC000) { machine.gfx.character_ram[addr - 0xA000] = v; return; }
    if (addr >= 0x8800 && addr < 0x8900) { machine.gfx.sprite_vram[addr - 0x8800] = v; return; }
    if (addr >= 0x8C00 && addr < 0x8C40) { machine.gfx.sprite_pal[addr - 0x8C00] = v & 0x0F; return; }
    if (addr >= 0x8200 && addr < 0x8400) { machine.gfx.palette_ram[addr - 0x8200] = v; return; }
    switch (addr) {
        case 0x8000:
            if (getenv("NGP_DEBUG") && (v & 0xC0) != machine.gfx.control_int)
                fprintf(stderr, "[ngp] 0x8000 <- %02x (cyc=%u raster=%u)\n", v, machine.cyc, machine.gfx.raster_line);
            machine.gfx.control_int = v & 0xC0;
            return;
        case 0x8002: machine.gfx.wba_h = v; return;
        case 0x8003: machine.gfx.wba_v = v; return;
        case 0x8004: machine.gfx.wsi_h = v; return;
        case 0x8005: machine.gfx.wsi_v = v; return;
        case 0x8006: machine.gfx.screen_period = v; return;
        case 0x8012: machine.gfx.control_2d = v & 0x87; return;
        case 0x8020: machine.gfx.po_h = v; return;
        case 0x8021: machine.gfx.po_v = v; return;
        case 0x8030: machine.gfx.p_f = v & 0x80; return;
        case 0x8032: machine.gfx.s1so_h = v; return;
        case 0x8033: machine.gfx.s1so_v = v; return;
        case 0x8034: machine.gfx.s2so_h = v; return;
        case 0x8035: machine.gfx.s2so_v = v; return;
        case 0x8101: machine.gfx.spplt[0] = v & 7; return;
        case 0x8102: machine.gfx.spplt[1] = v & 7; return;
        case 0x8103: machine.gfx.spplt[2] = v & 7; return;
        case 0x8105: machine.gfx.spplt[3] = v & 7; return;
        case 0x8106: machine.gfx.spplt[4] = v & 7; return;
        case 0x8107: machine.gfx.spplt[5] = v & 7; return;
        case 0x8109: machine.gfx.scrp1plt[0] = v & 7; return;
        case 0x810A: machine.gfx.scrp1plt[1] = v & 7; return;
        case 0x810B: machine.gfx.scrp1plt[2] = v & 7; return;
        case 0x810D: machine.gfx.scrp1plt[3] = v & 7; return;
        case 0x810E: machine.gfx.scrp1plt[4] = v & 7; return;
        case 0x810F: machine.gfx.scrp1plt[5] = v & 7; return;
        case 0x8111: machine.gfx.scrp2plt[0] = v & 7; return;
        case 0x8112: machine.gfx.scrp2plt[1] = v & 7; return;
        case 0x8113: machine.gfx.scrp2plt[2] = v & 7; return;
        case 0x8115: machine.gfx.scrp2plt[3] = v & 7; return;
        case 0x8116: machine.gfx.scrp2plt[4] = v & 7; return;
        case 0x8117: machine.gfx.scrp2plt[5] = v & 7; return;
        case 0x8118: machine.gfx.bg_col = v & 0xC7; return;
        case 0x87E0:
            if (v == 0x52) ngp_reset(); /* the GE reset command */
            return;
        case 0x87E2:
            if (getenv("NGP_DEBUG_MODE"))
                fprintf(stderr, "[ngp] 0x87E2 <- %02x at pc=%06x frame=%u cyc=%u\n",
                        v, machine.cpu.pc, machine.frames, machine.cyc);
            machine.gfx.k2ge_mode = v & 0x80;
            return;
        default: return;
    }
}
