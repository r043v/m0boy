#include "md_full.h"
#include <stdint.h>
#include <string.h>
#if __has_include("md_config.h")
#include "md_config.h"
#endif
/* The ESP32 projects build the tile-column runs as Xtensa (md_video_asm.S,
 * two pixels per loaded pattern byte); everything else, and MD_VIDEO_ASM=0,
 * uses the C below.  md_span_selftest compares the two at boot. */
#ifndef MD_VIDEO_ASM
#define MD_VIDEO_ASM 0
#endif

#if MD_VIDEO_ASM
void md_span_run(uint8_t *dst, uint8_t *pri, const uint8_t *pat, unsigned pal, unsigned packed);
#endif

static uint16_t vram_word(const MdFull *m, unsigned a) {
    return (uint16_t)(m->vram[a & 0xffffu] << 8 | m->vram[(a + 1u) & 0xffffu]);
}
static int16_t vs_word(const uint8_t *vs, unsigned a) {
    a %= 80u;
    return (int16_t)(vs[a] << 8 | vs[(a + 1u) % 80u]);
}
static unsigned pattern_pixel(const MdFull *m, unsigned tile, unsigned x, unsigned y) {
    unsigned a = ((tile & 0x7ffu) * 32u + y * 4u + (x >> 1)) & 0xffffu;
    return (x & 1u) ? m->vram[a] & 15u : m->vram[a] >> 4;
}
/* Sprites of line y into idx/pri (0 = none), first in link order wins.
 * Per line at most 20 sprites and 320 pixels in H40 (16 and 256 in H32);
 * a sprite at x = 0 masks the later ones once a sprite with x != 0 is on the line. */
static void sprite_line(const MdFull *m, const uint16_t *reg, unsigned y, unsigned width,
                        uint8_t *idx, uint8_t *pri) {
    const unsigned base = (reg[5] & (width == 320u ? 0x7eu : 0x7fu)) << 9;
    const unsigned max_sprites = width == 320u ? 80u : 64u, line_sprites = width == 320u ? 20u : 16u;
    unsigned link = 0, found = 0, pixels = 0;
    int masked = 0, shown = 0;
    for (unsigned i = 0; i < max_sprites; ++i) {
        MD_PERF_ADD(m, p_sat);
        const unsigned a = base + link * 8u;
        const unsigned sy = vram_word(m, a) & 0x1ffu, shape = vram_word(m, a + 2u);
        const unsigned tile = vram_word(m, a + 4u), sx = vram_word(m, a + 6u) & 0x1ffu;
        const unsigned w = (((shape >> 10) & 3u) + 1u) * 8u, h = (((shape >> 8) & 3u) + 1u) * 8u;
        const int dy = (int)y - (int)sy + 128;
        if (dy >= 0 && (unsigned)dy < h) {
            if (++found > line_sprites) break;
            if (sx == 0u && shown) masked = 1;
            if (sx != 0u) shown = 1;
            for (unsigned dx = 0; dx < w && !masked; ++dx) {
                if (++pixels > width) break;
                const int x = (int)sx - 128 + (int)dx;
                if (x < 0 || x >= (int)width || idx[x]) continue;
                const unsigned col = tile & 0x0800u ? w - 1u - dx : dx;
                const unsigned row = tile & 0x1000u ? h - 1u - (unsigned)dy : (unsigned)dy;
                const unsigned pattern = (tile & 0x7ffu) + (col >> 3) * (h >> 3) + (row >> 3);
                const unsigned nibble = pattern_pixel(m, pattern, col & 7u, row & 7u);
                if (nibble) { idx[x] = (uint8_t)(((tile >> 9) & 0x30u) | nibble); pri[x] = !!(tile & 0x8000u); MD_PERF_ADD(m, p_sprite_px); }
            }
            if (masked || pixels > width) break;
        }
        link = shape & 0x7fu;
        if (!link) break;
    }
}

/* One run inside a tile column: `n` pixels of pattern row `row` of tile
 * `entry`, pattern columns from `col0` stepping in `dir` (1, or -1 when
 * the tile is H-flipped).  Writes the palette indices (0 = transparent)
 * and, when `pri` is set, the tile's priority bit under them.  The run is
 * split by the callers so the columns never wrap inside the tile. */
static void span_pixels_c(const MdFull *m, unsigned entry, unsigned row,
                          unsigned col0, int dir, unsigned n, uint8_t *dst, uint8_t *pri) {
    MD_PERF_ADD(m, p_span);
    const uint8_t *p = m->vram + (((entry & 0x7ffu) << 5) | (row << 2));
    const unsigned pal = (entry >> 9) & 0x30u;
    const uint8_t pr = (uint8_t)(entry >> 15);
    unsigned col = col0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned nib = (col & 1u) ? p[col >> 1] & 15u : p[col >> 1] >> 4;
        dst[i] = (uint8_t)(nib ? pal | nib : 0);
        if (pri) pri[i] = pr;
        col = dir > 0 ? (col + 1u) & 7u : (col - 1u) & 7u;
    }
}

#if MD_VIDEO_ASM
/* the Xtensa kernel of md_video_asm.S, same run (the callers only make
 * runs that stay inside the tile) */
