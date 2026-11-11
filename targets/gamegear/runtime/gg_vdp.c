/* Game Gear VDP (TMS9918 derivative, mode 4): ports, CRAM, line counter,
 * sprites and the 160x144 window, drawn 1:1.
 *
 * A tile row is four bit-plane bytes; one table lookup per plane spreads
 * its bits into eight 4-bit fields (gg_tables.h), so a row is decoded with
 * four loads, three shifts and three ORs, then each pixel is a field
 * extraction and a palette read (CRAM kept as panel RGB565). */
#include "gg.h"
#include <stdio.h>
#include <stdlib.h>
#include "gg_tables.h"
#include <string.h>

/* Panel byte order (GG_PANEL_SWAP, sg_config.h) and the shared filter
 * arithmetic: the 5-in-8 box, the fast method (below) in m0_scale.h. */
#define M0_PANEL_SWAP GG_PANEL_SWAP
#include "m0_scale.h"

/* ---- CRAM ----------------------------------------------------------------- */

/* Master System: the CRAM spread for the box filter (true RGB565), and the
 * blends of palette 0 pairs (below) */
static uint32_t spal[32];
static uint16_t pair53[2][256];
static uint8_t pairs_stale = 1;
/* Master System colour: 6 bits, --BBGGRR */
static void cram_colour_sms(unsigned i) {
    static const uint8_t c5[4] = { 0, 10, 21, 31 }, c6[4] = { 0, 21, 42, 63 };
    gg_t *m = &machine;
    const unsigned c = m->cram[i];
    const uint32_t rgb = (uint32_t)c5[c & 3] << 11 | (uint32_t)c6[(c >> 2) & 3] << 5 | c5[(c >> 4) & 3];
    m->pal[i] = m0_panel(rgb);
    spal[i] = m0_spread565(rgb);
    pairs_stale = 1;
}

