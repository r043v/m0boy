#include "vdc.h"

/* the row composer is the runtime's hottest loop on the ESP32 target;
 * the pragma lifts it past the project-wide -Os */
#pragma GCC optimize ("O2")

#include <string.h>

/* the vertical phases, in the mednafen core's order */
enum { VP_VSW, VP_VDS, VP_VDW, VP_VCR };

/* the VRAM's auto-increment, from CR bits 11-12 */
static const uint16_t vram_inc_tab[4] = { 1, 32, 64, 128 };

/* the BAT's virtual size, from MWR bits 4-6 */
static const unsigned bat_width_tab[4] = { 32, 64, 128, 128 };
static const unsigned bat_width_shift_tab[4] = { 5, 6, 7, 7 };
static const unsigned bat_height_tab[2] = { 32, 64 };

/* the sprites: up to 16 a line, 16 or 32 wide, 16/32/64 high */
#define SPRF_PRIORITY 0x0080
#define SPRF_HFLIP    0x0800
#define SPRF_VFLIP    0x8000
#define SPRF_SPRITE0  0x10000

static const unsigned sprite_height_tab[4] = { 16, 32, 64, 64 };
static const unsigned sprite_height_no_mask[4] = { 0xFFFFu, 0xFFFDu, 0xFFF9u, 0xFFF9u };
static const unsigned sprite_width_tab[2] = { 16, 32 };

#define M_vdc_VSW ((v->vsr >> 0) & 0x1F)
#define M_vdc_VDS ((v->vsr >> 8) & 0xFF)
#define M_vdc_VDW ((v->vdr >> 0) & 0x1FF)
#define M_vdc_VCR ((v->vcr >> 0) & 0xFF)

typedef struct {
    uint16_t x, palette;
    uint32_t flags; /* includes the internal sprite-0 marker above bit 15 */
    uint32_t packed[2]; /* the row's 16 nibbles, leftmost pixel first */
} sprite_t;

static sprite_t sprite_list[16];
static int active_sprites;

/* Spread a four-bit plane across four pixel nibbles, leftmost bit first. */
static const uint16_t spread4[16] = {
    0x0000, 0x1000, 0x0100, 0x1100,
    0x0010, 0x1010, 0x0110, 0x1110,
    0x0001, 0x1001, 0x0101, 0x1101,
    0x0011, 0x1011, 0x0111, 0x1111
};

/* one 8x8 cell's pixel: the planes at char*16 (01) and char*16+8 (23),
 * bit 0 of the words is the cell's rightmost pixel */
static uint16_t vram_read(const vdc_t *v, unsigned address) {
    /* The registers have 16-bit addresses, but only $0000-$7FFF is fitted. */
    return address < VDC_VRAM_WORDS ? v->vram[address] : 0;
}

/* one cell's eight pixels as eight nibbles, leftmost pixel in the lowest
 * nibble: each plane byte spreads through spread4 (the low byte's nibbles
 * land on pixels 0-3, the high byte's on 4-7 — the same table serves both
 * halves), the four planes then stack into one nibble without carries */
static uint32_t cell_nibbles(uint16_t w0, uint16_t w1) {
    const uint32_t s0 = spread4[(w0 >> 4) & 0xF] | (uint32_t)spread4[w0 & 0xF] << 16;
    const uint32_t s1 = spread4[(w0 >> 12) & 0xF] | (uint32_t)spread4[(w0 >> 8) & 0xF] << 16;
    const uint32_t s2 = spread4[(w1 >> 4) & 0xF] | (uint32_t)spread4[w1 & 0xF] << 16;
    const uint32_t s3 = spread4[(w1 >> 12) & 0xF] | (uint32_t)spread4[(w1 >> 8) & 0xF] << 16;
    return s0 | s1 << 1 | s2 << 2 | s3 << 3;
}

/* the bg pattern-row cache: the same tile row renders on many lines and
 * many cells of a line, so the nibbles are keyed by the pattern row's
 * address.  256 entries, direct mapped on (line in the tile, tile low five
 * bits): the 32 tiles of a scan line land on 32 distinct slots.  A pattern
 * row is two words (planes 0-1 at the row's address, planes 2-3 eight words
 * on), so a VRAM write names exactly one row, `address & ~8`, and drops
 * it; $FFFF is the free tag (a row's address tops at $FFF7). */