static void span_pixels(const MdFull *m, unsigned entry, unsigned row,
                        unsigned col0, int dir, unsigned n, uint8_t *dst, uint8_t *pri) {
    MD_PERF_ADD(m, p_span);
    md_span_run(dst, pri, m->vram + (((entry & 0x7ffu) << 5) | (row << 2)),
                (entry >> 9) & 0x30u,
                (col0 & 7u) | (unsigned)(dir < 0) << 4 | (entry >> 10 & 0x20u) | n << 8);
}
#else
#define span_pixels span_pixels_c
#endif

#if MD_VIDEO_ASM
/* Deterministic runs through the C and the Xtensa paths; returns the
 * number of differing pixels (0 = the kernel matches). */
int md_span_selftest(MdFull *scratch) {
    uint8_t a[8], pa[8], b[8], pb[8];
    unsigned bad = 0;
    MdFull *const m = scratch; /* the machine is far too big for the task's stack */
    memset(m, 0, sizeof *m);
    for (unsigned seed = 0; seed < 64u; ++seed) {
        for (unsigned i = 0; i < 8; ++i) m->vram[8u * 32u + i] = (uint8_t)(seed * 37u + i * 29u);
        for (unsigned pal = 0; pal <= 0x30u; pal += 0x10u)
            for (unsigned pr = 0; pr <= 1u; ++pr)
                for (unsigned col = 0; col < 8u; ++col)
                    for (unsigned n = 1; n <= 8u; ++n)
                        for (unsigned dir = 0; dir <= 1u; ++dir) {
                            /* both run shapes the renderer may ask for */
                            if (dir == 0u && col + n > 8u) continue;
                            if (dir == 1u && n > col + 1u) continue;
                            const unsigned entry = 8u | (dir ? 0x0800u : 0u) |
                                pal << 9 | pr << 15;
                            span_pixels_c(m, entry, 0, col, dir ? -1 : 1, n, a, pa);
                            md_span_run(b, pb, m->vram + 8u * 32u, pal,
                                        col | dir << 4 | pr << 5 | n << 8);
                            for (unsigned i = 0; i < n; ++i)
                                if (a[i] != b[i] || pa[i] != pb[i]) ++bad;
                        }
    }
    return (int)bad;
}
#endif

/* The pattern row of `entry` that scanline `py` shows (V flip included). */
static unsigned entry_row(unsigned entry, unsigned py) {
    const unsigned row = py & 7u;
    return entry & 0x1000u ? 7u - row : row;
}
/* An H-flipped tile reads its pattern columns the other way. */
static int entry_dir(unsigned entry) {
    return entry & 0x0800u ? -1 : 1;
}
static unsigned entry_col0(unsigned entry, unsigned px) {
    return entry & 0x0800u ? 7u - (px & 7u) : px & 7u;
}

/* The line's palette indices, one per column.  Same layering as the pixel
 * reference this replaces (window over plane A where in_h or in_v holds,
 * B under A, sprites on top, high priority over low).  The registers and
 * VSRAM come in as parameters: the caller passes the machine's, or a
 * captured MdLineCtx's (the second-core render, one batch behind) — VRAM
 * is always read through `m`, live. */