/* Game Gear colour: 12 bits, ----BBBBGGGGRRRR (little endian pair) */
static void cram_colour(unsigned i) {
    gg_t *m = &machine;
    const unsigned w = m->cram[2 * i] | (m->cram[2 * i + 1] << 8);
    const unsigned r = w & 15, g = (w >> 4) & 15, b = (w >> 8) & 15;
    m->pal[i] = m0_panel(((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3));
}

/* ---- sprites -------------------------------------------------------------- */

/* Sprites covering each band of 8 lines (64 sprites: two words), kept up to
 * date on every Y write; terminator, size or table changes rebuild. */
#define GG_BANDS (GG_ACTIVE / 8)
static uint32_t band_mask[GG_BANDS][2];
static unsigned sat_end;

static inline int sprite_top(unsigned sy) { return sy >= 0xe0 ? (int)sy - 255 : (int)sy + 1; }
static inline unsigned sprite_span(void) {
    return ((machine.vdp[1] & 2) ? 16u : 8u) << (machine.vdp[1] & 1);
}
static void span_bits(unsigned sy, unsigned span, unsigned i, int set) {
    const int top = sprite_top(sy);
    int first = top < 0 ? 0 : top, last = top + (int)span;
    if (last > GG_ACTIVE) last = GG_ACTIVE;
    if (first >= last) return;
    const uint32_t bit = 1u << (i & 31);
    for (int b = first >> 3; b <= (last - 1) >> 3; b++) {
        if (set) band_mask[b][i >> 5] |= bit;
        else band_mask[b][i >> 5] &= ~bit;
    }
}
static void build_masks(void) {
    gg_t *m = &machine;
    memset(band_mask, 0, sizeof band_mask);
    const uint8_t *table = m->vram + m->sat_base;
    const unsigned span = sprite_span();
    unsigned i = 0;
    for (; i < 64 && table[i] != 0xd0; i++) span_bits(table[i], span, i, 1);
    sat_end = i;
    m->sprites_dirty = 0;
}
static void sprite_y(unsigned i, unsigned old, unsigned value) {
    gg_t *m = &machine;
    if (m->sprites_dirty || old == value) return;
    if (old == 0xd0 || value == 0xd0) { m->sprites_dirty = 1; return; }
    if (i >= sat_end) return;
    const unsigned span = sprite_span();
    span_bits(old, span, i, 0);
    span_bits(value, span, i, 1);
}

typedef struct {
    int16_t x;
    uint8_t planes[4]; /* pattern row */
} sprite_t;

static inline unsigned bit_index(uint32_t low) {
    static const uint8_t debruijn[32] = { 0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
                                          31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9 };
    return debruijn[(low * 0x077cb531u) >> 27];
}

/* The first eight sprites on line y (the ninth sets the overflow flag). */
static unsigned line_sprites(unsigned y, sprite_t *out) {
    gg_t *m = &machine;
    if (m->sprites_dirty) build_masks();
    const uint32_t *band = band_mask[y >> 3];
    if (!(band[0] | band[1])) return 0;
    const uint8_t *vram = m->vram, *sat = vram + m->sat_base;
    const unsigned zoom = m->vdp[1] & 1, tall = m->vdp[1] & 2, span = sprite_span();
    const unsigned base = (m->vdp[6] & 4u) << 6; /* tile 0 or 256 */
    const int shift = (m->vdp[0] & 8) ? 8 : 0;
    unsigned n = 0;
    for (unsigned w = 0; w < 2; w++)
        for (uint32_t bits = band[w]; bits;) {
            const uint32_t low = bits & (0u - bits);
            bits ^= low;
            const unsigned i = w * 32 + bit_index(low);
            const unsigned row = (unsigned)((int)y - sprite_top(sat[i]));
            if (row >= span) continue;
            if (n == 8) { m->vdp_status |= 64; return n; }
            unsigned tile = base | sat[0x81 + 2 * i], r = row >> zoom;
            if (tall) tile = (tile & ~1u) + (r >> 3);
            const uint8_t *p = vram + ((tile * 32 + (r & 7) * 4) & 0x3fff);
            sprite_t *s = &out[n++];
            s->x = (int16_t)(sat[0x80 + 2 * i] - shift);
            s->planes[0] = p[0]; s->planes[1] = p[1]; s->planes[2] = p[2]; s->planes[3] = p[3]; /* no memcpy call */
        }
    return n;
}

/* Two opaque sprite pixels on one screen pixel: the collision flag. */
static void collisions(const sprite_t *spr, unsigned n) {
    gg_t *m = &machine;
    if (n < 2 || (m->vdp_status & 32)) return;
    const unsigned zoom = m->vdp[1] & 1;
    uint32_t seen[9];
    for (unsigned i = 0; i < 9; i++) ((volatile uint32_t *)seen)[i] = 0; /* no memset call */
    for (unsigned k = 0; k < n; k++) {
        unsigned bits = spr[k].planes[0] | spr[k].planes[1] | spr[k].planes[2] | spr[k].planes[3];
        if (!bits) continue;
        uint32_t row = bits;
        if (zoom) { /* each pixel doubled: 16 bits */
            row = 0;
            for (unsigned b = 0; b < 8; b++) if (bits & (0x80u >> b)) row |= 0xc000u >> (2 * b);
            row <<= 16;
        } else row <<= 24;
        int x = spr[k].x;
        if (x < 0) { row <<= -x; x = 0; }
        const unsigned word = (unsigned)x >> 5, off = (unsigned)x & 31;
        const uint32_t a = row >> off, b = off ? row << (32 - off) : 0;
        if ((seen[word] & a) || (seen[word + 1] & b)) { m->vdp_status |= 32; return; }
        seen[word] |= a;
        seen[word + 1] |= b;
    }
}

/* ---- rendering ------------------------------------------------------------ */

static inline uint32_t row_fields(const uint8_t *p, int flip) {
    const uint32_t *t = flip ? gg_planes_flip : gg_planes;
    return t[p[0]] | t[p[1]] << 1 | t[p[2]] << 2 | t[p[3]] << 3;
}

/* count tiles of one name row from column tx into out (8 pixels each), row
 * tr of each tile; returns the tiles (bit k) with priority and a pixel. */
typedef struct {
    const uint8_t *vram;
    uint32_t tr;
    const uint16_t *pal;
    const uint32_t *planes;
    uint32_t count;
} gg_bg_args;
#if defined(__ARM_ARCH_6M__) && !defined(GG_NO_RENDER_ASM)
#define GG_RENDER_ASM 1
unsigned gg_bg_tiles(uint16_t *out, const uint8_t *names_row, unsigned tx, const gg_bg_args *args);
#else
#define GG_RENDER_ASM 0
#endif
#if !GG_RENDER_ASM || defined(GG_SELFTEST)
static unsigned gg_bg_tiles_c(uint16_t *o, const uint8_t *names_row, unsigned tx, const gg_bg_args *args) {
    unsigned prio = 0;
    for (unsigned k = 0; k < args->count; k++, o += 8, tx = (tx + 1) & 31) {
        const unsigned entry = names_row[2 * tx] | (names_row[2 * tx + 1] << 8);
        const unsigned tr = (entry & 0x400) ? 7 - args->tr : args->tr;
        const uint32_t w = row_fields(args->vram + (entry & 0x1ff) * 32 + tr * 4, entry & 0x200);
        const uint16_t *pal = args->pal + ((entry & 0x800) ? 16 : 0);
        o[0] = pal[w & 15];
        o[1] = pal[(w >> 4) & 15];
        o[2] = pal[(w >> 8) & 15];
        o[3] = pal[(w >> 12) & 15];
        o[4] = pal[(w >> 16) & 15];
        o[5] = pal[(w >> 20) & 15];
        o[6] = pal[(w >> 24) & 15];
        o[7] = pal[w >> 28];
        if ((entry & 0x1000) && w) prio |= 1u << k;
    }
    return prio;
}
#endif
#ifdef GG_SELFTEST
static int render_c; /* the self test renders both ways */
#define BG_TILES(o, n, t, a) (render_c ? gg_bg_tiles_c(o, n, t, a) : gg_bg_tiles(o, n, t, a))
#elif GG_RENDER_ASM
#define BG_TILES gg_bg_tiles
#else
#define BG_TILES gg_bg_tiles_c
#endif

/* Background of window line y into out[-7 .. 167]; bg_prio: pixels of
 * priority tiles that are not colour 0 (bit x + 8) under the n sprites spr,
 * returns whether any.  A NULL mask: no sprites to cover. */
static int background(unsigned y, uint16_t *out, uint32_t *bg_prio, const sprite_t *spr, unsigned n) {
    gg_t *m = &machine;
    const uint8_t *vram = m->vram;
    const unsigned names = (m->vdp[2] & 0x0eu) << 10;
    const unsigned hs = (y < 16 && (m->vdp[0] & 0x40)) ? 0 : m->vdp[8];
    const unsigned first = (GG_WIN_LEFT - hs) & 255, fine = first & 7, tx0 = first >> 3;
    const unsigned slot0 = fine ? 5 : 6; /* VDP column of the first tile */
    unsigned row = y + m->vscroll;
    if (row >= 224) row -= 224;
    /* tiles from VDP column 24 on ignore the vertical scroll (R0 bit 7) */
    const unsigned locked = (m->vdp[0] & 0x80) ? 24 - slot0 : 21;
    gg_bg_args args = { vram, row & 7, m->pal, &gg_planes_tab[0][0], locked };
    uint16_t *o = out - (int)fine;
    unsigned prio = BG_TILES(o, vram + names + (row >> 3) * 64, tx0, &args);
    if (locked < 21) {
        args.tr = y & 7;
        args.count = 21 - locked;
        prio |= BG_TILES(o + 8 * locked, vram + names + (y >> 3) * 64, (tx0 + locked) & 31, &args) << locked;
    }
    if (!prio || !bg_prio) return 0;
    uint32_t near = 0; /* tiles under a sprite: tile k shows pixels 8k - fine .. 8k + 7 - fine */
    const int width = 8 << (m->vdp[1] & 1);
    for (unsigned i = 0; i < n; i++) {
        const int lo = spr[i].x - (int)GG_WIN_LEFT + (int)fine, hi = lo + width - 1;
        if (hi < 0 || lo >= 8 * 21) continue;
        const unsigned k0 = lo < 0 ? 0 : (unsigned)lo >> 3, k1 = hi >= 8 * 21 ? 20 : (unsigned)hi >> 3;
        near |= (2u << k1) - (1u << k0);
    }
    prio &= near;
    if (!prio) return 0;
    for (unsigned k = 0; k < 21; k++) { /* rare: the pixels of priority tiles */
        if (!(prio >> k & 1)) continue;
        const unsigned r = k >= locked ? y : row, tx = (tx0 + k) & 31;
        const uint8_t *e = vram + names + ((r >> 3) * 32 + tx) * 2;
        const unsigned entry = e[0] | (e[1] << 8);
        const unsigned tr = (entry & 0x400) ? 7 - (r & 7) : (r & 7);
        const uint32_t w = row_fields(vram + (entry & 0x1ff) * 32 + tr * 4, entry & 0x200);
        const unsigned x0 = 8 * k + 8 - fine; /* bit of o[0] */
        for (unsigned px = 0; px < 8; px++)
            if ((w >> (4 * px)) & 15) bg_prio[(x0 + px) >> 5] |= 1u << ((x0 + px) & 31);
    }
    return 1;
}

static void draw_sprites(const sprite_t *spr, unsigned n, uint16_t *out, const uint32_t *bg_prio, int prio) {
    gg_t *m = &machine;
    const unsigned zoom = m->vdp[1] & 1;
    const uint16_t *pal = m->pal + 16;
    for (unsigned k = n; k-- > 0;) { /* lower numbers drawn last: on top */
        const uint32_t w = row_fields(spr[k].planes, 0);
        if (!w) continue;
        const int x0 = spr[k].x - (int)GG_WIN_LEFT;
        if (!zoom && !prio && x0 >= 0 && x0 <= GG_WIDTH - 8) { /* the usual case: whole, in front */
            uint16_t *o = out + x0;
            for (uint32_t v = w; v; v >>= 4, o++)
                if (v & 15) *o = pal[v & 15];
            continue;
        }
        for (unsigned px = 0; px < 8; px++) {
            const unsigned c = (w >> (4 * px)) & 15;
            if (!c) continue;
            for (unsigned z = 0; z <= zoom; z++) {
                const int x = x0 + (int)((px << zoom) + z);
                if (x < 0 || x >= GG_WIDTH) continue;
                if (prio && (bg_prio[(unsigned)(x + 8) >> 5] >> ((x + 8) & 31) & 1)) continue;
                out[x] = pal[c];
            }
        }
    }
}

/* A whole row of one colour (display off), two pixels per word: rows are
 * word-aligned. */
static void fill_row(uint16_t *out, uint16_t c) {
    uint32_t *w = (uint32_t *)out;
    const uint32_t v = c | (uint32_t)c << 16;
    for (unsigned i = 0; i < GG_WIDTH / 2; i += 4) { w[i] = v; w[i + 1] = v; w[i + 2] = v; w[i + 3] = v; }
}

static void sms_render(unsigned y, uint16_t *out, const sprite_t *spr, unsigned n);
static uint32_t sms_bg_fast(unsigned y, uint16_t *out);
static void render(unsigned y, uint16_t *out, const sprite_t *spr, unsigned n) {
    gg_t *m = &machine;
    if (GG_IS_SMS) { sms_render(y, out, spr, n); return; }
    if (!(m->vdp[1] & 0x40)) { /* display off: backdrop */
        fill_row(out, m->pal[16 + (m->vdp[7] & 15)]);
        return;
    }
    uint32_t bg_prio[6];
    if (n) for (unsigned i = 0; i < 6; i++) ((volatile uint32_t *)bg_prio)[i] = 0;
    const int prio = background(y, out, n ? bg_prio : 0, spr, n);
    if (n) draw_sprites(spr, n, out, bg_prio, prio);
}

/* ---- Master System: the whole line, box-filtered ------------------------- *
 *
 * 256 -> 160: each group of 8 screen pixels gives five, blends of two
 * neighbours in eighths: (5,3) (3,5) (4,4) (5,3) (3,5) on pixels 0-1, 1-2,
 * 3-4, 5-6, 6-7 (the box filter's (2,5,1) and (1,5,2) without their
 * smallest weight; the (4,4) blend rounds down).  Background groups of one
 * palette come straight from the tile rows: two neighbouring 4-bit pixels
 * form an 8-bit index into a table of (5,3) blends (pair53, per palette:
 * 1 KiB, rebuilt when the CRAM changes).  The groups under sprites, mixing
 * palettes or blanked go through colour indices. */

static void pairs_build(void) {
    for (unsigned h = 0; h < 2; h++)
        for (unsigned a = 0; a < 16; a++)
            for (unsigned b = 0; b < 16; b++) pair53[h][a | b << 4] = m0_panel(m0_fold565(5 * spal[16 * h + a] + 3 * spal[16 * h + b]));
    pairs_stale = 0;
}

/* five pixels from a group's 4-bit fields (one palette) */
static inline void group_out(uint32_t w, unsigned half, uint16_t *o) {
    const uint16_t *t = pair53[half];
    const uint32_t *sp = spal + 16 * half;
    o[0] = t[w & 0xff];
    o[1] = t[((w >> 8) & 15) | (w & 0xf0)];
    o[2] = m0_blend2_565(sp[(w >> 12) & 15], sp[(w >> 16) & 15]);
    o[3] = t[(w >> 20) & 0xff];
    o[4] = t[(w >> 28) | ((w >> 20) & 0xf0)];
}

/* the same from colour indices (both palettes) */
static void group_filter(const uint8_t *px, uint16_t *o) {
    uint32_t s[8];
    for (unsigned i = 0; i < 8; i++) s[i] = spal[px[i] & 31];
    m0_box58_fast(s, o);
}

/* Background colour indices of the tiles in mask (bit k: tile k, at screen
 * 8k - fine) into line[8 + ...] (bit 7: priority over sprites). */
static void sms_bg_indices(unsigned y, uint8_t *line, uint64_t tiles) {
    gg_t *m = &machine;
    const uint8_t *vram = m->vram;
    const unsigned names = (m->vdp[2] & 0x0eu) << 10;
    const unsigned hs = (y < 16 && (m->vdp[0] & 0x40)) ? 0 : m->vdp[8];
    const unsigned first = (0u - hs) & 255, fine = first & 7, tx0 = first >> 3;
    unsigned row = y + m->vscroll;
    if (row >= 224) row -= 224;
    for (unsigned k = 0; k < 33; k++, tiles >>= 1) {
        if (!(tiles & 1)) continue;
        uint8_t *o = line + 8 - fine + 8 * k;
        const int column = (int)k - (fine ? 1 : 0);
        const unsigned r = ((m->vdp[0] & 0x80) && column >= 24) ? y : row, tx = (tx0 + k) & 31;
        const uint8_t *e = vram + names + ((r >> 3) * 32 + tx) * 2;
        const unsigned entry = e[0] | (e[1] << 8);
        const unsigned tr = (entry & 0x400) ? 7 - (r & 7) : (r & 7);
        const uint32_t w = row_fields(vram + (entry & 0x1ff) * 32 + tr * 4, entry & 0x200);
        const unsigned base = (entry & 0x800) ? 16 : 0, prio = (entry & 0x1000) ? 0x80 : 0;
        for (unsigned px = 0; px < 8; px++) {
            const unsigned c = (w >> (4 * px)) & 15;
            o[px] = (uint8_t)((base + c) | (c ? prio : 0));
        }
    }
}

/* Background groups of one palette straight into out; returns the groups
 * left to the indexed path (two palettes across a tile boundary).  The
 * Thumb-1 gg_sms_bg (gg_render_asm.S) does the same from these. */
typedef struct {
    const uint8_t *names, *lock_names;  /* the name rows (64 bytes each) */
    const uint8_t *tr, *lock_tr;        /* vram + the row in the tile * 4 */
    const uint8_t *tr_flip, *lock_tr_flip; /* the same for a flipped tile */
    uint32_t lock, tx0x2, sh, sh2;      /* tiles in the first row, 2 * first column, 4 * fine, 32 - that */
    const uint32_t *planes;
    const uint16_t *pairs;
    const uint32_t *spal;
} gg_sms_args;
#if GG_RENDER_ASM
uint32_t gg_sms_bg(uint16_t *out, const gg_sms_args *a);
#endif
static uint32_t sms_bg_fast_c(unsigned y, uint16_t *out) {
    gg_t *m = &machine;
    const uint8_t *vram = m->vram;
    const unsigned names = (m->vdp[2] & 0x0eu) << 10;
    const unsigned hs = (y < 16 && (m->vdp[0] & 0x40)) ? 0 : m->vdp[8];
    const unsigned first = (0u - hs) & 255, fine = first & 7, tx0 = first >> 3, sh = 4 * fine;
    unsigned row = y + m->vscroll;
    if (row >= 224) row -= 224;
    const unsigned lock = (m->vdp[0] & 0x80) ? 24 + (fine ? 1 : 0) : 33; /* first tile with the vertical lock */
    uint32_t slow = 0, prev = 0, prev_pal = 0;
    for (unsigned k = 0; k < 33; k++) {
        const unsigned r = k >= lock ? y : row, tx = (tx0 + k) & 31;
        const uint8_t *e = vram + names + ((r >> 3) * 32 + tx) * 2;
        const unsigned entry = e[0] | (e[1] << 8);
        const unsigned tr = (entry & 0x400) ? 7 - (r & 7) : (r & 7);
        const uint32_t w = row_fields(vram + (entry & 0x1ff) * 32 + tr * 4, entry & 0x200);
        const uint32_t pal = entry & 0x800;
        if (k) { /* group k - 1: the end of tile k - 1, the start of tile k */
            const unsigned g = k - 1;
            if (fine && pal != prev_pal) slow |= 1u << g;
            else group_out(fine ? prev >> sh | w << (32 - sh) : prev, prev_pal ? 1 : 0, out + 5 * g);
        }
        if (!fine && k == 32) break;
        prev = w;
        prev_pal = pal;
    }
    return slow;
}

static void sms_compose(unsigned y, uint8_t *line, const sprite_t *spr, unsigned n, uint32_t groups);
void gg_render_indices(unsigned y, uint8_t *pixels) {
    gg_t *m = &machine;
    sprite_t spr[8];
    uint8_t line[8 + 256 + 8];
    const uint8_t bd = (uint8_t)(16 + (m->vdp[7] & 15));
    const unsigned n = (m->vdp[1] & 0x40) ? line_sprites(y, spr) : 0;
    if (!(m->vdp[1] & 0x40)) memset(line, bd, sizeof line);
    else sms_compose(y, line, spr, n, 0xffffffffu);
    if (m->vdp[0] & 0x20) memset(line + 8, bd, 8);
    for (unsigned i = 0; i < 256; i++) pixels[i] = line[8 + i] & 31;
}

/* a = (a + b) / 2 per channel, two pixels per word (rows are word-aligned) */
static inline uint32_t rev16(uint32_t v) {
#if GG_PANEL_SWAP && defined(__arm__)
    __asm__("rev16 %0, %0" : "+l"(v));
#elif GG_PANEL_SWAP
    v = ((v >> 8) & 0x00ff00ffu) | ((v << 8) & 0xff00ff00u);
#endif
    return v;
}
static void average_rows(uint16_t *a, const uint16_t *b) {
    uint32_t *wa = (uint32_t *)a;
    const uint32_t *wb = (const uint32_t *)b;
    for (unsigned i = 0; i < GG_WIDTH / 2; i++) {
        const uint32_t x = rev16(wa[i]), y = rev16(wb[i]);
        wa[i] = rev16((x & y) + (((x ^ y) & 0xf7def7deu) >> 1));
    }
}


/* ---- TMS9918 modes (Korean 150-in-1) ----------------------------------------- *
 *
 * The VDP falls into Graphics I/II, text and multicolour whenever M4 (reg 0
 * bit 2) is clear, whatever the cartridge:
 * the Master System's VDP draws those modes like a TMS9918, with the colours
 * of its own fixed palette.  Only that cartridge pays for the code: the other
 * Master System firmwares never take this path (GG_KOREAN150 is constant).
 * Sprites of the line are found from the table each time (no mask kept),
 * the line goes through 256 colour indices and the same 5-in-8 filter. */
typedef struct {
    int16_t left;
    uint16_t bits; /* pattern row, bit 15 = leftmost */
    uint8_t color, width, shift;
} tms_sprite_t;

/* TMS colour -> Master System colour (--BBGGRR) as the VDP shows it */
static const uint8_t tms_sms[16] = { 0x00, 0x00, 0x08, 0x0c, 0x10, 0x30, 0x01, 0x3c, 0x02, 0x03, 0x05, 0x0f, 0x04, 0x33, 0x15, 0x3f };
static uint32_t tms_spal[16];
static uint16_t tms_solid[16]; /* five equal output pixels */
static uint8_t tms_ready;
static void tms_palette(void) {
    static const uint8_t c5[4] = { 0, 10, 21, 31 }, c6[4] = { 0, 21, 42, 63 };
    for (unsigned i = 0; i < 16; i++) {
        const unsigned c = tms_sms[i];
        tms_spal[i] = m0_spread565((uint32_t)c5[c & 3] << 11 | (uint32_t)c6[(c >> 2) & 3] << 5 | c5[(c >> 4) & 3]);
        tms_solid[i] = m0_panel(m0_fold565(8 * tms_spal[i]));
    }
    tms_ready = 1;
}

/* The first four sprites on line y, with the fifth-sprite flag */
static unsigned tms_sprites(unsigned y, tms_sprite_t *out) {
    gg_t *m = &machine;
    const uint8_t *vram = m->vram;
    const unsigned table = (m->vdp[5] & 0x7fu) << 7, base = (m->vdp[6] & 7u) << 11;
    const unsigned size = (m->vdp[1] & 2) ? 16 : 8, shift = m->vdp[1] & 1, span = size << shift;
    unsigned count = 0, n = 0;
    for (unsigned i = 0; i < 32; i++) {
        const uint8_t *e = vram + table + i * 4;
        if (e[0] == 208) break;
        const int top = e[0] > 208 ? (int)e[0] - 255 : (int)e[0] + 1;
        if ((unsigned)((int)y - top) >= span) continue;
        if (++count == 5) {
            if (!(m->vdp_status & 64)) m->vdp_status = (uint8_t)((m->vdp_status & 0xa0) | 64 | i);
            break;
        }
        const unsigned color = e[3] & 15;
        if (!color) continue;
        const unsigned prow = (unsigned)((int)y - top) >> shift;
        unsigned tile = e[2];
        if (size == 16) tile &= ~3u;
        const unsigned a = (base + tile * 8 + (prow & 7) + (prow >= 8 ? 8 : 0)) & 0x3fff;
        tms_sprite_t *t = &out[n++];
        t->left = (int16_t)((int)e[1] - ((e[3] & 128) ? 32 : 0));
        t->bits = (uint16_t)((vram[a] << 8) | (size == 16 ? vram[(a + 16) & 0x3fff] : 0));
        t->color = (uint8_t)color;
        t->width = (uint8_t)size;
        t->shift = (uint8_t)shift;
    }
    return n;
}

/* Coincidence: two opaque sprite pixels on the same screen pixel */
static void tms_collisions(const tms_sprite_t *t, unsigned n) {
    if (n < 2) return;
    uint32_t used[9] = { 0 }; /* used[8]: past the right edge */
    for (unsigned k = 0; k < n; k++) {
        uint32_t w = 0;
        for (unsigned b = 0; b < t[k].width; b++)
            if (t[k].bits & (0x8000u >> b))
                for (unsigned z = 0; z <= t[k].shift; z++) w |= 0x80000000u >> ((b << t[k].shift) + z);
        int x = t[k].left;
        if (x < 0) { if (x <= -32) continue; w <<= -x; x = 0; }
        const unsigned word = (unsigned)x >> 5, off = (unsigned)x & 31;
        const uint32_t a = w >> off, b = off ? w << (32 - off) : 0;
        if ((used[word] & a) || (word < 7 && (used[word + 1] & b))) { machine.vdp_status |= 32; return; }
        used[word] |= a;
        if (word < 7) used[word + 1] |= b;
    }
}

/* Colour numbers of the line into px[0..255]: Graphics I, II, text,
 * multicolour, then the sprites (lower numbers on top) */
static void tms_indices(unsigned y, uint8_t *px, const tms_sprite_t *t, unsigned n, uint32_t tiles) {
    gg_t *m = &machine;
    const uint8_t *vram = m->vram;
    const unsigned backdrop = m->vdp[7] & 15;
    if (!(m->vdp[1] & 0x40)) { memset(px, (int)backdrop, 256); return; }
    const unsigned text = !!(m->vdp[1] & 16), multi = !!(m->vdp[1] & 8), graphics2 = !!(m->vdp[0] & 2);
    const unsigned name = (m->vdp[2] & 15u) << 10, pattern = (m->vdp[4] & 7u) << 11, color = (unsigned)m->vdp[3] << 6;
    if (text) { memset(px, (int)backdrop, 8); memset(px + 248, (int)backdrop, 8); }
    const unsigned columns = text ? 40 : 32, width = text ? 6 : 8;
    const unsigned rowname = name + (y / 8) * columns;
    uint8_t *dest = px + (text ? 8 : 0);
    for (unsigned tx = 0; tx < columns; tx++) {
        if (tiles != 0xffffffffu && !(tx < 32 && (tiles >> tx & 1))) { dest += width; continue; }
        const unsigned tile = vram[(rowname + tx) & 0x3fff];
        unsigned bits = 0, colors;
        if (text) {
            bits = vram[(pattern + tile * 8 + (y & 7)) & 0x3fff];
            colors = m->vdp[7];
        } else if (multi) {
            colors = vram[(pattern + tile * 8 + ((y & 31) >> 2)) & 0x3fff];
            const unsigned hi = colors >> 4, lo = colors & 15;
            memset(dest, (int)(hi ? hi : backdrop), 4);
            memset(dest + 4, (int)(lo ? lo : backdrop), 4);
            dest += 8;
            continue;
        } else {
            unsigned offset = tile * 8 + (y & 7);
            if (graphics2) {
                offset |= (y & 0xc0) << 5;
                bits = vram[((m->vdp[4] & 4u) << 11) | (offset & (((m->vdp[4] & 3u) << 11) | 0x7ff))];
                colors = vram[((m->vdp[3] & 128u) << 6) | (offset & (((m->vdp[3] & 127u) << 6) | 63))];
            } else {
                bits = vram[(pattern + offset) & 0x3fff];
                colors = vram[(color + (tile >> 3)) & 0x3fff];
            }
        }
        unsigned hi = colors >> 4, lo = text ? backdrop : colors & 15;
        if (!hi && !text) hi = backdrop;
        if (!lo) lo = backdrop;
        for (unsigned xx = 0; xx < width; xx++) { *dest++ = (uint8_t)((bits & 128) ? hi : lo); bits <<= 1; }
    }
    if (text) return;
    for (unsigned k = n; k-- > 0;)
        for (unsigned b = 0; b < t[k].width; b++) {
            if (!(t[k].bits & (0x8000u >> b))) continue;
            for (unsigned z = 0; z <= t[k].shift; z++) {
                const int x = t[k].left + (int)((b << t[k].shift) + z);
                if (x >= 0 && x < 256) px[x] = t[k].color;
            }
        }
}

/* Five output pixels of a background tile of two colours: the filter's
 * weights (eighths of the foreground) follow the pattern bits */
static inline void tms_tile(unsigned bits, unsigned hi, unsigned lo, uint16_t *o) {
    if (!(bits & 0xff)) { for (unsigned i = 0; i < 5; i++) o[i] = tms_solid[lo]; return; }
    if ((bits & 0xff) == 0xff) { for (unsigned i = 0; i < 5; i++) o[i] = tms_solid[hi]; return; }
    const uint32_t f = tms_spal[hi], b = tms_spal[lo];
    const unsigned b0 = bits >> 7 & 1, b1 = bits >> 6 & 1, b2 = bits >> 5 & 1, b3 = bits >> 4 & 1,
                   b4 = bits >> 3 & 1, b5 = bits >> 2 & 1, b6 = bits >> 1 & 1, b7 = bits & 1;
    const unsigned w0 = 5 * b0 + 3 * b1, w1 = 3 * b1 + 5 * b2, w3 = 5 * b5 + 3 * b6, w4 = 3 * b6 + 5 * b7;
    o[0] = m0_panel(m0_fold565(w0 * f + (8 - w0) * b));
    o[1] = m0_panel(m0_fold565(w1 * f + (8 - w1) * b));
    o[2] = m0_blend2_565(b3 ? f : b, b4 ? f : b);
    o[3] = m0_panel(m0_fold565(w3 * f + (8 - w3) * b));
    o[4] = m0_panel(m0_fold565(w4 * f + (8 - w4) * b));
}

static void tms_render(unsigned y, uint16_t *out, const tms_sprite_t *t, unsigned n) {
    gg_t *m = &machine;
    if (!tms_ready) tms_palette();
    if (!(m->vdp[1] & 0x40)) { /* display off: backdrop */
        for (unsigned i = 0; i < 160; i++) out[i] = tms_solid[m->vdp[7] & 15];
        return;
    }
    const int slow_mode = (m->vdp[1] & 0x18) != 0; /* text, multicolour: all through indices */
    uint32_t tiles = slow_mode ? 0xffffffffu : 0; /* tiles of the line drawn through indices */
    for (unsigned k = 0; k < n && !slow_mode; k++) {
        const int x0 = t[k].left, x1 = t[k].left + (int)(t[k].width << t[k].shift) - 1;
        if (x1 < 0 || x0 > 255) continue;
        const unsigned a = (unsigned)(x0 < 0 ? 0 : x0) >> 3, b = (unsigned)(x1 > 255 ? 255 : x1) >> 3;
        tiles |= (0xffffffffu >> (31 - b)) & (0xffffffffu << a);
    }
    if (tiles != 0xffffffffu) {
        const uint8_t *vram = m->vram;
        const unsigned backdrop = m->vdp[7] & 15, graphics2 = m->vdp[0] & 2;
        const unsigned rowname = ((m->vdp[2] & 15u) << 10) + (y >> 3) * 32;
        const unsigned pattern = (m->vdp[4] & 7u) << 11, color = (unsigned)m->vdp[3] << 6;
        const unsigned pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff, cmask = ((m->vdp[3] & 127u) << 6) | 63;
        const unsigned pbase = (m->vdp[4] & 4u) << 11, cbase = (m->vdp[3] & 128u) << 6;
        for (unsigned tx = 0; tx < 32; tx++) {
            if (tiles >> tx & 1) continue;
            const unsigned tile = vram[(rowname + tx) & 0x3fff];
            unsigned bits, colors;
            if (graphics2) {
                const unsigned offset = tile * 8 | (y & 0xc0) << 5 | (y & 7);
                bits = vram[pbase | (offset & pmask)];
                colors = vram[cbase | (offset & cmask)];
            } else {
                bits = vram[(pattern + tile * 8 + (y & 7)) & 0x3fff];
                colors = vram[(color + (tile >> 3)) & 0x3fff];
            }
            unsigned hi = colors >> 4, lo = colors & 15;
            if (!hi) hi = backdrop;
            if (!lo) lo = backdrop;
            tms_tile(bits, hi, lo, out + 5 * tx);
        }
    }
    if (!tiles) return;
    uint8_t px[256];
    tms_indices(y, px, t, n, tiles);
    for (unsigned g = 0; g < 32; g++) {
        if (!(tiles >> g & 1)) continue;
        uint32_t s[8];
        for (unsigned i = 0; i < 8; i++) s[i] = tms_spal[px[8 * g + i] & 15];
        m0_box58_fast(s, out + 5 * g);
    }
}

void gg_render_line(unsigned y, uint16_t *out) {
#if GG_OUT_WIDTH != GG_WIDTH /* native panel (PC Master System project): the
                              * whole line, palette applied 1:1 — the indices
                              * renderers, no box filter, no window crop */
    uint8_t px[256];
    if (!(machine.vdp[0] & 4)) {
        tms_sprite_t t[4];
        const unsigned n = (machine.vdp[1] & 0x40) ? tms_sprites(y, t) : 0;
        tms_indices(y, px, t, n, 0xffffffffu);
        for (unsigned i = 0; i < GG_OUT_WIDTH; i++) out[i] = tms_solid[px[i] & 15];
        return;
    }
    gg_render_indices(y, px);
    for (unsigned i = 0; i < GG_OUT_WIDTH; i++) out[i] = machine.pal[px[i]];
#else
    if (!(machine.vdp[0] & 4)) { /* TMS9918 modes, below */
        tms_sprite_t t[4];
        tms_render(y, out, t, (machine.vdp[1] & 0x40) ? tms_sprites(y, t) : 0);
        return;
    }
    sprite_t spr[8];
    const unsigned n = (machine.vdp[1] & 0x40) ? line_sprites(y, spr) : 0;
    render(y, out, spr, n);
#endif
}

static void vdp_line(unsigned y) {
    gg_t *m = &machine;
    sprite_t spr[8];
    tms_sprite_t tms[4];
    unsigned n = 0;
    const int legacy = !(m->vdp[0] & 4); /* Graphics I/II, text, multicolour */
    if (legacy) {
        if (m->vdp[1] & 0x40) {
            n = tms_sprites(y, tms);
            tms_collisions(tms, n);
        }
    } else if (m->vdp[1] & 0x40) {
        n = line_sprites(y, spr);
        collisions(spr, n);
    }
    if (y < GG_WIN_TOP || y >= GG_WIN_TOP + GG_WIN_LINES || !m->row || !m->row_out) return;
    const int draw = m->draw_line ? m->draw_line(m->platform, y) : 1;
    if (!draw) return;
#if GG_OUT_WIDTH != GG_WIDTH /* native panel: the whole line through the
                              * indices renderer, no box filter, no window
                              * crop (the display callback gets 256 columns) */
    gg_render_line(y, m->row_out);
#else
    if (legacy) {
        if (draw == 2 && m->row_tmp) {
            tms_render(y, m->row_tmp, tms, n);
            average_rows(m->row_out, m->row_tmp);
        } else tms_render(y, m->row_out, tms, n);
    } else if (draw == 2 && m->row_tmp) {
        render(y, m->row_tmp, spr, n);
        average_rows(m->row_out, m->row_tmp);
    } else render(y, m->row_out, spr, n);
#endif
    if (draw != 3) m->row(m->platform, y, m->row_out);
}

void gg_vdp_catchup(void) {
    gg_t *m = &machine;
    while (m->render_line < GG_ACTIVE && (int32_t)(m->cyc - m->render_at) >= 0) {
        const unsigned y = m->render_line++;
        m->render_at += GG_LINE_CYCLES;
        vdp_line(y);
    }
}

/* ---- line counter --------------------------------------------------------- */

/* Counted near the end of each line (GG_LINE_IRQ_AT): lines 0-192
 * decrement it, an underflow reloads R10 and raises the line interrupt;
 * later lines reload it. */
void gg_line_counter_catchup(void) {
    gg_t *m = &machine;
    if (m->lc_line >= GG_LINES) return;
    uint32_t t = m->cyc - m->frame_start;
    if ((int32_t)(t - GG_LINE_IRQ_AT) < 0) return;
    t -= GG_LINE_IRQ_AT;
    /* Same exact reciprocal as the V counter, with division for long gaps. */
    unsigned end = (t < 77000u ? ((t >> 2) * 18397u) >> 20 : t / GG_LINE_CYCLES) + 1;
    if (end > GG_LINES) end = GG_LINES;
    if (end <= m->lc_line) return;
    const unsigned active = end > GG_ACTIVE + 1 ? GG_ACTIVE + 1 : end;
    if (active > m->lc_line) {
        unsigned n = active - m->lc_line;
        if (n <= m->lc_count) m->lc_count -= n;
        else {
            n -= m->lc_count + 1;
            const unsigned period = m->vdp[10] + 1;
            m->line_irq = 1;
            m->lc_count = (uint8_t)(m->vdp[10] - (n < period ? n : n % period));
        }
    }
    if (end > GG_ACTIVE + 1) m->lc_count = m->vdp[10];
    m->lc_line = (uint16_t)end;
}

uint32_t gg_vdp_next_event(void) {
    gg_t *m = &machine;
    if (!(m->vdp[0] & 16) || m->line_irq || m->lc_line > GG_ACTIVE) return 0;
    const unsigned line = m->lc_line + m->lc_count;
    if (line > GG_ACTIVE) return 0;
    return m->frame_start + (uint32_t)line * GG_LINE_CYCLES + GG_LINE_IRQ_AT;
}

void gg_vdp_frame_start(void) {
    machine.vscroll = machine.vdp[9];
    machine.lc_line = 0;
}

/* ---- ports ---------------------------------------------------------------- */

uint8_t gg_vcounter(void) {
    gg_t *m = &machine;
    const uint32_t t = m->cyc - m->frame_start;
    /* t / 228 for t < 77000 (a frame and a bit): multiply and shift */
    uint32_t line = t < 77000u ? ((t >> 2) * 18397u) >> 20 : t / GG_LINE_CYCLES;
    if (line >= GG_LINES) line = GG_LINES - 1;
    return (uint8_t)(line <= 0xda ? line : line - 6);
}

/* An IN samples the bus in its last T-state: a status read issued by an
 * instruction that starts at m->cyc sees the vblank flag when the flag's
 * raise cycle (frame_start + GG_VBLANK_CYCLES) falls at or before that
 * T-state, GG_STATUS_READ_T cycles in (IN A,(n): the 11th T-state) — the
 * event model alone only raises it at instruction boundaries, one poll
 * iteration late.  GG_VBL_READ_EARLY=k overrides the offset (bench sweeps). */
#ifndef GG_STATUS_READ_T
#define GG_STATUS_READ_T 10
#endif
static int vbl_read_early = -1;

uint8_t gg_vdp_status_read(void) {
    gg_t *m = &machine;
    if (vbl_read_early < 0) {
        const char *e = getenv("GG_VBL_READ_EARLY");
        vbl_read_early = e ? atoi(e) : GG_STATUS_READ_T;
    }
    m->vdp_pending = 0;
    gg_vdp_catchup();
    gg_line_counter_catchup();
    if (vbl_read_early && !m->vblank_done) {
        const uint32_t vb = m->frame_start + GG_VBLANK_CYCLES;
        if ((int32_t)(m->cyc + vbl_read_early - vb) >= 0) m->vdp_status |= 128;
    }
    const uint8_t v = (uint8_t)(m->vdp_status | 31);
    m->vdp_status = 0;
    m->line_irq = 0;
    gg_irq_update();
    m->next_evt = m->cyc; /* the next line interrupt is scheduled again */
    return v;
}

uint8_t gg_vdp_data_read(void) {
    gg_t *m = &machine;
    m->vdp_pending = 0;
    const uint8_t v = m->vdp_buffer;
    m->vdp_buffer = m->vram[m->vdp_address];
    m->vdp_address = (m->vdp_address + 1) & 0x3fff;
    return v;
}

void gg_vdp_data_write(uint8_t v) {
    gg_t *m = &machine;
    m->vdp_pending = 0;
    gg_vdp_catchup();
    if (m->vdp_code == 3 && GG_IS_SMS) { /* CRAM: 32 bytes */
        m->cram[m->vdp_address & 31] = v;
        cram_colour_sms(m->vdp_address & 31);
    } else if (m->vdp_code == 3) { /* CRAM: even bytes latched, odd bytes write the pair */
        const unsigned a = m->vdp_address & 63;
        if (!(a & 1)) m->cram_latch = v;
        else {
            m->cram[a - 1] = m->cram_latch;
            m->cram[a] = v;
            cram_colour(a >> 1);
        }
    } else {
        const unsigned a = m->vdp_address, entry = (uint16_t)(a - m->sat_base);
        if (entry < 64) sprite_y(entry, m->vram[a], v);
        m->vram[a] = v;
    }
    m->vdp_buffer = v;
    m->vdp_address = (m->vdp_address + 1) & 0x3fff;
}

void gg_vdp_control_write(uint8_t v) {
    gg_t *m = &machine;
    if (!m->vdp_pending) {
        m->vdp_latch = v;
        m->vdp_address = (uint16_t)((m->vdp_address & 0x3f00) | v);
        m->vdp_pending = 1;
        return;
    }
    m->vdp_pending = 0;
    m->vdp_code = v >> 6;
    m->vdp_address = (uint16_t)(((v & 63) << 8) | m->vdp_latch);
    if (m->vdp_code == 0) { /* read setup: prefetch */
        m->vdp_buffer = m->vram[m->vdp_address];
        m->vdp_address = (m->vdp_address + 1) & 0x3fff;
    } else if (m->vdp_code == 2) {
        const unsigned r = v & 15;
        if (r > 10) return;
        gg_vdp_catchup();
        gg_line_counter_catchup();
        const uint8_t before = m->vdp[r];
        m->vdp[r] = m->vdp_latch;
        if (r == 5) {
            m->sat_base = (uint16_t)((m->vdp_latch & 0x7e) << 7);
            if (before != m->vdp_latch) m->sprites_dirty = 1;
        }
        if (r == 1 && ((before ^ m->vdp_latch) & 3)) m->sprites_dirty = 1;
        if (r <= 1) {
            gg_irq_update();
            m->next_evt = m->cyc; /* interrupt enables: schedule again */
        }
    }
}

/* ---- self test ------------------------------------------------------------ */

#ifdef GG_SELFTEST
/* Background tiles, Thumb-1 against C, on random VRAM, CRAM, scrolls and
 * locks: the number of differing lines (both palettes, flips, priority). */
#if GG_RENDER_ASM
/* Odd rounds: a name table of three entries, two of them the same tile in
 * both palettes, in runs (rounds 3, 7...) or mixed: the paths of repeated
 * tiles and groups. */
static void selftest_names(uint32_t *r, unsigned round) {
    if (!(round & 1)) return;
    gg_t *m = &machine;
    uint16_t set[3];
    for (unsigned k = 0; k < 3; k++) { *r = *r * 1103515245u + 12345u; set[k] = (uint16_t)(*r >> 12); }
    set[1] = set[0] ^ 0x0800;
    const unsigned names = (m->vdp[2] & 0x0eu) << 10;
    unsigned pick = 0;
    for (unsigned i = 0; i < 32 * 28; i++) {
        *r = *r * 1103515245u + 12345u;
        if (!(round & 2) || ((*r >> 16) & 7) == 0) pick = (*r >> 20) % 3;
        m->vram[names + 2 * i] = (uint8_t)set[pick];
        m->vram[names + 2 * i + 1] = (uint8_t)(set[pick] >> 8);
    }
}
#endif

unsigned gg_render_selftest(void) {
#if GG_RENDER_ASM
    gg_t *m = &machine;
    uint32_t r = 12345;
    unsigned bad = 0;
    uint16_t a[GG_ROW_STRIDE], b[GG_ROW_STRIDE];
    sprite_t cover[23]; /* sprites over every pixel: the whole priority mask */
    for (unsigned i = 0; i < 23; i++) cover[i].x = (int16_t)((int)GG_WIN_LEFT - 8 + 8 * (int)i);
    for (unsigned round = 0; round < 16; round++) {
        for (unsigned i = 0; i < 16384; i++) { r = r * 1103515245u + 12345u; m->vram[i] = (uint8_t)(r >> 16); }
        for (unsigned i = 0; i < 32; i++) { r = r * 1103515245u + 12345u; m->pal[i] = (uint16_t)(r >> 8); }
        r = r * 1103515245u + 12345u;
        m->vdp[0] = (uint8_t)(0x06 | ((r >> 16) & 0xc0));
        m->vdp[1] = 0x00; /* no sprites: the background only */
        m->vdp[2] = (uint8_t)(0x0e & (r >> 8));
        m->vdp[8] = (uint8_t)(r >> 20);
        m->vscroll = (uint8_t)((r >> 24) % 224);
        selftest_names(&r, round);
        for (unsigned y = GG_TOP; y < GG_TOP + GG_HEIGHT; y++) {
            uint32_t pa[6] = { 0 }, pb[6] = { 0 };
            memset(a, 0, sizeof a); memset(b, 0, sizeof b);
            render_c = 0; const int qa = background(y, a + GG_ROW_PAD, pa, cover, 23);
            render_c = 1; const int qb = background(y, b + GG_ROW_PAD, pb, cover, 23);
            bad += qa != qb || memcmp(a, b, sizeof a) || memcmp(pa, pb, sizeof pa);
        }
    }
    /* Master System background: random CRAM (pair tables), scrolls, locks */
    m->sms = 1;
    for (unsigned round = 0; round < 16; round++) {
        for (unsigned i = 0; i < 16384; i++) { r = r * 1103515245u + 12345u; m->vram[i] = (uint8_t)(r >> 16); }
        for (unsigned i = 0; i < 32; i++) { r = r * 1103515245u + 12345u; m->cram[i] = (uint8_t)(r >> 16) & 63; cram_colour_sms(i); }
        pairs_build();
        r = r * 1103515245u + 12345u;
        m->vdp[0] = (uint8_t)(0x06 | ((r >> 16) & 0xc0));
        m->vdp[2] = (uint8_t)(0x0e & (r >> 8));
        m->vdp[8] = (uint8_t)(r >> 20);
        m->vscroll = (uint8_t)((r >> 24) % 224);
        selftest_names(&r, round);
        for (unsigned y = 0; y < GG_ACTIVE; y++) {
            memset(a, 0, sizeof a); memset(b, 0, sizeof b);
            render_c = 0; const uint32_t qa = sms_bg_fast(y, a + GG_ROW_PAD);
            render_c = 1; const uint32_t qb = sms_bg_fast(y, b + GG_ROW_PAD);
            for (unsigned g = 0; g < 32; g++) /* groups left to the C path: not drawn here */
                if (qa >> g & 1) memset(a + GG_ROW_PAD + 5 * g, 0, 10), memset(b + GG_ROW_PAD + 5 * g, 0, 10);
            bad += qa != qb || memcmp(a, b, sizeof a);
        }
    }
    m->sms = 0;
    render_c = 0;
    return bad;
#else
    return 0;
#endif
}
#endif

/* Colour indices of the groups in mask (bit g: screen pixels 8g-8g+7) in
 * line[8 + ...]: background, then sprites (bit 7 of the background: its
 * priority over them). */
static void sms_compose(unsigned y, uint8_t *line, const sprite_t *spr, unsigned n, uint32_t groups) {
    gg_t *m = &machine;
    const unsigned zoom = m->vdp[1] & 1;
    const unsigned hs = (y < 16 && (m->vdp[0] & 0x40)) ? 0 : m->vdp[8], fine = (0u - hs) & 7;
    /* Keep bit 32: a finely scrolled last group also covers tile 32. */
    uint64_t tiles = groups;
    if (fine) tiles |= tiles << 1;
    sms_bg_indices(y, line, tiles);
    for (unsigned k = n; k-- > 0;) { /* lower numbers drawn last: on top */
        const uint32_t w = row_fields(spr[k].planes, 0);
        if (!w) continue;
        for (unsigned px = 0; px < 8; px++) {
            const unsigned c = (w >> (4 * px)) & 15;
            if (!c) continue;
            for (unsigned z = 0; z <= zoom; z++) {
                const int x = spr[k].x + (int)((px << zoom) + z);
                if (x < 0 || x >= 256 || (line[8 + x] & 0x80)) continue;
                line[8 + x] = (uint8_t)(16 + c);
            }
        }
    }
}

static uint32_t sms_bg_fast(unsigned y, uint16_t *out) {
#if GG_RENDER_ASM
#ifdef GG_SELFTEST
    if (render_c) return sms_bg_fast_c(y, out);
#endif
    gg_t *m = &machine;
    const unsigned names = (m->vdp[2] & 0x0eu) << 10;
    const unsigned hs = (y < 16 && (m->vdp[0] & 0x40)) ? 0 : m->vdp[8];
    const unsigned first = (0u - hs) & 255, fine = first & 7;
    unsigned row = y + m->vscroll;
    if (row >= 224) row -= 224;
    const uint8_t *const vram = m->vram;
    const unsigned lock = (m->vdp[0] & 0x80) ? 24u + (fine ? 1u : 0u) : 33u;
    const gg_sms_args a = { vram + names + (row >> 3) * 64, vram + names + (y >> 3) * 64,
                            vram + (row & 7) * 4, vram + (y & 7) * 4,
                            vram + (7 - (row & 7)) * 4, vram + (7 - (y & 7)) * 4,
                            lock, (first >> 3) * 2, 4 * fine, 32 - 4 * fine,
                            &gg_planes_tab[0][0], &pair53[0][0], spal };
    return gg_sms_bg(out, &a);
#else
    return sms_bg_fast_c(y, out);
#endif
}

/* A Master System line: the fast background, then the groups under
 * sprites, of palette 1 or blanked through colour indices. */
static void sms_render(unsigned y, uint16_t *out, const sprite_t *spr, unsigned n) {
    gg_t *m = &machine;
    const uint16_t bd = m->pal[16 + (m->vdp[7] & 15)];
    if (!(m->vdp[1] & 0x40)) { fill_row(out, bd); return; }
    if (pairs_stale) pairs_build();
    uint32_t slow = sms_bg_fast(y, out);
    const unsigned zoom = m->vdp[1] & 1;
    for (unsigned k = 0; k < n; k++) { /* groups under the sprites' opaque pixels */
        const unsigned opaque = spr[k].planes[0] | spr[k].planes[1] | spr[k].planes[2] | spr[k].planes[3];
        if (!opaque) continue;
        unsigned left = 0, right = 7; /* pixel 0 is bit 7 */
        while (!(opaque & (0x80u >> left))) left++;
        while (!(opaque & (0x80u >> right))) right--;
        const int x0 = spr[k].x + (int)(left << zoom), x1 = spr[k].x + (int)((right + 1) << zoom) - 1;
        if (x1 < 0 || x0 > 255) continue;
        for (int g = (x0 < 0 ? 0 : x0) >> 3; g <= (x1 > 255 ? 255 : x1) >> 3; g++) slow |= 1u << g;
    }
    const int blank = (m->vdp[0] & 0x20) != 0;
    if (blank) slow &= ~1u;
    if (slow) {
        uint8_t line[8 + 256 + 8];
        sms_compose(y, line, spr, n, slow);
        for (unsigned g = 0; g < 32; g++) if (slow >> g & 1) group_filter(line + 8 + 8 * g, out + 5 * g);
    }
    if (blank) for (unsigned i = 0; i < 5; i++) out[i] = bd; /* left column */
}

#ifdef GG_ROM_TRACE
/* Host explorer: the machine was copied back from a snapshot, rebuild what
 * the VDP derives from it (sprite masks, the Master System's palette). */
void gg_vdp_restore(void) {
    machine.sprites_dirty = 1;
    for (unsigned i = 0; i < 32; i++) {
        if (GG_IS_SMS) cram_colour_sms(i);
        else cram_colour(i);
    }
}
#endif
