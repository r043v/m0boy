#include "ppu.h"
#if __has_include("nes_config.h") /* a generated project's; the checkout has none */
#include "nes_config.h"
#endif
#ifndef NES_PANEL_SWAP
#define NES_PANEL_SWAP 0
#endif

/* the panel's byte order is the generated project's (nes_config.h) */
#define M0_PANEL_SWAP NES_PANEL_SWAP
#include "m0_scale.h"

/* NES master palette as RGB565: the classic 2C02 mix used by nes-py
 * (FCEUX-style), converted from its 24-bit table. */
static const uint16_t nes_colors[64] = {
    0x7BEF, 0x001F, 0x0017, 0x4157, 0x9010, 0xA804, 0xA880, 0x88A0,
    0x5180, 0x03C0, 0x0340, 0x02C0, 0x020B, 0x0000, 0x0000, 0x0000,
    0xBDF7, 0x03DF, 0x02DF, 0x6A3F, 0xD819, 0xE00B, 0xF9C0, 0xE2E2,
    0xABE0, 0x05C0, 0x0540, 0x0548, 0x0451, 0x0000, 0x0000, 0x0000,
    0xFFDF, 0x3DFF, 0x6C5F, 0x9BDF, 0xFBDF, 0xFAD3, 0xFBCB, 0xFD08,
    0xFDC0, 0xBFC3, 0x5ECA, 0x5FD3, 0x075B, 0x7BCF, 0x0000, 0x0000,
    0xFFFF, 0xA73F, 0xBDDF, 0xDDDF, 0xFDDF, 0xFD38, 0xF696, 0xFF15,
    0xFECF, 0xDFCF, 0xBFD7, 0xBFDB, 0x07FF, 0xFEDF, 0x0000, 0x0000,
};

#define CTRL_NMI 0x80
#define CTRL_SPR16 0x20
#define CTRL_BG_PAGE 0x10 /* bit 4: background pattern table */
#define CTRL_SPR_PAGE 0x08 /* bit 3: sprite pattern table */
#define CTRL_INC 0x04
#define MASK_BG 0x08
#define MASK_SPR 0x10
#define MASK_BG_LEFT 0x02
#define MASK_SPR_LEFT 0x04
#define ST_VBLANK 0x80
#define ST_SPRITE0 0x40

/* the panel tables' palette entries a palette RAM write makes stale, and
 * the count of nametable writes (the decoded tile row's key) */
static uint32_t pal_dirty = 0xFFFFFFFFu, nt_writes;

/* NES_PPU_ASM: the panel renderer's loops in Thumb-1 (ppu_asm.S), the C
 * versions in this file being their reference (the host's) */
#ifndef NES_PPU_ASM
#if defined(__ARM_ARCH_6M__) && !defined(NES_NO_PPU_ASM)
#define NES_PPU_ASM 1
#else
#define NES_PPU_ASM 0
#endif
#endif

/* the loops stay functions of their own (profiles name them) and in flash:
 * on the META, code run from SRAM faulted with the CPU overclocked
 * (gbl_profile.h, GBL_PPU_RAMFUNC) */
#define NES_LOOP __attribute__((noinline))
static void tables_init(void);
#ifdef NES_PPU_REFERENCE
static int pair_init; /* the pair tables wait for the palette RAM to exist */
static void pair_write(nes_ppu_t *ppu, unsigned i);
#endif

void nes_ppu_set_rows(nes_ppu_t *ppu, unsigned rows) {
    m0_plan_ends(ppu->panel_ends, 240, rows);
}

void nes_ppu_reset(nes_ppu_t *ppu) {
    ppu->ctrl = ppu->mask = ppu->status = 0;
    ppu->v = ppu->t = 0;
    ppu->x = ppu->w = 0;
    ppu->buffer = 0;
    ppu->frame = 0;
    ppu->oam_dirty = 1;
    pal_dirty = 0xFFFFFFFFu;
    nt_writes++; /* the nametables may have been cleared behind the PPU */
    tables_init();
}

/* $2000-$2FFF lands in the 2 KiB of nametable RAM per the cartridge's
 * mirroring (vertical mirroring mirrors horizontally, and conversely). */
static uint16_t mirror(nes_ppu_t *ppu, uint16_t at) {
    at &= 0x0FFF;
    if (ppu->vertical_mirror) return (uint16_t)(at & 0x07FF);
    return (uint16_t)((at & 0x03FF) | ((at & 0x0800) >> 1));
}

/* CHR ROM (NROM has no CHR RAM): both pattern tables are addressable, the
 * page bit of $2000 picks one per sprite or background fetch. */
static uint8_t chr_at(const nes_ppu_t *ppu, unsigned at) {
    return ppu->chr ? ppu->chr[at & 0x1FFF] : 0;
}