#define CELL_CACHE 256
#define CELL_SLOT(row) (((row) & 7u) | (((row) >> 1) & 0xF8u))
static uint16_t cell_cache_addr[CELL_CACHE];
static uint32_t cell_cache_bits[CELL_CACHE];

/* the sprite pattern-row cache, the same idea for the 4-plane rows the
 * fetch reads (16 pixels, 64 bits of packed nibbles): the same row comes
 * back every frame while a sprite holds still.  Direct mapped on (row in
 * the pattern, pattern low bits), so the 16 rows of one pattern take 16
 * distinct slots.  A row's words sit 16 apart (planes 0-3), so a VRAM
 * write names the row `address & ~0x30`.  A row's address tops at $FFCF.
 * The size is the target's RAM to spend. */
#ifndef PCE_SPRITE_CACHE
#ifdef PCE_RENDER_THREAD
#define PCE_SPRITE_CACHE 512
#else
#define PCE_SPRITE_CACHE 2048
#endif
#endif
#define SPRITE_CACHE PCE_SPRITE_CACHE
#define SPRITE_SLOT(row) (((row) & 15u) | (((row) >> 2) & (unsigned)(SPRITE_CACHE - 16)))
static uint16_t sprite_cache_addr[SPRITE_CACHE];
static uint32_t sprite_cache_bits[SPRITE_CACHE][2];

static void vram_cache_drop(unsigned address) {
    cell_cache_addr[CELL_SLOT(address & ~8u)] = 0xFFFFu;
    sprite_cache_addr[SPRITE_SLOT(address & ~0x30u)] = 0xFFFFu;
}

/* one sprite row's 16 nibbles, leftmost pixel first (the draw's own
 * non-flipped order: pixel px is word bit 15-px of the four planes) */
static void sprite_pack_row(const uint16_t w[4], uint32_t out[2]) {
    /* each plane spreads to nibble lanes by the same table the bg uses */
    uint32_t lo = 0, hi = 0;
    for (unsigned plane = 0; plane < 4; plane++) {
        const uint32_t x = w[plane];
        /* pixel px (0 = leftmost) is bit 15-px: the high byte holds
         * pixels 0-7, spread4 turns four bits into four nibbles with the
         * first bit in the lowest nibble */
        const uint32_t h = (uint32_t)spread4[(x >> 12) & 0xF] | (uint32_t)spread4[(x >> 8) & 0xF] << 16;
        const uint32_t l = (uint32_t)spread4[(x >> 4) & 0xF] | (uint32_t)spread4[x & 0xF] << 16;
        lo |= h << plane;
        hi |= l << plane;
    }
    out[0] = lo;
    out[1] = hi;
}

/* the hflip: the 8 nibbles of a half reversed */
static uint32_t nibrev32(uint32_t x) {
    x = x << 16 | x >> 16;
    x = (x & 0x00FF00FFu) << 8 | (x & 0xFF00FF00u) >> 8;
    x = (x & 0x0F0F0F0Fu) << 4 | (x & 0xF0F0F0F0u) >> 4;
    return x;
}

/* a byte of nibbles as two 16-bit pixels (the low nibble first), so the
 * background writes a cell as four 32-bit stores */
static uint32_t nib2[256];
static int nib2_ready;
static void nib2_init(void) {
    for (unsigned b = 0; b < 256; b++) nib2[b] = (b & 0xFu) | (uint32_t)(b >> 4) << 16;
    nib2_ready = 1;
}

typedef uint32_t __attribute__((may_alias)) u32a;