static void render_indices(const MdFull *m, const uint16_t *reg, const uint8_t *vs,
                           unsigned y, uint8_t *idx) {
    const unsigned width = reg[12] & 1u ? 320u : 256u;
    const unsigned backdrop = reg[7] & 63u;
    if (!(reg[1] & 0x40u)) { memset(idx, (int)backdrop, 320); return; }

    uint8_t spr[320], spr_pri[320];
    memset(spr, 0, sizeof spr);
    memset(spr_pri, 0, sizeof spr_pri);
    sprite_line(m, reg, y, width, spr, spr_pri);

    /* both planes: base, cell grid, H scroll of this line's row, and the
     * V scroll (one value, or one per 16-pixel column band) */
    const unsigned scroll_mode = reg[11] & 3u;
    const unsigned hrow = scroll_mode == 3u ? y : scroll_mode == 2u ? y & ~7u : 0;
    const unsigned hbase = (reg[13] & 0x3fu) << 10;
    const int vs_columns = reg[11] & 4u;
    unsigned base[2], mask_x[2], mask_y[2], pw[2];
    int hs[2];
    int16_t vs_tab[2][20];
    for (unsigned p = 0; p < 2; ++p) {
        const unsigned wcode = reg[16] & 3u, hcode = (reg[16] >> 4) & 3u;
        pw[p] = wcode == 3u ? 128u : wcode == 1u ? 64u : 32u;
        const unsigned height = hcode == 3u ? 128u : hcode == 1u ? 64u : 32u;
        base[p] = p ? (reg[4] & 7u) << 13 : (reg[2] & 0x38u) << 10;
        mask_x[p] = pw[p] * 8u - 1u;
        mask_y[p] = height * 8u - 1u;
        hs[p] = (int)(int16_t)vram_word(m, hbase + hrow * 4u + p * 2u);
        for (unsigned b = 0; b < 20; ++b)
            vs_tab[p][b] = vs_columns ? vs_word(vs, b * 4u + p * 2u) : vs_word(vs, p * 2u);
    }

    /* window plane (registers 3, 17, 18): over A where in_h or in_v holds */
    const unsigned hreg = reg[17], vreg = reg[18];
    const unsigned hx = (hreg & 0x1fu) * 16u, vy = (vreg & 0x1fu) * 8u;
    const int window_line = vreg & 0x80u ? y >= vy : y < vy;
    const unsigned wbase = (reg[3] & (width == 320u ? 0x3cu : 0x3eu)) << 10;
    const unsigned wcells = width == 320u ? 64u : 32u;

    for (unsigned x0 = 0; x0 < 320u; x0 += 8u) {
        uint8_t a[8] = {0}, b[8] = {0}, a_pri[8] = {0}, b_pri[8] = {0};
        const unsigned band = x0 >> 4;
        /* plane B */
        {
            const unsigned px0 = (unsigned)(x0 - hs[1]) & mask_x[1];
            const unsigned py = (y + (unsigned)(int16_t)vs_tab[1][band]) & mask_y[1];
            const unsigned head = (8u - (px0 & 7u)) & 7u;
            const unsigned e = vram_word(m, base[1] + (((py >> 3) * pw[1] + (px0 >> 3)) << 1));
            span_pixels(m, e, entry_row(e, py), entry_col0(e, px0), entry_dir(e), head, b, b_pri);
            if (head < 8u) {
                const unsigned px1 = (px0 + head) & mask_x[1];
                const unsigned e1 = vram_word(m, base[1] + (((py >> 3) * pw[1] + (px1 >> 3)) << 1));
                span_pixels(m, e1, entry_row(e1, py), entry_col0(e1, px1), entry_dir(e1),
                            8u - head, b + head, b_pri + head);
            }
        }
        /* plane A, or the window over it */
        if (window_line || (hreg & 0x80u ? x0 >= hx : x0 < hx)) {
            const unsigned e = vram_word(m, wbase + (((y >> 3) * wcells + (x0 >> 3)) << 1));
            span_pixels(m, e, entry_row(e, y), entry_col0(e, x0), entry_dir(e), 8, a, a_pri);
        } else {
            const unsigned px0 = (unsigned)(x0 - hs[0]) & mask_x[0];
            const unsigned py = (y + (unsigned)(int16_t)vs_tab[0][band]) & mask_y[0];
            const unsigned head = (8u - (px0 & 7u)) & 7u;
            const unsigned e = vram_word(m, base[0] + (((py >> 3) * pw[0] + (px0 >> 3)) << 1));
            span_pixels(m, e, entry_row(e, py), entry_col0(e, px0), entry_dir(e), head, a, a_pri);
            if (head < 8u) {
                const unsigned px1 = (px0 + head) & mask_x[0];
                const unsigned e1 = vram_word(m, base[0] + (((py >> 3) * pw[0] + (px1 >> 3)) << 1));
                span_pixels(m, e1, entry_row(e1, py), entry_col0(e1, px1), entry_dir(e1),
                            8u - head, a + head, a_pri + head);
            }
        }
        for (unsigned i = 0; i < 8u; ++i) {
            const unsigned x = x0 + i;
            unsigned index = backdrop;
            if (x < width) {
                const unsigned s = spr[x], ps = spr_pri[x];
                if (b[i] && !b_pri[i]) index = b[i];
                if (a[i] && !a_pri[i]) index = a[i];
                if (s && !ps) index = s;
                if (b[i] && b_pri[i]) index = b[i];
                if (a[i] && a_pri[i]) index = a[i];
                if (s && ps) index = s;
            }
            idx[x] = (uint8_t)index;
        }
    }
}

void md_full_line_ctx(const MdFull *m, MdLineCtx *c) {
    memcpy(c->reg, m->vdp_reg, sizeof c->reg);
    memcpy(c->vsram, m->vsram, sizeof c->vsram);
    memcpy(c->cram, m->cram_565, sizeof c->cram);
}

void md_full_render_line(const MdFull *m, unsigned y, uint32_t *row) {
    uint8_t idx[320];
    render_indices(m, m->vdp_reg, m->vsram, y, idx);
    for (unsigned x = 0; x < 320; ++x) row[x] = m->cram_argb[idx[x]];
}

void md_full_render_line565(const MdFull *m, unsigned y, uint16_t *row) {
    uint8_t idx[320];
    render_indices(m, m->vdp_reg, m->vsram, y, idx);
    for (unsigned x = 0; x < 320; ++x) row[x] = m->cram_565[idx[x]];
}

void md_full_render_line565_ctx(const MdFull *m, const MdLineCtx *c, unsigned y, uint16_t *row) {
    uint8_t idx[320];
    render_indices(m, c->reg, c->vsram, y, idx);
    for (unsigned x = 0; x < 320; ++x) row[x] = c->cram[idx[x]];
}

void md_full_render(const MdFull *m, uint32_t *pixels, unsigned stride) {
    for (unsigned y = 0; y < 224; ++y) md_full_render_line(m, y, pixels + y * stride);
}