uint8_t nes_ppu_read(nes_ppu_t *ppu, unsigned reg) {
    switch (reg) {
    case 2: {
        const uint8_t out = (uint8_t)((ppu->status & (ST_VBLANK | ST_SPRITE0 | 0x20)) |
                                      (ppu->open_bus & 0x1F));
        ppu->status &= (uint8_t)~ST_VBLANK;
        ppu->w = 0;
        return out;
    }
    case 4: return ppu->oam[ppu->oam_addr];
    case 7: {
        const uint16_t at = ppu->v & 0x3FFF;
        const uint8_t out = ppu->buffer;
        if ((at & 0x3F00) == 0x3F00) {
            ppu->buffer = ppu->nametable[mirror(ppu, at & 0x2FFF)];
        } else if (at >= 0x2000) ppu->buffer = ppu->nametable[mirror(ppu, at)];
        else ppu->buffer = chr_at(ppu, at);
        ppu->v = (uint16_t)(ppu->v + ((ppu->ctrl & CTRL_INC) ? 32 : 1));
        return out;
    }
    default: return ppu->open_bus;
    }
}

void nes_ppu_write(nes_ppu_t *ppu, unsigned reg, uint8_t value) {
    ppu->open_bus = value;
    switch (reg) {
    case 0:
        if ((ppu->ctrl ^ value) & CTRL_SPR16) ppu->oam_dirty = 1;
        ppu->ctrl = value;
        ppu->t = (uint16_t)((ppu->t & 0x73FF) | ((value & 3) << 10));
        break;
    case 1: ppu->mask = value; break;
    case 3: ppu->oam_addr = value; break;
    case 4:
        ppu->oam[ppu->oam_addr++] = value;
        ppu->oam_dirty = 1;
        break;
    case 5:
        if (!ppu->w) {
            ppu->x = value & 7;
            ppu->t = (uint16_t)((ppu->t & 0x7FE0) | (value >> 3));
            ppu->w = 1;
        } else {
            ppu->t = (uint16_t)((ppu->t & 0x0C1F) | ((value & 7) << 12) | ((value & 0xF8) << 2));
            ppu->w = 0;
        }
        break;
    case 6:
        if (!ppu->w) {
            ppu->t = (uint16_t)((ppu->t & 0x00FF) | ((value & 0x3F) << 8));
            ppu->w = 1;
        } else {
            ppu->t = (uint16_t)((ppu->t & 0x7F00) | value);
            ppu->v = ppu->t;
            ppu->w = 0;
        }
        break;
    case 7: {
        const uint16_t at = ppu->v & 0x3FFF;
        if ((at & 0x3F00) == 0x3F00) {
            uint8_t idx = at & 0x1F;
            if ((idx & 0x13) == 0x10) idx &= 0x0F; /* the mirrors of $3F10 */
            ppu->palette[idx] = value & 0x3F;
            /* colour 0 of every palette shows the backdrop; 4, 8 and 12
             * are never read */
            pal_dirty |= (idx & 3) ? 1u << idx : idx == 0 ? 0x11111111u : 0u;
#ifdef NES_PPU_REFERENCE
            if (pair_init) pair_write(ppu, idx);
#endif
        } else if (at >= 0x2000) {
            ppu->nametable[mirror(ppu, at)] = value;
            nt_writes++; /* the panel renderer's tile row is stale */
        } else if (ppu->chr_writable) {
            /* CHR-RAM: the game uploads its patterns through $2007 (ROM
             * boards ignore the write) */
            ((uint8_t *)ppu->chr)[at & 0x1FFF] = value;
        }
        ppu->v = (uint16_t)(ppu->v + ((ppu->ctrl & CTRL_INC) ? 32 : 1));
        break;
    }
    default: break;
    }
}

/* A sprite's pattern row on a line, MSB = leftmost pixel; an 8x16 sprite
 * takes its two tiles from the page that bit 0 of its tile number picks. */
static const uint8_t *sprite_pattern(const nes_ppu_t *ppu, const uint8_t *oam, unsigned row) {
    const unsigned height = (ppu->ctrl & CTRL_SPR16) ? 16 : 8;
    const unsigned flip_v = (oam[2] & 0x80) ? height - 1 - row : row;
    unsigned pattern;
    if (ppu->ctrl & CTRL_SPR16)
        pattern = ((oam[1] & 1) ? 0x1000u : 0u) + (oam[1] & 0xFEu) * 16u + (flip_v & 7) + (flip_v & 8 ? 16 : 0);
    else
        pattern = oam[1] * 16u + flip_v + ((ppu->ctrl & CTRL_SPR_PAGE) ? 0x1000u : 0u);
    return ppu->chr + pattern;
}

/* ---- the panel picture --------------------------------------------------
 *
 * A line is first a row of colour indices in fetch-stream order (screen
 * pixel c at c + fine x), one byte a pixel, the palette-space index times
 * two: background (pal << 3 | c << 1), c = 0 the backdrop whatever the
 * palette; sprites 32 | pal << 3 | c << 1.  Times two, an index is the byte
 * offset of a pair in the 32x32 tables of 16-bit colours.
 *
 * The two lines of a panel row are kept word-interleaved: stream word w of
 * line r (0 or 1) at byte 8w + 4r, so one load-multiple brings both lines'
 * words of a group (ROW_AT).  A row alone is line 0 of the pair.
 *
 * The background comes from the planes through one table (expand: a nibble
 * of each plane -> four index bytes) and a tile row decoded once for its
 * eight lines, the sprites from per-line masks of the OAM, rebuilt when the
 * OAM or the sprite size changes.  Each line then goes through
 * m0_box58_fast (256 -> 160) from tables of pixel pairs, and the two lines
 * of a row are averaged (m0_avg2_panel).  Shortcuts that give the same
 * pixels: a group whose words are one colour repeated is that colour, a
 * group equal on both lines is the line alone. */