static void draw_bg(vdc_t *v) {
    if (!nib2_ready) nib2_init();
    const unsigned bat_width = bat_width_tab[(v->mwr >> 4) & 3];
    const unsigned bat_width_mask = bat_width - 1;
    const unsigned bat_width_shift = bat_width_shift_tab[(v->mwr >> 4) & 3];
    const unsigned bat_height_mask = bat_height_tab[(v->mwr >> 6) & 1] - 1;
    const unsigned width = v->row_width;
    const unsigned bx = v->bxr;
    const unsigned bat_y = ((v->bg_y >> 3) & bat_height_mask) << bat_width_shift;
    const unsigned line_sub = v->bg_y & 7;
    const unsigned skip = bx & 7; /* the scroll offset inside the line's first cell */
    /* whole cells land on 8-pixel boundaries (4-byte aligned): a scrolled
     * line draws its cells in a scratch row and copies the window out */
    uint16_t scratch[1032] __attribute__((aligned(4)));
    uint16_t *const out = skip ? scratch : v->linebuf;
    const unsigned cells = (width + skip + 7) >> 3;
    unsigned cx = bx >> 3;

    for (unsigned c = 0; c < cells; c++, cx++) {
        const uint16_t bat = v->vram[(cx & bat_width_mask) | bat_y];
        const unsigned address = (bat & 0x0FFF) * 16 + line_sub;
        const unsigned slot = CELL_SLOT(address);
        uint32_t s;
        if (cell_cache_addr[slot] == address) {
            s = cell_cache_bits[slot];
        } else {
            s = cell_nibbles(vram_read(v, address), vram_read(v, address + 8));
            cell_cache_addr[slot] = (uint16_t)address;
            cell_cache_bits[slot] = s;
        }
        const uint32_t pal2 = (uint32_t)((bat >> 8) & 0xF0) * 0x00010001u;
        u32a *d = (u32a *)(out + c * 8);
        d[0] = nib2[s & 255] | pal2;
        d[1] = nib2[(s >> 8) & 255] | pal2;
        d[2] = nib2[(s >> 16) & 255] | pal2;
        d[3] = nib2[s >> 24] | pal2;
    }
    if (skip) memcpy(v->linebuf, scratch + skip, width * sizeof v->linebuf[0]);
}

/* the SAT walk: the sprites of this line, 16 at most, a 32-wide one as
 * two halves */
static void fetch_sprites(vdc_t *v) {
    active_sprites = 0;
    /* RCRCount has already advanced for this line, as in the mednafen
     * core's line fetch phase */
    const int sprite_line = v->rcr_count;
    const uint16_t *sp = v->sat;
    for (unsigned i = 0; i < 64; i++, sp += 4) {
        const int y = (int)(sp[0] & 0x3FF) - 0x40;
        /* most of the 64 miss this line: 64 rows is the tallest sprite */
        if ((unsigned)(sprite_line - y) >= 64u) continue;
        uint16_t x = sp[1] & 0x3FF;
        uint16_t no = (sp[2] >> 1) & 0x3FF;
        const uint16_t flags = sp[3];
        const unsigned palette = (flags & 0xF) << 4;
        const unsigned height = sprite_height_tab[(flags >> 12) & 3];
        const unsigned width = sprite_width_tab[(flags >> 8) & 1];

        if (sprite_line < y || sprite_line >= (int)(y + height))
            continue;

        for (unsigned half = 0; half < (width == 32 ? 2u : 1u); half++) {
            if (active_sprites == 16) {
                if (v->cr & 0x02) v->status |= VDCS_OR;
                return; /* the 16-sprite line limit */
            }
            unsigned y_offset = (unsigned)(sprite_line - y);
            if (flags & SPRF_VFLIP) y_offset = height - 1 - y_offset;

            unsigned no_half = no & sprite_height_no_mask[(flags >> 12) & 3];
            no_half |= (y_offset & 0x30) >> 3;
            if (width == 32) no_half &= ~1u;
            if (half) no_half |= 1;
            if ((flags & SPRF_HFLIP) && width == 32) no_half ^= 1;

            sprite_list[active_sprites].x = x;
            sprite_list[active_sprites].palette = palette;
            sprite_list[active_sprites].flags = flags | (i ? 0 : SPRF_SPRITE0);
            const unsigned pattern = no_half * 64 + (y_offset & 15);
            const unsigned slot = SPRITE_SLOT(pattern);
            uint32_t pk[2];
            if (sprite_cache_addr[slot] == pattern) {
                pk[0] = sprite_cache_bits[slot][0];
                pk[1] = sprite_cache_bits[slot][1];
            } else {
                const uint16_t w[4] = { vram_read(v, pattern), vram_read(v, pattern + 16),
                    vram_read(v, pattern + 32), vram_read(v, pattern + 48) };
                sprite_pack_row(w, pk);
                sprite_cache_addr[slot] = (uint16_t)pattern;
                sprite_cache_bits[slot][0] = pk[0];
                sprite_cache_bits[slot][1] = pk[1];
            }
            sprite_list[active_sprites].packed[0] = pk[0];
            sprite_list[active_sprites].packed[1] = pk[1];
            active_sprites++;

            x += 16; /* the second half sits 16 pixels to the right */
        }
    }
}

