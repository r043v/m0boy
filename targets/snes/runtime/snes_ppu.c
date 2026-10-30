/* Initial SNES PPU: VRAM/CGRAM/OAM ports, DMA, backgrounds and objects.
 * The renderer samples the registers per line — the frame's rows keep the
 * registers as the code left them, so a write during a line's HBlank
 * reaches the next line (writes inside the visible part too: the exact
 * mid-scanline change points are not modelled).
 *
 * The tile modes 0-4 render with their real depths: the offset-per-tile
 * maps of modes 2 and 4, and the direct color of modes 3/4.  Mode 7
 * renders its affine matrix (no EXBG).  Not modelled: modes 5/6, windows,
 * mosaic, color math, the sub screen, interlace, and the mode 7 line-count
 * quirk of the Y coordinate. */
#include "snes.h"

static unsigned vram_word(void) {
    unsigned a = machine.vmaddr;
    switch ((machine.ppu_regs[0x15] >> 2) & 3) {
    case 1: return (a & 0xff00) | ((a & 0x1f) << 3) | ((a >> 5) & 7);
    case 2: return (a & 0xfe00) | ((a & 0x3f) << 3) | ((a >> 6) & 7);
    case 3: return (a & 0xfc00) | ((a & 0x7f) << 3) | ((a >> 7) & 7);
    default: return a;
    }
}
static void vram_inc(unsigned high) {
    static const unsigned step[4] = {1, 32, 128, 128};
    if (((machine.ppu_regs[0x15] >> 7) & 1) == high)
        machine.vmaddr += step[machine.ppu_regs[0x15] & 3];
}

static unsigned oam_offset(unsigned addr) {
    addr &= 0x3ff;
    return addr < 0x200 ? addr : 0x200 + (addr & 31);
}

void snes_ppu_write(uint16_t at, uint8_t v) {
    unsigned r = at & 0xff;
    if (r >= 0x40) return;
    machine.ppu_regs[r] = v;
    switch (r) {
    case 0x02: case 0x03:
        machine.oamaddr = ((machine.ppu_regs[2] | ((unsigned)machine.ppu_regs[3] & 1) << 8) << 1) & 0x3ff;
        break;
    case 0x04: {
        unsigned a = machine.oamaddr;
        if (!(a & 1)) machine.oam_latch = v;
        if (a & 0x200) machine.oam[oam_offset(a)] = v;
        else if (a & 1) {
            machine.oam[oam_offset(a - 1)] = machine.oam_latch;
            machine.oam[oam_offset(a)] = v;
        }
        machine.oamaddr = (a + 1) & 0x3ff;
        break;
    }
    case 0x0d: case 0x0e: case 0x0f: case 0x10:
    case 0x11: case 0x12: case 0x13: case 0x14:
        machine.scroll[r - 0x0d] = ((unsigned)v << 8 | machine.scroll_latch) & 0x1fff;
        machine.scroll_latch = v;
        break;
    case 0x1b: case 0x1c: case 0x1d: case 0x1e:
    case 0x1f: case 0x20: /* the mode 7 parameters, two writes each */
        if (!machine.m7_phase) {
            machine.m7[(r - 0x1b) * 2] = v;
            machine.m7_phase = 1;
        } else {
            machine.m7[(r - 0x1b) * 2 + 1] = v;
            machine.m7_phase = 0;
        }
        break;
    case 0x16: machine.vmaddr = (machine.vmaddr & 0xff00) | v; break;
    case 0x17: machine.vmaddr = (machine.vmaddr & 0x00ff) | (unsigned)v << 8; break;
    case 0x18: case 0x19:
        machine.vram[((vram_word() << 1) + (r & 1)) & 0xffff] = v;
        vram_inc(r & 1);
        break;
    case 0x21: machine.cgaddr = (unsigned)v << 1; break;
    case 0x32: /* COLDATA: the fixed color, one channel per write */
        if (v & 0x20) machine.fixed_color = (uint16_t)((machine.fixed_color & ~31u) | (v & 31));
        if (v & 0x40) machine.fixed_color = (uint16_t)((machine.fixed_color & ~0x3e0u) | ((v & 31) << 5));
        if (v & 0x80) machine.fixed_color = (uint16_t)((machine.fixed_color & ~0x7c00u) | ((v & 31) << 10));
        break;
    case 0x22:
        if (!(machine.cgaddr & 1)) machine.cg_latch = v;
        else {
            machine.cgram[(machine.cgaddr - 1) & 511] = machine.cg_latch;
            machine.cgram[machine.cgaddr & 511] = v & 0x7f;
        }
        machine.cgaddr = (machine.cgaddr + 1) & 511;
        break;
    }
}