#define PAIR_BYTES 528u /* 66 stream words of two lines; the last group's
                         * third word pair is still inside */
#define ROW_AT(s) ((((s) >> 2) << 3) | ((s) & 3u)) /* stream byte -> offset */
static uint8_t pairs[PAIR_BYTES] __attribute__((aligned(4)));
static uint8_t probe[PAIR_BYTES] __attribute__((aligned(4))); /* sprite-0 lines of hidden frames */
static uint8_t row_x[2];               /* each pair line's fine x */
static uint32_t expand[256];           /* lo nibble | hi nibble << 4 -> 4 bytes */
static uint8_t flip8[256];             /* bit reversal: horizontal flip */
static uint32_t spr_lines[240][2];     /* the sprites over each line, bit = OAM index */
static uint32_t spal[32];              /* spread colours of the palette indices */
static uint16_t pair_tabs[2][32 * 32]; /* m0_box58_fast's (5,3) and (4,4) of
                                        * pixel pairs, native RGB565 (one array:
                                        * ppu_asm.S finds the second after the first) */
#define pair53 pair_tabs[0]
#define pair44 pair_tabs[1]
static unsigned panel_row;             /* the row being filled */
static unsigned panel_open;            /* the row has its first line (pair slot 1 next) */

static void tables_init(void) {
    static int ready;
    if (ready) return;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t w = 0;
        for (unsigned j = 0; j < 4; j++) {
            const unsigned bit = 3 - j;
            const unsigned c = ((i >> bit) & 1) << 1 | ((i >> (4 + bit)) & 1) << 2;
            w |= (uint32_t)c << (8 * j);
        }
        expand[i] = w;
        unsigned r = 0;
        for (unsigned b = 0; b < 8; b++) r |= ((i >> b) & 1) << (7 - b);
        flip8[i] = (uint8_t)r;
    }
    ready = 1;
}

#define PAIR(tab, a, b) (*(const uint16_t *)((const uint8_t *)(tab) + ((unsigned)(a) << 5) + (b)))

static inline void pair_set(unsigned a, unsigned b) {
    pair53[a * 32 + b] = m0_fold565(5 * spal[a] + 3 * spal[b]);
    const uint32_t m = ((spal[a] + spal[b]) >> 1) & 0x07e0f81fu; /* m0_blend2_565, native */
    pair44[a * 32 + b] = (uint16_t)(m | (m >> 16));
}

/* the tables follow the palette RAM: the entries a write touched, all of
 * them past a dozen */
static void nes_ppu_tables(const nes_ppu_t *ppu) {
    const uint32_t d = pal_dirty;
    unsigned n = 0;
    pal_dirty = 0;
    for (unsigned i = 0; i < 32; i++)
        if (d >> i & 1) {
            spal[i] = m0_spread565(nes_colors[ppu->palette[(i & 3) ? i : 0] & 0x3F]);
            n++;
        }
    if (n > 12) {
        for (unsigned a = 0; a < 32; a++)
            for (unsigned b = 0; b < 32; b++) pair_set(a, b);
        return;
    }
    for (unsigned i = 0; i < 32; i++)
        if (d >> i & 1)
            for (unsigned j = 0; j < 32; j++) {
                pair_set(i, j);
                pair_set(j, i);
            }
}

static void nes_ppu_sprite_lines(nes_ppu_t *ppu) {
    uint32_t *m = &spr_lines[0][0];
    for (unsigned i = 0; i < 480; i++) m[i] = 0;
    const unsigned height = (ppu->ctrl & CTRL_SPR16) ? 16 : 8;
    for (unsigned s = 0; s < 64; s++) {
        unsigned y = ppu->oam[s * 4] + 1u; /* a sprite at y covers y+1.. */
        if (y >= 240) continue;
        unsigned end = y + height;
        if (end > 240) end = 240;
        uint32_t *line = &spr_lines[y][s >> 5];
        const uint32_t bit = 1u << (s & 31);
        for (; y < end; y++, line += 2) *line |= bit;
    }
    ppu->oam_dirty = 0;
}

/* The tile row under v, decoded once for the eight lines that share it:
 * each tile's pattern (page and tile number) and its palette in every byte
 * of a word.  The key is v without its fine y, the page, and a count of
 * the nametable writes. */
typedef struct { const uint8_t *pattern; uint32_t pal; } nes_tile;
static nes_tile tile_row[33];
static uint32_t tile_key = 0xFFFFFFFFu;

void nes_ppu_state_loaded(nes_ppu_t *ppu) {
    pal_dirty = 0xFFFFFFFFu;
    nt_writes++;
    tile_key = 0xFFFFFFFFu;
    ppu->oam_dirty = 1;
#ifdef NES_PPU_REFERENCE
    pair_init = 0;
#endif
}