static void draw_sprites(vdc_t *v) {
    if (!active_sprites) return;
    /* the sprites compose into a scratch row first (the first of the list
     * wins), 16-pixel blocks at a time: only the blocks a sprite touches
     * are cleared, painted and merged into the background */
    static uint16_t sprite_line[1056] __attribute__((aligned(4)));
    const unsigned width = v->row_width;
    uint64_t touched = 0;

    /* backwards: the first sprite of the list paints last and wins */
    for (int i = active_sprites - 1; i >= 0; i--) {
        uint32_t lo = sprite_list[i].packed[0], hi = sprite_list[i].packed[1];
        if (!(lo | hi)) continue; /* paints nothing */
        const unsigned flags = sprite_list[i].flags;
        const int pos = (int)sprite_list[i].x - 0x20;
        const int left = pos > 0 ? pos : 0;
        const int right = pos + 16 < (int)width ? pos + 16 : (int)width;
        if (left >= right) continue;
        for (unsigned blk = (unsigned)left >> 4; blk <= (unsigned)(right - 1) >> 4; blk++) {
            if (touched >> blk & 1) continue;
            touched |= 1ull << blk;
            memset(sprite_line + blk * 16, 0, 16 * sizeof sprite_line[0]);
        }
        if (flags & SPRF_HFLIP) { const uint32_t t = nibrev32(hi); hi = nibrev32(lo); lo = t; }
        const unsigned prio_or = (flags & SPRF_PRIORITY) ? 0x200 : 0;
        const unsigned raw_or = sprite_list[i].palette | 0x100 | prio_or;
        const int collide = (flags & SPRF_SPRITE0) && (v->cr & 0x01);
        /* the pixels land in growing tx: only the clipped span draws (the
         * per-pixel bounds of the reference stay true) */
        const unsigned p0 = (unsigned)(left - pos), p1 = (unsigned)(right - pos);
        uint16_t *dst = sprite_line + pos;
        for (unsigned px = p0; px < p1; px++) {
            const unsigned raw = ((px < 8 ? lo >> (4 * px) : hi >> (4 * (px - 8)))) & 15;
            const uint16_t cur = dst[px];
            if (collide && raw && (cur & 0xF)) v->status |= VDCS_CR;
            dst[px] = raw ? (uint16_t)(raw | raw_or) : cur;
        }
    }
    /* a sprite pixel wins over a transparent BG pixel, or with the
     * priority flag over any BG pixel — two pixels a step */
    while (touched) {
        const unsigned blk = (unsigned)__builtin_ctzll(touched);
        touched &= touched - 1;
        unsigned x = blk * 16, end = x + 16;
        if (end > width) end = width;
        for (; x < end; x += 2) {
            const uint32_t sp = *(const u32a *)(sprite_line + x);
            u32a *bgp = (u32a *)(v->linebuf + x);
            const uint32_t bg = *bgp;
            const uint32_t st = sp & 0x000F000Fu, bt = bg & 0x000F000Fu;
            const uint32_t snz = (st | st >> 1 | st >> 2 | st >> 3) & 0x00010001u;
            const uint32_t bz = ~(bt | bt >> 1 | bt >> 2 | bt >> 3) & 0x00010001u;
            const uint32_t take = snz & (bz | ((sp >> 9) & 0x00010001u));
            const uint32_t m = take * 0xFFFFu;
            *bgp = (bg & ~m) | (sp & 0x01FF01FFu & m);
        }
    }
}