uint8_t snes_ppu_read(uint16_t at) {
    unsigned r = at & 0xff;
    if (r == 0x38) {
        uint8_t v = machine.oam[oam_offset(machine.oamaddr)];
        machine.oamaddr = (machine.oamaddr + 1) & 0x3ff;
        return v;
    }
    if (r == 0x39 || r == 0x3a) {
        unsigned high = r == 0x3a;
        uint8_t v = machine.vram[((vram_word() << 1) + high) & 0xffff];
        vram_inc(high);
        return v;
    }
    if (r == 0x3b) {
        uint8_t v = machine.cgram[machine.cgaddr & 511];
        machine.cgaddr = (machine.cgaddr + 1) & 511;
        return v;
    }
    if (r == 0x3f) /* STAT77: the last evaluated line's sprite flags */
        return (uint8_t)((machine.obj_time_over ? 0x40 : 0) | (machine.obj_range_over ? 0x20 : 0)) |
               (machine.cpu.mdr & 0x1f);
    return machine.cpu.mdr;
}

/* the transfer modes' B-side register offsets and unit sizes, shared by
 * the general DMA and the HDMA */
static const uint8_t dma_offsets[8][4] = {
    {0,0,0,0}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}
};
static const uint8_t dma_periods[8] = {1,2,2,4,4,4,2,4};

/* The general DMA: the transfers run at once from the $420B write, and the
 * CPU pays their bus time — a model of 16 cycles of channel start-up then 8
 * cycles per byte (the full-speed transfer), accounted into dma_stolen and
 * taken off the CPU slices at their next boundary.  The model's shape is an
 * estimate: no SNES has measured it here. */
void snes_dma_start(uint8_t mask) {
    for (unsigned ch = 0; ch < 8; ch++) if (mask & (1u << ch)) {
        uint8_t *d = machine.dma[ch];
        unsigned mode = d[0] & 7, addr = d[2] | d[3] << 8;
        unsigned n = d[5] | d[6] << 8;
        if (!n) n = 65536;
        machine.dma_stolen += 16 + 8u * n;
        for (unsigned i = 0; i < n; i++) {
            uint32_t a = (uint32_t)d[4] << 16 | addr;
            uint32_t b = 0x2100u | (uint8_t)(d[1] + dma_offsets[mode][i % dma_periods[mode]]);
            if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(b));
            else snes_bus_write(b, snes_bus_read(a));
            if (!(d[0] & 8)) addr = (addr + ((d[0] & 0x10) ? -1 : 1)) & 0xffff;
        }
        d[2] = addr; d[3] = addr >> 8; d[5] = d[6] = 0;
    }
}

/* The HDMA: one unit per line, run at the line's H-Blank, its bus time
 * paid like the general DMA's (8 cycles a byte, 16 the table read, 16 more
 * an indirect pointer).  Each enabled channel walks its table at
 * A1Bx/A2Ax: a 0 count byte ends the channel for the frame, bit 7 of the
 * count byte repeats the entry's unit every line for count lines, and
 * 01h-7Fh transfer one unit then pause count-1 lines.  Indirect mode
 * (DMAPx bit 6) loads DASx from a 16-bit table pointer per entry, the
 * data bank being DASBx; the data address always increments. */