static void nes_ppu_tile_row(const nes_ppu_t *ppu) {
    const unsigned v = ppu->v;
    unsigned cx = v & 31;
    const unsigned cy = (v >> 5) & 31, table = (v >> 10) & 3;
    unsigned half, other;
    if (ppu->vertical_mirror) { half = table & 1; other = half ^ 1; }
    else { half = table >> 1; other = half; }
    const uint8_t *chr = ppu->chr + ((ppu->ctrl & CTRL_BG_PAGE) ? 0x1000 : 0);
    const unsigned yshift = (cy & 2) << 1;
    nes_tile *t = tile_row;
    unsigned left = 33;
    for (;;) {
        const uint8_t *nt = ppu->nametable + half * 0x400u;
        const uint8_t *names = nt + cy * 32;
        const uint8_t *attrs = nt + 0x3C0 + (cy >> 2) * 8;
        unsigned n = 32 - cx;
        if (n > left) n = left;
        left -= n;
        for (; n; n--, cx++, t++) {
            t->pattern = chr + names[cx] * 16u;
            t->pal = ((attrs[cx >> 2] >> (yshift | (cx & 2))) & 3) * 0x08080808u;
        }
        if (!left) break;
        cx = 0;
        half = other;
    }
}

#if NES_PPU_ASM
void nes_ppu_tiles_asm(const nes_tile *t, uint32_t *out, const uint32_t *expand, unsigned fine_y);
void nes_ppu_out1_asm(const uint8_t *group, uint16_t *o, const uint16_t *pairs53, unsigned misalign);
void nes_ppu_out2_asm(const uint8_t *group, uint16_t *o, const uint16_t *pairs53, unsigned misalign);
#endif

/* the tiles' indices into one line of a pair buffer (out: its first word) */
NES_LOOP static void nes_ppu_tiles(const nes_tile *t, uint32_t *out, unsigned fine_y) {
#if NES_PPU_ASM
    nes_ppu_tiles_asm(t, out, expand, fine_y);
#else
    const uint32_t *ex = expand;
    for (unsigned n = 33; n; n--, t++, out += 4) {
        const uint8_t *p = t->pattern + fine_y;
        const unsigned lo = p[0], hi = p[8];
        const uint32_t pal = t->pal;
        out[0] = ex[(lo >> 4) | (hi & 0xF0)] | pal;
        out[2] = ex[(lo & 15) | ((hi << 4) & 0xF0)] | pal;
    }
#endif
}

static void line_clear(uint8_t *line) {
    uint32_t *w = (uint32_t *)line;
    for (unsigned i = 0; i < PAIR_BYTES / 4; i += 2) w[i] = 0;
}

/* the background's indices along v into line (pair buffer + 4r): 33 tiles
 * from coarse x, the second nametable of the pair after the wrap */
static void nes_ppu_bg(const nes_ppu_t *ppu, uint8_t *line) {
    if (!ppu->chr) {
        line_clear(line);
        return;
    }
    const uint32_t key = (ppu->v & 0x0FFFu) | (ppu->ctrl & CTRL_BG_PAGE) << 8 | nt_writes << 13;
    if (key != tile_key) {
        nes_ppu_tile_row(ppu);
        tile_key = key;
    }
    nes_ppu_tiles(tile_row, (uint32_t *)line, (ppu->v >> 12) & 7);
    if (!(ppu->mask & MASK_BG_LEFT))
        for (unsigned s = ppu->x; s < ppu->x + 8u; s++) line[ROW_AT(s)] = 0;
}

/* sprite 0 against the background indices of line (before any sprite is
 * drawn into it) */
static void nes_ppu_hit(nes_ppu_t *ppu, unsigned line_y, const uint8_t *line) {
    const uint8_t *oam = ppu->oam;
    const uint8_t *p = sprite_pattern(ppu, oam, line_y - (oam[0] + 1u));
    unsigned lo = p[0], hi = p[8];
    if (oam[2] & 0x40) { lo = flip8[lo]; hi = flip8[hi]; }
    unsigned px = oam[3];
    for (unsigned s = ppu->x + px; px < 255 && s < ppu->x + oam[3] + 8u; s++, px++, lo <<= 1, hi <<= 1) {
        if (!(ppu->mask & MASK_SPR_LEFT) && px < 8) continue;
        if (((lo | hi) & 0x80) && (line[ROW_AT(s)] & 6)) {
            ppu->status |= ST_SPRITE0; /* the status bar's split */
            return;
        }
    }
}

static int sprite0_on(const nes_ppu_t *ppu, unsigned line) {
    const unsigned height = (ppu->ctrl & CTRL_SPR16) ? 16 : 8;
    return line - (ppu->oam[0] + 1u) < height;
}

/* the first eight sprites of the line in OAM order; drawn from the last so
 * that the highest index that shows wins (a pixel a sprite took stays its),
 * a sprite behind the background showing only where it is transparent */