static void run_vram_dma(vdc_t *v) {
    /* the whole transfer at once, as the mednafen core's force path */
    uint16_t src = v->sour, dst = v->desr;
    const int sdir = (v->dcr & 0x04) ? -1 : 1;
    const int ddir = (v->dcr & 0x08) ? -1 : 1;
    uint16_t len = v->lenr;
    do {
        if (dst < VDC_VRAM_WORDS) {
            v->vram[dst] = vram_read(v, src);
            vram_cache_drop(dst);
        }
        src = (uint16_t)(src + sdir);
        dst = (uint16_t)(dst + ddir);
        len--;
    } while (len != 0xFFFF);
    v->sour = src;
    v->desr = dst;
    if (v->dcr & 0x02) v->status |= VDCS_DV;
}

static void run_sat_dma(vdc_t *v) {
    if (v->dvssr < VDC_VRAM_WORDS) {
        unsigned len = 256;
        if (v->dvssr > VDC_VRAM_WORDS - 0x100) len = VDC_VRAM_WORDS - v->dvssr;
        memcpy(v->sat, &v->vram[v->dvssr], len * sizeof(uint16_t));
    }
    if (v->dcr & 0x01) v->status |= VDCS_DS;
}

void vdc_reset(vdc_t *v) {
    if (!nib2_ready) nib2_init();
    memset(v->vram, 0, sizeof v->vram);
    memset(v->sat, 0, sizeof v->sat);
    memset(cell_cache_addr, 0xFF, sizeof cell_cache_addr);
    memset(sprite_cache_addr, 0xFF, sizeof sprite_cache_addr);
    v->select = 0;
    v->mawr = v->marr = 0;
    v->cr = v->rcr = v->bxr = v->byr = v->mwr = 0;
    v->hsr = v->hdr = v->vsr = v->vdr = v->vcr = 0;
    v->dcr = v->sour = v->desr = v->lenr = v->dvssr = 0;
    v->read_buffer = 0xFFFF;
    v->write_latch = 0;
    v->status = 0;
    v->vphase = VP_VSW;
    v->vcounter = 1;
    v->rcr_count = 0;
    v->burst_mode = 1;
    v->bg_y = 0;
    v->vb_test = v->sat_test = 0;
    v->sat_pending = 0;
    v->row_width = 256;
    v->active_width = v->active_height = 0;
    v->active_started = 0;
}

uint8_t vdc_read(vdc_t *v, unsigned at) {
    switch (at & 3) {
    case 0: { /* the status, read once and cleared */
        const uint8_t ret = v->status;
        v->status &= (uint8_t)~0x3F;
        return ret;
    }
    case 2: return (uint8_t)v->read_buffer;
    case 3:
        if (v->select == 0x02) { /* VRR: the word, then the next one */
            const uint8_t ret = (uint8_t)(v->read_buffer >> 8);
            v->read_buffer = vram_read(v, v->marr);
            v->marr = (uint16_t)(v->marr + vram_inc_tab[(v->cr >> 11) & 3]);
            return ret;
        }
        return (uint8_t)(v->read_buffer >> 8);
    default: return 0;
    }
}