void snes_hdma_line(void) {
    for (unsigned ch = 0; ch < 8; ch++) {
        if (!(machine.hdma_enable & (1u << ch)) || machine.hdma_done[ch]) continue;
        uint8_t *d = machine.dma[ch];
        if (!machine.hdma_run[ch]) {
            machine.hdma_run[ch] = 1; /* started this frame, possibly mid-frame */
            machine.hdma_count[ch] = 0;
            d[8] = d[2]; d[9] = d[3]; /* the table pointer reloads */
        }
        const unsigned mode = d[0] & 7;
        const uint16_t period = dma_periods[mode];
        if (!machine.hdma_count[ch]) {
            uint16_t table = d[8] | d[9] << 8;
            const uint32_t at = (uint32_t)d[4] << 16 | table;
            const uint8_t b = snes_bus_read(at);
            machine.dma_stolen += 8;
            table++;
            if (d[0] & 0x40) {
                const uint16_t p = (uint16_t)(snes_bus_read((uint32_t)d[4] << 16 | table) |
                                             snes_bus_read((uint32_t)d[4] << 16 | (uint16_t)(table + 1)) << 8);
                machine.dma_stolen += 16;
                table += 2;
                d[5] = (uint8_t)p; d[6] = (uint8_t)(p >> 8); /* DASx: the data address */
            }
            d[8] = (uint8_t)table; d[9] = (uint8_t)(table >> 8);
            if (!b) {
                machine.hdma_done[ch] = 1;
                machine.hdma_run[ch] = 0;
                continue;
            }
            machine.hdma_len[ch] = b & 0x7F;
            machine.hdma_count[ch] = b & 0x7F;
            machine.hdma_rep[ch] = b >> 7;
        }
        if ((machine.hdma_rep[ch] && machine.hdma_count[ch]) ||
            (!machine.hdma_rep[ch] && machine.hdma_count[ch] == machine.hdma_len[ch])) {
            machine.dma_stolen += 8u * period;
            for (unsigned j = 0; j < period; j++) {
                const uint32_t bport = 0x2100u | (uint8_t)(d[1] + dma_offsets[mode][j]);
                if (d[0] & 0x40) { /* indirect: the data at DASBx/DASx */
                    const uint32_t a = (uint32_t)d[7] << 16 | d[5] | d[6] << 8;
                    if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(bport));
                    else snes_bus_write(bport, snes_bus_read(a));
                    const uint16_t da = (uint16_t)(d[5] | d[6] << 8) + 1;
                    d[5] = (uint8_t)da; d[6] = (uint8_t)(da >> 8);
                } else { /* direct: the table itself is the data */
                    const uint32_t a = (uint32_t)d[4] << 16 | d[8] | d[9] << 8;
                    if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(bport));
                    else snes_bus_write(bport, snes_bus_read(a));
                    const uint16_t ta = (uint16_t)(d[8] | d[9] << 8) + 1;
                    d[8] = (uint8_t)ta; d[9] = (uint8_t)(ta >> 8);
                }
            }
        }
        if (machine.hdma_count[ch]) machine.hdma_count[ch]--;
    }
}

static uint16_t vword(unsigned a) {
    a &= 0xffff;
    return machine.vram[a] | (unsigned)machine.vram[(a + 1) & 0xffff] << 8;
}

/* Object pixels are selected in OAM order before their priority is
 * compared to backgrounds: 32 sprites at most per line, 34 8-pixel tile
 * slots (a wide sprite spends several), the extras setting the flags of
 * $213F and staying invisible.  The vertical range counts even for a
 * sprite whose X sits off screen, as the hardware's evaluation does. */