NES_LOOP static void nes_ppu_sprites(nes_ppu_t *ppu, unsigned line_y, uint8_t *line) {
    unsigned list[8], n = 0;
    for (unsigned w = 0; w < 2 && n < 8; w++) {
        uint32_t m = spr_lines[line_y][w];
        unsigned s = w * 32;
        while (m && n < 8) {
            while (!(m & 0xFF)) { m >>= 8; s += 8; }
            if (m & 1) list[n++] = s;
            m >>= 1;
            s++;
        }
    }
    if (!n) return;
    if (list[0] == 0 && (ppu->mask & MASK_BG) && !(ppu->status & ST_SPRITE0)) nes_ppu_hit(ppu, line_y, line);
    const unsigned left = (ppu->mask & MASK_SPR_LEFT) ? 0 : 8;
    while (n--) {
        const uint8_t *oam = ppu->oam + list[n] * 4;
        const uint8_t *p = sprite_pattern(ppu, oam, line_y - (oam[0] + 1u));
        unsigned lo = p[0], hi = p[8];
        if (oam[2] & 0x40) { lo = flip8[lo]; hi = flip8[hi]; }
        const unsigned pal = 32u | (oam[2] & 3u) << 3, behind = oam[2] & 0x20;
        unsigned px = oam[3], s = ppu->x + px;
        for (unsigned bit = 0; bit < 8 && px < 256; bit++, px++, s++, lo <<= 1, hi <<= 1) {
            const unsigned c = ((lo >> 7) & 1) | ((hi >> 6) & 2);
            if (!c || px < left) continue;
            uint8_t *d = line + ROW_AT(s);
            const unsigned cur = *d;
            if (cur & 32) continue;
            if (behind && (cur & 6)) continue;
            *d = (uint8_t)(pal | c << 1);
        }
    }
}

/* a group is eight stream pixels from m (fine x & 3) in the word pairs at
 * g; GB(g, m, k, r) is pixel k of line r */
#define GB(g, m, k, r) ((g)[ROW_AT((m) + (k)) + 4u * (r)])
#define UNIFORM(w) ((w) == ((w) & 0xFFu) * 0x01010101u)

/* m0_box58_fast of one line's group, native */
static inline void group_box(const uint8_t *g, unsigned m, unsigned r, uint16_t *o) {
    const unsigned p1 = GB(g, m, 1, r), p6 = GB(g, m, 6, r);
    o[0] = PAIR(pair53, GB(g, m, 0, r), p1);
    o[1] = PAIR(pair53, GB(g, m, 2, r), p1);
    o[2] = PAIR(pair44, GB(g, m, 3, r), GB(g, m, 4, r));
    o[3] = PAIR(pair53, GB(g, m, 5, r), p6);
    o[4] = PAIR(pair53, GB(g, m, 7, r), p6);
}

/* the three word pairs of a group (two when the group is word aligned)
 * hold one byte repeated, on line 0 only (alone) or on both (pair) */
static inline int group_flat(const uint32_t *w, unsigned m, int pair) {
    const uint32_t c = w[0];
    if (!UNIFORM(c) || w[2] != c || (m && w[4] != c)) return 0;
    return !pair || (w[1] == c && w[3] == c && (!m || w[5] == c));
}
static inline int group_equal(const uint32_t *w, unsigned m) {
    return w[0] == w[1] && w[2] == w[3] && (!m || w[4] == w[5]);
}

/* a row alone: line 0 of the pair buffer from stream pixel x */
NES_LOOP static void nes_ppu_out1(unsigned x, uint16_t *o) {
    const unsigned m = x & 3;
    const uint8_t *g = pairs + ((x >> 2) << 3);
#if NES_PPU_ASM
    nes_ppu_out1_asm(g, o, pair53, m);
#else
    for (unsigned j = 0; j < 32; j++, g += 16, o += 5) {
        if (group_flat((const uint32_t *)g, m, 0)) {
            const uint16_t c = m0_panel(PAIR(pair53, g[0], g[0]));
            o[0] = c; o[1] = c; o[2] = c; o[3] = c; o[4] = c;
            continue;
        }
        group_box(g, m, 0, o);
        for (unsigned i = 0; i < 5; i++) o[i] = m0_panel(o[i]);
    }
#endif
}

/* a row of two lines sharing their fine x */
NES_LOOP static void nes_ppu_out2(unsigned x, uint16_t *o) {
    const unsigned m = x & 3;
    const uint8_t *g = pairs + ((x >> 2) << 3);
#if NES_PPU_ASM
    nes_ppu_out2_asm(g, o, pair53, m);
#else
    for (unsigned j = 0; j < 32; j++, g += 16, o += 5) {
        const uint32_t *w = (const uint32_t *)g;
        if (group_flat(w, m, 1)) {
            const uint16_t c = m0_panel(PAIR(pair53, g[0], g[0]));
            o[0] = c; o[1] = c; o[2] = c; o[3] = c; o[4] = c;
            continue;
        }
        group_box(g, m, 0, o);
        if (group_equal(w, m))
            for (unsigned i = 0; i < 5; i++) o[i] = m0_panel(o[i]);
        else {
            uint16_t b[5];
            group_box(g, m, 1, b);
            for (unsigned i = 0; i < 5; i++)
                o[i] = m0_panel((uint16_t)((o[i] & b[i]) + (((o[i] ^ b[i]) & 0xf7deu) >> 1)));
        }
    }
#endif
}