void vdc_write(vdc_t *v, unsigned at, uint8_t value) {
    unsigned msb;
    switch (at & 3) {
    case 0: v->select = value & 0x1F; return;
    case 2: msb = 0; break;
    case 3: msb = 1; break;
    default: return;
    }
    /* the registers take each byte at once (the mednafen core's
     * VDC_REGSETP); only the VRAM port waits for the high byte */
    #define REGSETP(reg) do { reg &= (uint16_t)(msb ? 0x00FFu : 0xFF00u); \
        reg |= (uint16_t)((unsigned)value << (msb ? 8 : 0)); } while (0)
    switch (v->select) {
    case 0x00: REGSETP(v->mawr); break;
    case 0x01: /* MARR: the read goes off on the high byte */
        REGSETP(v->marr);
        if (msb) {
            v->read_buffer = vram_read(v, v->marr);
            v->marr = (uint16_t)(v->marr + vram_inc_tab[(v->cr >> 11) & 3]);
        }
        break;
    case 0x02: /* the VRAM port */
        if (!msb) v->write_latch = value;
        else {
            if (v->mawr < VDC_VRAM_WORDS) {
                v->vram[v->mawr] = (uint16_t)(v->write_latch | value << 8);
                vram_cache_drop(v->mawr);
            }
            v->mawr = (uint16_t)(v->mawr + vram_inc_tab[(v->cr >> 11) & 3]);
        }
        break;
    case 0x05: REGSETP(v->cr); break;
    case 0x06: REGSETP(v->rcr); v->rcr &= 0x3FF; break;
    case 0x07: REGSETP(v->bxr); v->bxr &= 0x3FF; break;
    case 0x08: REGSETP(v->byr); v->byr &= 0x1FF; v->bg_y = v->byr; break;
    case 0x09: REGSETP(v->mwr); break;
    case 0x0A: REGSETP(v->hsr); break;
    case 0x0B: REGSETP(v->hdr); break;
    case 0x0C: REGSETP(v->vsr); break;
    case 0x0D: REGSETP(v->vdr); break;
    case 0x0E: REGSETP(v->vcr); break;
    case 0x0F: REGSETP(v->dcr); break;
    case 0x10: REGSETP(v->sour); break;
    case 0x11: REGSETP(v->desr); break;
    case 0x12: /* LENR: the high byte starts the DMA, run to completion */
        REGSETP(v->lenr);
        if (msb) run_vram_dma(v);
        break;
    case 0x13: REGSETP(v->dvssr); v->sat_pending = 1; break;
    default: break;
    }
    #undef REGSETP
}

void vdc_line(vdc_t *v, unsigned line) {
    v->row_width = (unsigned)(((v->hdr & 0x7F) + 1) * 8);

    if (line == 0) { /* the frame's start: the phase machine rewinds */
        v->vphase = VP_VSW;
        v->vcounter = M_vdc_VSW + 1;
        v->rcr_count = 0;
        v->bg_y = v->byr;
        v->vb_test = 0;
        v->sat_test = 0;
        v->active_started = 0;
        v->active_width = v->row_width;
        v->active_height = (unsigned)(M_vdc_VDW + 1);
        if (v->active_height > 242) v->active_height = 242;
    } else {
        v->vcounter--;
        if (v->vcounter <= 0) {
            v->vphase = (v->vphase + 1) & 3;
            switch (v->vphase) {
            case VP_VDS: v->vcounter = M_vdc_VDS + 2; break;
            case VP_VDW:
                v->vcounter = M_vdc_VDW + 1;
                v->rcr_count = 0;
                v->burst_mode = !(v->cr & 0xC0);
                v->vb_test = 1;
                v->sat_test = 1;
                break;
            case VP_VCR: v->vcounter = M_vdc_VCR; break;
            case VP_VSW: v->vcounter = M_vdc_VSW + 1; break;
            }
        }
        v->rcr_count++;

        /* the SATB DMA and the vblank flag land at the first line after
         * the active area, where the display would start */
        if (v->vphase != VP_VDW && v->sat_test) {
            v->sat_test = 0;
            if (v->sat_pending || (v->dcr & 0x10)) {
                v->sat_pending = 0;
                run_sat_dma(v);
            } else {
                v->burst_mode = 1;
            }
        }
        if (v->vphase != VP_VDW && v->vb_test) {
            v->vb_test = 0;
            if (v->cr & 0x08) v->status |= VDCS_VD;
        }

        if (v->vphase == VP_VDW && !v->burst_mode)
            fetch_sprites(v);

        if ((int)v->rcr_count == (int)v->rcr - 0x40 && (v->cr & 0x04))
            v->status |= VDCS_RR;
    }

    /* the row: composed where the active area would start; the BYR latch
     * uses the line's count (0 on the first active line: the register) */
    if (!v->burst_mode && v->vphase == VP_VDW) {
        v->bg_y = v->rcr_count == 0 ? (int)v->byr : v->bg_y + 1;
        if (v->cr & 0x80) draw_bg(v);
        else memset(v->linebuf, 0, v->row_width * sizeof v->linebuf[0]);
        if (v->cr & 0x40)
            draw_sprites(v);
        v->active_started = 1;
    } else {
        memset(v->linebuf, 0, v->row_width * sizeof v->linebuf[0]);
    }
    active_sprites = 0;
}