static void object_line(unsigned y, uint8_t color[256], uint8_t prio[256]) {
    static const uint8_t width[8][2] = {
        {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}
    };
    static const uint8_t height[8][2] = {
        {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{32,64},{32,32}
    };
    const uint8_t obsel = machine.ppu_regs[1];
    const unsigned size_mode = obsel >> 5;
    const unsigned first = (machine.ppu_regs[3] & 0x80) ? (machine.ppu_regs[2] >> 1) : 0;
    unsigned selected[32], ns = 0, tiles = 0;
    machine.obj_range_over = 0;
    machine.obj_time_over = 0;
    for (unsigned n = 0; n < 128; n++) {
        const unsigned i = (first + n) & 127;
        const uint8_t *o = machine.oam + i * 4;
        const unsigned high = (machine.oam[512 + i / 4] >> ((i & 3) * 2)) & 3;
        const unsigned h = height[size_mode][high >> 1];
        const unsigned py = (y - o[1]) & 255;
        if (py >= h) continue;
        if (ns == 32) {
            machine.obj_range_over = 1;
            break;
        }
        tiles += width[size_mode][high >> 1] / 8;
        if (tiles > 34) {
            machine.obj_time_over = 1;
            break;
        }
        selected[ns++] = i;
    }
    /* the lowest OAM order paints last, as the hardware's priority */
    for (int n = (int)ns - 1; n >= 0; n--) {
        const unsigned i = selected[n];
        const uint8_t *o = machine.oam + i * 4;
        const unsigned high = (machine.oam[512 + i / 4] >> ((i & 3) * 2)) & 3;
        const unsigned w = width[size_mode][high >> 1], h = height[size_mode][high >> 1];
        const int x0 = (int)o[0] - ((high & 1) ? 256 : 0);
        unsigned py = (y - o[1]) & 255;
        if (py >= h || x0 >= 256 || x0 + (int)w <= 0) continue;
        if (o[3] & 0x80) py = h - 1 - py;
        unsigned base = (obsel & 3) * 0x4000u;
        if (o[3] & 1) base += (((obsel >> 3) & 3) + 1) * 0x2000u;
        for (unsigned dx = 0; dx < w; dx++) {
            const int x = x0 + (int)dx;
            if (x < 0 || x >= 256) continue;
            const unsigned px = (o[3] & 0x40) ? w - 1 - dx : dx;
            const unsigned tile = ((o[2] + (py >> 3) * 16) & 0xf0) | ((o[2] + (px >> 3)) & 15);
            const unsigned row = base + tile * 32 + (py & 7) * 2, bit = 7 - (px & 7);
            unsigned c = 0;
            for (unsigned p = 0; p < 4; p++)
                c |= ((machine.vram[(row + (p / 2) * 16 + (p & 1)) & 0xffff] >> bit) & 1) << p;
            if (c) {
                color[x] = (uint8_t)(128 + ((o[3] >> 1) & 7) * 16 + c);
                prio[x] = (o[3] >> 4) & 3;
            }
        }
    }
}

static unsigned bg_priority(unsigned mode, unsigned bg, unsigned high, unsigned bg3_priority) {
    static const uint8_t mode0[4][2] = {{8,11},{7,10},{2,5},{1,4}};
    static const uint8_t mode1[3][2] = {{7,10},{6,9},{2,4}};
    if (mode == 0) return mode0[bg][high];
    if (bg == 2 && high && bg3_priority) return 12;
    return mode1[bg][high];
}

/* the tile-based modes' bits per pixel, BG1 to BG4 */
static const uint8_t mode_bpp[5][4] = {
    {2,2,2,2}, /* 0 */
    {4,4,2,0}, /* 1 */
    {4,4,0,0}, /* 2: offset-per-tile */
    {8,4,0,0}, /* 3: BG1 direct color capable */
    {8,2,0,0}, /* 4: offset-per-tile */
};

/* One tile-mode background's pixel: the BGR555 color (0 = transparent)
 * and its priority.  Modes 2 and 4 add the offset-per-tile map one block
 * past the screen base: its entry's low byte shifts the horizontal
 * scroll, mode 2's high byte the vertical, mode 4's bit 15 flips the
 * tile.  Mode 3/4's BG1 honors the direct color bit of $2105. */
static void tile_pixel(const uint8_t *r, unsigned mode, unsigned bg, unsigned x, unsigned y,
                       unsigned *out_color, unsigned *out_prio) {
    const unsigned bpp = mode_bpp[mode][bg];
    if (!bpp) return;
    const unsigned size = (r[5] & (0x10 << bg)) ? 16 : 8;
    unsigned hscroll = machine.scroll[bg * 2], vscroll = machine.scroll[bg * 2 + 1];
    unsigned opt_h = 0, opt_v = 0, opt_flip = 0;
    if (mode == 2 || mode == 4) {
        const unsigned opt = (r[7 + bg] & 0xfc) * 0x200u + 0x400u;
        const unsigned tx = ((x + hscroll) & 0x3ff) / size, ty = ((y + vscroll) & 0x3ff) / size;
        const uint16_t e = vword(opt + ((ty & 31) * 32 + (tx & 31)) * 2);
        opt_h = e & 0xff;
        if (mode == 2) opt_v = e >> 8;
        else opt_flip = e >> 15;
    }
    const unsigned sx = (x + hscroll + opt_h) & 0x3ff;
    const unsigned sy = (y + vscroll + opt_v) & 0x3ff;
    unsigned tx = sx / size, ty = sy / size;
    unsigned screen = r[7 + bg] & 3;
    unsigned page_x = (tx >> 5) & (screen & 1);
    unsigned page_y = (ty >> 5) & ((screen >> 1) & 1);
    unsigned page = page_x + page_y * ((screen & 1) ? 2 : 1);
    unsigned map = (r[7 + bg] & 0xfc) * 0x200u + page * 0x800u;
    uint16_t entry = vword(map + ((ty & 31) * 32 + (tx & 31)) * 2);
    unsigned px = sx % size, py = sy % size;
    if (entry & 0x4000) px = size - 1 - px;
    if (entry & 0x8000) py = size - 1 - py;
    if (opt_flip) py = size - 1 - py;
    unsigned tile = (entry & 0x3ff) + (px >> 3) + (py >> 3) * 16;
    unsigned base = ((r[0x0b + bg / 2] >> ((bg & 1) * 4)) & 15) * 0x2000u;
    unsigned row = base + tile * (bpp * 8) + (py & 7) * 2;
    unsigned bit = 7 - (px & 7), index = 0;
    for (unsigned p = 0; p < bpp; p++)
        index |= ((machine.vram[(row + (p / 2) * 16 + (p & 1)) & 0xffff] >> bit) & 1) << p;
    if (!index) return;
    *out_prio = bg_priority(mode, bg, (entry >> 13) & 1, r[5] & 8);
    if (bpp == 8) {
        if ((r[0x30] & 1) && (mode == 3 || mode == 4)) { /* CGWSEL direct color: RRRGGGBB */
            *out_color = ((index >> 5) << 10) | (((index >> 2) & 7) << 5) | (index & 3);
            return;
        }
        *out_color = machine.cgram[(index * 2) & 511] | machine.cgram[(index * 2 + 1) & 511] << 8;
        return;
    }
    const unsigned pal = mode == 0 ? bg * 32 : 0;
    const unsigned shift = bpp == 2 ? 2 : 4;
    const unsigned c = pal + ((entry >> 10) & 7) * (1u << bpp) + index;
    *out_color = machine.cgram[(c * 2) & 511] | machine.cgram[(c * 2 + 1) & 511] << 8;
    (void)shift;
}

/* Mode 7's BG1: the map's entries sit in the low bytes of the first 32 KiB,
 * the 8bpp tiles in the high bytes of the second (tile t, row r, pixel p at
 * $4000 + t*128 + r*16 + p*2 + 1).  The matrix is signed 8.8, the screen
 * coordinates flip per M7SEL bits 0-1, and bits 6-7 choose between the
 * 128x128 wrap, transparency and tile 0 outside.  BG2's EXBG is not
 * modelled, and the hardware's line-count quirk of the Y coordinate is
 * not reproduced. */
static void mode7_pixel(const uint8_t *r, unsigned x, unsigned y, unsigned *out_color, unsigned *out_prio) {
    const unsigned sel = r[0x1a];
    int cx = machine.m7[8] | (machine.m7[9] & 1) << 8;
    int cy = machine.m7[10] | (machine.m7[11] & 1) << 8;
    if (cx & 0x100) cx -= 0x200;
    if (cy & 0x100) cy -= 0x200;
    int px = (int)x, py = (int)y;
    if (sel & 1) px = 255 - px;
    if (sel & 2) py = 223 - py;
    const int dx = px + (int)machine.scroll[0] - cx;
    const int dy = py + (int)machine.scroll[1] - cy;
    const int a = (int16_t)(machine.m7[0] | machine.m7[1] << 8);
    const int b = (int16_t)(machine.m7[2] | machine.m7[3] << 8);
    const int c = (int16_t)(machine.m7[4] | machine.m7[5] << 8);
    const int d = (int16_t)(machine.m7[6] | machine.m7[7] << 8);
    int u = ((a * dx + b * dy) >> 8) + cx;
    int v = ((c * dx + d * dy) >> 8) + cy;
    const unsigned over = (sel >> 6) & 3;
    unsigned tile = 0;
    if (over >= 2 && ((u | v) & ~0x3ff)) { /* outside the 128x128 map */
        if (over == 2) return; /* transparent: the layers below show */
        u &= 0x3ff; v &= 0x3ff; /* tile 0 fills the outside */
    } else {
        u &= 0x3ff; v &= 0x3ff;
        tile = machine.vram[(v >> 3) * 256 + (u >> 3) * 2];
    }
    const unsigned col = machine.vram[0x4000u + tile * 128u + (v & 7) * 16u + (u & 7) * 2u + 1];
    if (!col) return;
    *out_color = machine.cgram[(col * 2) & 511] | machine.cgram[(col * 2 + 1) & 511] << 8;
    *out_prio = 7;
}

/* Where the windows leave a layer visible: layers 0-3 the BGs, 4 the
 * objects, 5 the color window the math regions use.  A disabled window
 * stays neutral (visible); the color window with no window enabled is
 * "outside" everywhere, per the register doc. */
static unsigned layer_window(const uint8_t *r, unsigned layer, unsigned x) {
    const uint8_t sel = r[0x23 + (layer >> 1)] >> ((layer & 1) * 4);
    const unsigned e1 = sel & 1, i1 = (sel >> 1) & 1, e2 = (sel >> 2) & 1, i2 = (sel >> 3) & 1;
    const unsigned op = ((layer < 4 ? r[0x2a] : r[0x2b]) >> ((layer & 3) * 2)) & 3;
    if (!e1 && !e2) return layer == 5 ? 0 : 1;
    const unsigned neutral = (op == 1 || op == 3);
    unsigned v1 = neutral, v2 = neutral;
    if (e1) { const unsigned in = x >= r[0x26] && x < r[0x27]; v1 = i1 ^ in; }
    if (e2) { const unsigned in = x >= r[0x28] && x < r[0x29]; v2 = i2 ^ in; }
    unsigned v;
    if (op == 0) v = v1 | v2;
    else if (op == 1) v = v1 & v2;
    else if (op == 2) v = v1 ^ v2;
    else v = (v1 ^ v2) ^ 1;
    return v;
}

/* One screen's row: each pixel's BGR555 color and the layer that painted
 * it (0-3 the BGs, 4 the objects, 5 the backdrop, 6 nothing — black).
 * The sub flag picks TM/TS and the sub windows; the sub screen exists for
 * the color math's addend (and one day the hires modes).  Mosaic samples
 * the mosaic-sized block's first pixel of the enabled BGs. */
static void compose_row(const uint8_t *r, unsigned mode, unsigned y, unsigned sub,
                        const uint8_t win[6][256], const uint8_t *obj_color,
                        const uint8_t *obj_prio, uint16_t *color_row, uint8_t *layer_row) {
    const unsigned tm = sub ? r[0x2d] : r[0x2c];
    const unsigned tw = sub ? r[0x2f] : r[0x2e];
    const unsigned mosa = r[6], msize = (mosa >> 4) + 1;
    static const uint8_t sprite_priority[2][4] = {{3,6,9,12}, {3,5,8,11}};
    for (unsigned x = 0; x < 256; x++) {
        unsigned color = 0, best = 0, layer = 6;
        if (!(r[0] & 0x80) && (mode <= 4 || mode == 7)) {
            const unsigned w4 = (tw & 0x10) ? win[4][x] : 1;
            if ((tm & 0x10) && obj_color[x] && w4) {
                best = sprite_priority[mode == 0 ? 0 : 1][obj_prio[x]];
                const unsigned oc = obj_color[x];
                color = machine.cgram[(oc * 2) & 511] | machine.cgram[(oc * 2 + 1) & 511] << 8;
                layer = 4;
            }
            if (mode == 7) {
                const unsigned mx = (mosa & 1) ? (x / msize) * msize : x;
                unsigned c7 = 0, p7 = 0;
                mode7_pixel(r, mx, y, &c7, &p7);
                if (c7 && p7 > best && ((tw & 1) ? win[0][x] : 1)) {
                    best = p7;
                    color = c7;
                    layer = 0;
                }
            } else if (mode <= 4) {
                for (int bg = 3; bg >= 0; bg--) {
                    if (!(tm & (1 << bg))) continue;
                    if ((tw & (1 << bg)) && !win[bg][x]) continue;
                    /* mosaic: the block's first pixel stands for the block */
                    const unsigned mx = (mosa & (1 << bg)) ? (x / msize) * msize : x;
                    unsigned c = 0, p = 0;
                    tile_pixel(r, mode, (unsigned)bg, mx, y, &c, &p);
                    if (c && p > best) {
                        best = p;
                        color = c;
                        layer = (unsigned)bg;
                    }
                }
            }
        }
        /* nothing painted: the backdrop, CGRAM's color 0 (also behind the
         * layers' transparent pixels: the screens compose over it) */
        if (layer == 6 && !(r[0] & 0x80)) {
            color = machine.cgram[0] | machine.cgram[1] << 8;
            layer = 5;
        }
        color_row[x] = (uint16_t)color;
        layer_row[x] = (uint8_t)layer;
    }
}

void snes_render_line(unsigned y, uint8_t *row) {
    const uint8_t *r = machine.ppu_regs;
    unsigned mode = r[5] & 7, bright = r[0] & 15;
    uint8_t obj_color[256] = {0}, obj_prio[256] = {0};
    if (!(r[0] & 0x80) && ((r[0x2c] | r[0x2d]) & 0x10) && (mode <= 4 || mode == 7))
        object_line(y, obj_color, obj_prio);
    uint8_t win[6][256];
    for (unsigned l = 0; l < 6; l++)
        for (unsigned x = 0; x < 256; x++)
            win[l][x] = (uint8_t)layer_window(r, l, x);
    uint16_t main_c[256], sub_c[256];
    uint8_t main_l[256], sub_l[256];
    compose_row(r, mode, y, 0, win, obj_color, obj_prio, main_c, main_l);
    compose_row(r, mode, y, 1, win, obj_color, obj_prio, sub_c, sub_l);
    /* the color math ($2130 ccmm--sd): the main pixel clips to black in
     * the color window's region (the spotlight's trick), math is prevented
     * in another region, and the painted main pixel adds or subtracts the
     * subscreen (or the fixed color), half if $2131.6 */
    const unsigned clip = (r[0x30] >> 6) & 3, prevent = (r[0x30] >> 4) & 3;
    const unsigned from_sub = r[0x30] & 2, half = r[0x31] & 0x40, op_sub = r[0x31] & 0x80;
    for (unsigned x = 0; x < 256; x++) {
        unsigned m = main_c[x], l = main_l[x];
        if (clip == 3 || (clip && (clip == 1 ? !win[5][x] : win[5][x]))) m = 0;
        const unsigned stopped =
            prevent == 3 || (prevent && (prevent == 1 ? !win[5][x] : win[5][x]));
        if (l < 6 && !stopped && (r[0x31] & (1u << l))) {
            /* the sprites' math reaches their palettes 4-7 only */
            if (l != 4 || obj_color[x] >= 0xc0) {
                unsigned a = from_sub ? sub_c[x] : machine.fixed_color;
                for (unsigned k = 0; k < 3; k++) {
                    unsigned av = (a >> (k * 5)) & 31, mv = (m >> (k * 5)) & 31;
                    if (half) av = (av >> 1) + (op_sub ? 1 : 0);
                    mv = op_sub ? (mv > av ? mv - av : 0) : (mv + av > 31 ? 31 : mv + av);
                    m = (m & ~(31u << (k * 5))) | (mv << (k * 5));
                }
            }
        }
        for (unsigned k = 0; k < 3; k++) {
            unsigned v = (m >> (k * 5)) & 31;
            *row++ = (uint8_t)((v * 255u * bright + 232u) / (31u * 15u));
        }
    }
}

void snes_render(uint8_t *rgb) {
    for (unsigned y = 0; y < 224; y++)
        snes_render_line(y, rgb + y * 256u * 3u);
}