/* two lines whose fine x differ (a $2005 write between them): the general
 * case, by pixel */
static void nes_ppu_out2_split(unsigned xa, unsigned xb, uint16_t *o) {
    for (unsigned j = 0; j < 32; j++, o += 5) {
        uint16_t a[5], b[5];
        const unsigned sa = xa + 8 * j, sb = xb + 8 * j;
        group_box(pairs + ((sa >> 2) << 3), sa & 3, 0, a);
        group_box(pairs + ((sb >> 2) << 3), sb & 3, 1, b);
        for (unsigned i = 0; i < 5; i++)
            o[i] = m0_panel((uint16_t)((a[i] & b[i]) + (((a[i] ^ b[i]) & 0xf7deu) >> 1)));
    }
}

/* the line's indices into its half of the pair buffer: background,
 * backdrop when it is off, the sprites over it */
static void nes_ppu_indices(nes_ppu_t *ppu, unsigned line_y, uint8_t *line) {
    if (ppu->mask & MASK_BG) nes_ppu_bg(ppu, line);
    else line_clear(line);
    if ((ppu->mask & MASK_SPR) && ppu->chr) {
        if (ppu->oam_dirty) nes_ppu_sprite_lines(ppu);
        nes_ppu_sprites(ppu, line_y, line);
    }
}

/* the panel side of a visible line: the row of indices, the panel row once
 * its lines are in; a frame not shown only keeps sprite-0 */
static void nes_ppu_panel_line(nes_ppu_t *ppu, unsigned line_y, unsigned on) {
    if (ppu->skip || !ppu->panel) {
        if ((on & (MASK_BG | MASK_SPR)) == (MASK_BG | MASK_SPR) && !(ppu->status & ST_SPRITE0) &&
            ppu->chr && sprite0_on(ppu, line_y)) {
            nes_ppu_bg(ppu, probe);
            nes_ppu_hit(ppu, line_y, probe);
        }
        return;
    }
    const unsigned slot = panel_open;
    uint8_t *line = pairs + 4u * slot;
    if (on) nes_ppu_indices(ppu, line_y, line);
    else line_clear(line);
    row_x[slot] = ppu->x;
    if (ppu->panel_ends[line_y >> 5] >> (line_y & 31) & 1) {
        if (pal_dirty) nes_ppu_tables(ppu);
        if (!slot) nes_ppu_out1(row_x[0], ppu->panel_out);
        else if (row_x[0] == row_x[1]) nes_ppu_out2(row_x[0], ppu->panel_out);
        else nes_ppu_out2_split(row_x[0], row_x[1], ppu->panel_out);
        ppu->panel(ppu->host, panel_row);
    }
}

#ifdef NES_PPU_REFERENCE
static unsigned palette_color(nes_ppu_t *ppu, uint8_t index) {
    return nes_colors[ppu->palette[index] & 0x3F];
}

/* The background's tile expansion: two planar pixels and the tile's palette
 * go through one 64-entry table that holds the composed colour pair and the
 * palette-space index pair (0 = backdrop, else (pal << 2) | colour, what
 * sprite priority and sprite-0 test against).  Entry c of a palette p is
 * c = lo_pair | hi_pair << 2 | p << 4.  The tables are pure functions of the
 * palette RAM, rebuilt around the entries a $2007 write touches (four if the
 * written colour is one of the mirrored ones the renderer never reads). */
static uint32_t pair_rgb[64]; /* two RGB565 pixels, low pixel first */
static uint8_t pair_idx[64]; /* two palette-space indices, low nibble first */

static void pair_build(nes_ppu_t *ppu, unsigned c) {
    const unsigned pal = c >> 4, pair = c & 15;
    /* the four bits are two pixels' plane bits: bit1 left lo, bit0 right lo,
     * bit3 left hi, bit2 right hi (the walk takes its pairs MSB first) */
    const unsigned c0 = (pair >> 3 & 1) << 1 | (pair >> 1 & 1);
    const unsigned c1 = (pair >> 2 & 1) << 1 | (pair & 1);
    const unsigned i0 = c0 ? (pal << 2) | c0 : 0;
    const unsigned i1 = c1 ? (pal << 2) | c1 : 0;
    pair_idx[c] = (uint8_t)(i0 | (i1 << 4));
    pair_rgb[c] = nes_colors[ppu->palette[i0]] | nes_colors[ppu->palette[i1]] << 16;
}

