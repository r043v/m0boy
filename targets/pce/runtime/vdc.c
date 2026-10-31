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
 * address (16 entries, direct mapped — mednafen keeps the same per-tile
 * cache).  A VRAM write drops the matching entry; $FFFF is the free tag
 * (a row's address tops at $FFF7). */
static uint16_t cell_cache_addr[16];
static uint32_t cell_cache_bits[16];

/* the sprite pattern-row cache, the same idea for the 4-plane rows the
 * fetch reads (16 pixels, 64 bits of packed nibbles): the same row comes
 * back every frame while a sprite holds still, and the per-pixel spread
 * expansion ran 256 times a line.  512 entries, direct mapped on
 * (address >> 2); a row's address tops at $FFCF. */
#define SPRITE_CACHE 512
static uint16_t sprite_cache_addr[SPRITE_CACHE];
static uint32_t sprite_cache_bits[SPRITE_CACHE][2];

static void vram_cache_drop(unsigned address) {
    cell_cache_addr[(address >> 3) & 15] = 0xFFFFu;
    sprite_cache_addr[(address >> 2) & (SPRITE_CACHE - 1)] = 0xFFFFu;
}

/* one sprite row's 16 nibbles, leftmost pixel first (the draw's own
 * non-flipped order: pixel px is word bit 15-px of the four planes) */
static void sprite_pack_row(const uint16_t w[4], uint32_t out[2]) {
    uint32_t lo = 0, hi = 0;
    for (unsigned px = 0; px < 16; px++) {
        const unsigned sh = 15 - px;
        const unsigned n = ((w[0] >> sh) & 1) | (((w[1] >> sh) & 1) << 1) |
            (((w[2] >> sh) & 1) << 2) | (((w[3] >> sh) & 1) << 3);
        if (px < 8) lo |= n << (4 * px); else hi |= n << (4 * (px - 8));
    }
    out[0] = lo;
    out[1] = hi;
}

/* the hflip: all 16 nibbles reversed (32-bit halves first — skipping
 * that step only mirrors each 8-nibble half in place) */
static uint64_t sprite_nibrev(uint64_t x) {
    x = (x & 0x00000000FFFFFFFFull) << 32 | (x & 0xFFFFFFFF00000000ull) >> 32;
    x = (x & 0x0000FFFF0000FFFFull) << 16 | (x & 0xFFFF0000FFFF0000ull) >> 16;
    x = (x & 0x00FF00FF00FF00FFull) << 8 | (x & 0xFF00FF00FF00FF00ull) >> 8;
    x = (x & 0x0F0F0F0F0F0F0F0Full) << 4 | (x & 0xF0F0F0F0F0F0F0F0ull) >> 4;
    return x;
}

static void draw_bg(vdc_t *v) {
    const unsigned bat_width = bat_width_tab[(v->mwr >> 4) & 3];
    const unsigned bat_width_mask = bat_width - 1;
    const unsigned bat_width_shift = bat_width_shift_tab[(v->mwr >> 4) & 3];
    const unsigned bat_height_mask = bat_height_tab[(v->mwr >> 6) & 1] - 1;
    const unsigned width = v->row_width;
    unsigned bx = v->bxr;
    const unsigned bat_y = ((v->bg_y >> 3) & bat_height_mask) << bat_width_shift;
    const unsigned line_sub = v->bg_y & 7;
    unsigned skip = bx & 7; /* the scroll offset inside the line's first cell */

    for (unsigned x = 0; x < width;) {
        const uint16_t bat = v->vram[((bx >> 3) & bat_width_mask) | bat_y];
        const unsigned address = (bat & 0x0FFF) * 16 + line_sub;
        const unsigned slot = (address >> 3) & 15;
        uint32_t s;
        if (cell_cache_addr[slot] == address) {
            s = cell_cache_bits[slot];
        } else {
            const uint16_t w0 = vram_read(v, address);
            const uint16_t w1 = vram_read(v, address + 8);
            s = cell_nibbles(w0, w1);
            cell_cache_addr[slot] = (uint16_t)address;
            cell_cache_bits[slot] = s;
        }
        const uint16_t palette = (bat >> 8) & 0xF0;
        s >>= 4 * skip;
        unsigned count = 8 - skip;
        if (count > width - x) count = width - x;
        for (unsigned i = 0; i < count; i++, s >>= 4)
            v->linebuf[x + i] = (uint16_t)(palette | (s & 0xF));
        x += count;
        bx += count;
        skip = 0;
    }
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
            const unsigned slot = (pattern >> 2) & (SPRITE_CACHE - 1);
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
    uint16_t sprite_line[1024];
    const unsigned width = v->row_width;
    unsigned first = width, end = 0;

    for (int i = 0; i < active_sprites; i++) {
        const int pos = (int)sprite_list[i].x - 0x20;
        if (pos >= (int)width || pos <= -16) continue;
        const unsigned left = pos > 0 ? (unsigned)pos : 0;
        const unsigned right = pos + 16 < (int)width ? (unsigned)(pos + 16) : width;
        if (left < first) first = left;
        if (right > end) end = right;
    }
    if (first >= end) return;

    memset(sprite_line + first, 0, (end - first) * sizeof sprite_line[0]);
    /* backwards: the first sprite of the list paints last and wins */
    for (int i = active_sprites - 1; i >= 0; i--) {
        const unsigned flags = sprite_list[i].flags;
        const int pos = (int)sprite_list[i].x - 0x20;
        const unsigned prio_or = (flags & SPRF_PRIORITY) ? 0x200 : 0;
        const unsigned raw_or = sprite_list[i].palette | 0x100 | prio_or;
        const int collide = (flags & SPRF_SPRITE0) && (v->cr & 0x01);
        /* the pixels land in growing tx: skip the left clip, stop at the
         * right one (the per-pixel bounds of the reference stay true) */
        const int left = pos > 0 ? pos : 0;
        const int right = pos + 16 < (int)width ? pos + 16 : (int)width;
        const unsigned hflip = flags & SPRF_HFLIP;
        const uint64_t row = (uint64_t)sprite_list[i].packed[0] |
            (uint64_t)sprite_list[i].packed[1] << 32;
        const uint64_t nibbles = hflip ? sprite_nibrev(row) : row;
        for (unsigned px = 0; px < 16; px++) {
            const int tx = pos + (int)px;
            if (tx >= right) break; /* the pixels only grow */
            const unsigned raw = (unsigned)(nibbles >> (4 * px)) & 15;
            if (!raw) continue;
            if (tx < left) continue;
            if (collide && (sprite_line[tx] & 0xF)) v->status |= VDCS_CR;
            sprite_line[tx] = (uint16_t)(raw | raw_or);
        }
    }
    /* a sprite pixel wins over a transparent BG pixel, or with the
     * priority flag over any BG pixel */
    for (unsigned x = first; x < end; x++)
        if ((sprite_line[x] & 0xF) && (!(v->linebuf[x] & 0xF) || (sprite_line[x] & 0x200)))
            v->linebuf[x] = sprite_line[x] & 0x1FF;
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