static void pair_write(nes_ppu_t *ppu, unsigned i) {
    /* i: the palette-space index $2007 wrote (its mirrors already folded).
     * The sprite palettes (i >= $10) don't feed these tables — their colours
     * are read per pixel — and 4, 8 and 12 are never read by the renderer.
     * A colour sits in the pair's left pixel (bits 1 and 3 of the key) or
     * its right pixel (bits 0 and 2). */
    const unsigned col = i & 3;
    if (i >= 0x10 || !((i & 3) || i == 0)) return;
    unsigned base = col ? (i >> 2) << 4 : 0;
    const unsigned end = col ? base + 16 : 64;
    for (; base < end; base += 16) {
        if (col) {
            for (unsigned free_bits = 0; free_bits < 4; free_bits++) {
                pair_build(ppu, base | (col & 1) << 1 | (col >> 1) << 3 |
                                    (free_bits & 1) | (free_bits >> 1) << 2);
                pair_build(ppu, base | (col & 1) | (col >> 1) << 2 |
                                    (free_bits & 1) << 1 | (free_bits >> 1) << 3);
            }
        } else { /* the backdrop is colour 0 of every palette's table */
            for (unsigned free_bits = 0; free_bits < 4; free_bits++) {
                pair_build(ppu, base | (free_bits & 1) | (free_bits >> 1) << 2);
                pair_build(ppu, base | (free_bits & 1) << 1 | (free_bits >> 1) << 3);
            }
        }
    }
}

static void pair_build_all(nes_ppu_t *ppu) {
    for (unsigned c = 0; c < 64; c++) pair_build(ppu, c);
    pair_init = 1;
}

/* The reference line: the background's colour indices first (0 means the
 * backdrop), the sprites over them, sprite 0's hit against them.  bg_pack
 * holds the palette-space indices two to a byte for the 264 pixels the
 * fetch walk covers; the row keeps the same stream order. */
static void render_line(nes_ppu_t *ppu, unsigned line) {
    const int bg_on = (ppu->mask & MASK_BG) != 0;
    const int spr_on = (ppu->mask & MASK_SPR) != 0;
    static uint8_t bg_pack[132]; /* 264 pixels, two 4-bit indices a byte */
    const uint32_t backdrop = palette_color(ppu, 0);
    uint32_t *const row32 = (uint32_t *)ppu->row;

    if (!pair_init) pair_build_all(ppu);
    if (!bg_on) {
        const uint32_t both = backdrop | backdrop << 16;
        for (unsigned x = 0; x < 132; x++) row32[x] = both;
        for (unsigned b = 0; b < 132; b++) bg_pack[b] = 0;
    } else {
        /* the walk starts at v's nametable, coarse and fine position; the 33
         * fetched tiles form the pixel stream the fine x of $2005 shifts */
        uint8_t *const nt = ppu->nametable;
        const uint8_t *const chr = ppu->chr;
        const unsigned vm = ppu->vertical_mirror;
        uint32_t *out = row32;
        uint8_t *pack = bg_pack;
        uint16_t v = ppu->v;
        unsigned coarse_x = v & 0x1F;
        unsigned table = v & 0x0C00;
        const unsigned coarse_y = (v >> 5) & 0x1F;
        const unsigned fine_y = (v >> 12) & 7;
        const unsigned page = (ppu->ctrl & CTRL_BG_PAGE) ? 0x1000 : 0;
        for (unsigned t = 0; t < 33; t++) {
            const unsigned at = (unsigned)((0x2000 + table) | (coarse_y << 5) | coarse_x);
            const uint8_t tile = nt[vm ? at & 0x07FF : (at & 0x03FF) | ((at & 0x0800) >> 1)];
            const unsigned aa = (at & 0x2C00) | 0x03C0 | ((coarse_y & 0x1C) << 1) | ((coarse_x & 0x1C) >> 2);
            const uint8_t attr = nt[vm ? aa & 0x07FF : (aa & 0x03FF) | ((aa & 0x0800) >> 1)];
            const unsigned shift = ((coarse_x & 2) | ((coarse_y & 2) << 1));
            const unsigned c = ((attr >> shift) & 3) << 4;
            const unsigned pattern = page + tile * 16 + fine_y;
            const uint8_t lo = chr ? chr[pattern & 0x1FFF] : 0;
            const uint8_t hi = chr ? chr[(pattern + 8) & 0x1FFF] : 0;
            out[0] = pair_rgb[c | (lo >> 6 & 3) | (hi >> 6 & 3) << 2];
            out[1] = pair_rgb[c | (lo >> 4 & 3) | (hi >> 4 & 3) << 2];
            out[2] = pair_rgb[c | (lo >> 2 & 3) | (hi >> 2 & 3) << 2];
            out[3] = pair_rgb[c | (lo & 3) | (hi & 3) << 2];
            pack[0] = pair_idx[c | (lo >> 6 & 3) | (hi >> 6 & 3) << 2];
            pack[1] = pair_idx[c | (lo >> 4 & 3) | (hi >> 4 & 3) << 2];
            pack[2] = pair_idx[c | (lo >> 2 & 3) | (hi >> 2 & 3) << 2];
            pack[3] = pair_idx[c | (lo & 3) | (hi & 3) << 2];
            out += 4;
            pack += 4;
            if (++coarse_x == 32) { coarse_x = 0; table ^= 0x0400; }
        }
        /* the left column may be masked off (stream pixels x..x+7) */
        if (!(ppu->mask & MASK_BG_LEFT))
            for (unsigned s = ppu->x; s < ppu->x + 8; s++) {
                bg_pack[s >> 1] &= (s & 1) ? 0x0F : 0xF0;
                ppu->row[s] = backdrop;
            }
    }

    /* compose the sprites over the row, sprite 0's hit against the background */
    if (spr_on) {
        const unsigned height = (ppu->ctrl & CTRL_SPR16) ? 16 : 8;
        unsigned found = 0;
        for (unsigned sprite = 0; sprite < 64; sprite++) {
            const uint8_t *oam = ppu->oam + sprite * 4;
            const unsigned sy = oam[0] + 1; /* a sprite at y covers y+1.. */
            if (line < sy || line >= sy + height) continue;
            if (++found > 8) break; /* the over-the-limit drop, no overflow flag yet */
            const uint8_t *p = ppu->chr ? sprite_pattern(ppu, oam, line - sy) : 0;
            const uint8_t lo = p ? p[0] : 0;
            const uint8_t hi = p ? p[8] : 0;
            const unsigned flip_h = (oam[2] & 0x40) ? 7 : 0;
            const unsigned pal = (oam[2] & 3) << 2;
            uint16_t colors[4]; /* the sprite's palette, transparent is never read */
            for (unsigned idx = 1; idx < 4; idx++)
                colors[idx] = palette_color(ppu, (uint8_t)(0x10 | pal | idx));
            const int behind = (oam[2] & 0x20) != 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                const unsigned px = oam[3] + bit;
                if (px >= 256) break;
                if (!(ppu->mask & MASK_SPR_LEFT) && px < 8) continue;
                const unsigned shift_bit = flip_h ? bit : 7 - bit;
                const unsigned index = (((hi >> shift_bit) & 1) << 1) | ((lo >> shift_bit) & 1);
                if (!index) continue;
                const unsigned spx = px + ppu->x; /* the stream pixel behind it */
                const uint8_t pack = bg_pack[spx >> 1];
                const unsigned bg_index = (spx & 1) ? pack >> 4 : pack & 15;
                if (sprite == 0 && bg_index && px < 255 && bg_on)
                    ppu->status |= ST_SPRITE0; /* the status bar's split */
                if (!behind || !bg_index)
                    ppu->row[spx] = colors[index];
            }
        }
    }
}

/* the reference line first (the panel row it closes is checked against
 * it), then its sprite-0 is compared with the panel renderer's */
static uint8_t reference_line(nes_ppu_t *ppu, unsigned line) {
    const uint8_t status_before = ppu->status;
    if (ppu->mask & (MASK_BG | MASK_SPR)) render_line(ppu, line);
    else
        for (unsigned x = 0; x < 264; x++) ppu->row[x] = (uint16_t)palette_color(ppu, 0);
    ppu->render_row(ppu->host, line);
    const uint8_t status = ppu->status;
    ppu->status = status_before;
    return status;
}
#endif

int nes_ppu_line(nes_ppu_t *ppu, unsigned line) {
    if (line == 241) {
        ppu->status |= ST_VBLANK;
        return (ppu->ctrl & CTRL_NMI) ? 1 : 0;
    }
    if (line == 261) {
        ppu->status &= (uint8_t)~(ST_VBLANK | ST_SPRITE0);
        /* the pre-render line reloads v from t only while rendering is on:
         * with the screen off, v keeps whatever $2006 set, which is how
         * Super Mario Bros streams a column across the frame boundary */
        if (ppu->mask & (MASK_BG | MASK_SPR)) ppu->v = ppu->t;
        ppu->frame++;
        return 0;
    }
    if (line >= 240) return 0;
    if (line == 0) panel_row = panel_open = 0;
    const unsigned on = ppu->mask & (MASK_BG | MASK_SPR);
#ifdef NES_PPU_REFERENCE
    const uint8_t reference = ppu->render_row ? reference_line(ppu, line) : 0;
    nes_ppu_panel_line(ppu, line, on);
    if (ppu->render_row && ppu->status != reference) {
        ppu->hit_mismatch++;
        ppu->status = reference;
    }
#else
    nes_ppu_panel_line(ppu, line, on);
#endif
    if (on) {
        /* fine y, then coarse y with its wraps */
        if ((ppu->v & 0x7000) != 0x7000) ppu->v = (uint16_t)(ppu->v + 0x1000);
        else {
            ppu->v &= (uint16_t)~0x7000;
            unsigned y = (ppu->v >> 5) & 0x1F;
            if (y == 29) { y = 0; ppu->v ^= 0x0800; }
            else if (y == 31) y = 0;
            else y++;
            ppu->v = (uint16_t)((ppu->v & ~0x03E0) | (y << 5));
        }
        /* the horizontal bits reload from t at the line's end */
        ppu->v = (uint16_t)((ppu->v & ~0x041F) | (ppu->t & 0x041F));
    }
    /* the vertical plan (ppu.h): a row ends on each set bit */
    if (ppu->panel_ends[line >> 5] >> (line & 31) & 1) {
        panel_row++;
        panel_open = 0;
    } else
        panel_open = 1;
    return 0;
}
