#include "samd21.h"
#include <string.h>

/* Gamebuino META board model.  Register semantics follow the SAMD21
 * datasheet and what the META firmwares rely on (lib and homebrew); every
 * register the recorded guest profile touches is covered here. */

enum { IRQ_DMAC = 6, IRQ_TC4 = 19, IRQ_TC5 = 20 };
enum { TRIG_SERCOM4_RX = 0x09, TRIG_SERCOM4_TX = 0x0a, TRIG_TC4 = 0x1b, TRIG_TC5 = 0x1c };
#define LINE_MASK ((1u << IRQ_DMAC) | (1u << IRQ_TC4) | (1u << IRQ_TC5))
#define SERCOM4_DATA 0x42001828u
#define FRAME_CYCLES 803649u            /* 48 MHz / 59.7275 Hz */

#define LANES(n, a)  (((n) == 4 ? ~0u : (1u << (8 * (n))) - 1u) << (8 * ((a) & 3u)))
#define MERGE(o, v, m) (((o) & ~(m)) | ((v) & (m)))

static uint32_t rd(Samd21 *b, uint32_t a, unsigned n) { return m0_machine_read(&b->m, a, n); }
static void wr(Samd21 *b, uint32_t a, uint32_t v, unsigned n) { m0_machine_write(&b->m, a, v, n); }

/* ----------------------------------------------------------- interrupts */

static uint32_t line_bits(const Samd21 *b) {
    uint32_t line = 0;
    for (unsigned c = 0; c < SAMD21_DMA_CHANNELS; ++c)
        if (b->dma_flag[c] & b->dma_en[c] & 7u) { line |= 1u << IRQ_DMAC; break; }
    if (b->tc[0].intFlag & b->tc[0].intEn) line |= 1u << IRQ_TC4;
    if (b->tc[1].intFlag & b->tc[1].intEn) line |= 1u << IRQ_TC5;
    return line;
}
/* the NVIC inputs are levels: re-derived whenever a flag or an enable moves */
static uint32_t line_cache;
static void touch(Samd21 *b) {
    line_cache = line_bits(b);
    b->m.nvic_pending = (b->m.nvic_pending & ~LINE_MASK) | line_cache;
}

/* ----------------------------------------------------------------- LCD */

static uint16_t rgb444(uint32_t c) {
    uint32_t r = (c >> 8) & 15u, g = (c >> 4) & 15u, bl = c & 15u;
    return (uint16_t)(((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (bl << 1 | bl >> 3));
}
/* pixels are kept as BGR565 (the META panel's order) */
static void lcd_put(Samd21 *b, uint16_t p) {
    if (!b->lcd_bgr) p = (uint16_t)((p >> 11) | (p & 0x07e0u) | ((p & 0x1fu) << 11));
    if (b->lcd_x < SAMD21_W && b->lcd_y < SAMD21_H) b->pix[b->lcd_y * SAMD21_W + b->lcd_x] = p;
    if (++b->lcd_x > b->lcd_xe) {
        b->lcd_x = b->lcd_xs;
        if (++b->lcd_y > b->lcd_ye) b->lcd_y = b->lcd_ys;
    }
}
static void st7735(Samd21 *b, uint8_t v) {
    if (b->pb_out & (1u << 22)) return;                 /* CS high */
    if (!(b->pb_out & (1u << 23))) { b->lcd_cmd = v; b->lcd_arg = 0; return; }
    if (b->lcd_cmd == 0x3a && b->lcd_arg == 0) b->lcd_colmod = v & 7u;
    if (b->lcd_cmd == 0x2c && b->lcd_colmod != 5) {
        b->lcd_pix[b->lcd_arg % 3] = v;
        if (b->lcd_colmod == 3) {
            if (b->lcd_arg % 3 == 1) lcd_put(b, rgb444(((uint32_t)b->lcd_pix[0] << 4) | (b->lcd_pix[1] >> 4)));
            else if (b->lcd_arg % 3 == 2) lcd_put(b, rgb444(((uint32_t)(b->lcd_pix[1] & 15u) << 8) | b->lcd_pix[2]));
        } else if (b->lcd_arg % 3 == 2) {
            lcd_put(b, (uint16_t)(((b->lcd_pix[0] >> 3) << 11) | ((b->lcd_pix[1] >> 2) << 5) | (b->lcd_pix[2] >> 3)));
        }
        ++b->lcd_arg;
        return;
    }
    if (b->lcd_cmd == 0x36) b->lcd_bgr = (v >> 3) & 1u;
    switch (b->lcd_cmd) {
    case 0x2c:
        if (b->lcd_arg % 2 == 0) b->lcd_tmp = v;
        else lcd_put(b, (uint16_t)((b->lcd_tmp << 8) | v));
        break;
    case 0x2a:
        if (b->lcd_arg == 1) b->lcd_xs = b->lcd_x = v; else if (b->lcd_arg == 3) b->lcd_xe = v;
        break;
    case 0x2b:
        if (b->lcd_arg == 1) b->lcd_ys = b->lcd_y = v; else if (b->lcd_arg == 3) b->lcd_ye = v;
        break;
    }
    ++b->lcd_arg;
}

/* -------------------------------------------------------------- SERCOM4 */

static uint32_t sercom4_hz(const Samd21 *b) {
    return b->gclk6_pll && b->dpll_on ? (uint32_t)(b->dpll_ldr + 1u) * 1000000u : SAMD21_CPU_HZ;
}
/* A byte's time, in cycles or in thirds of a cycle.  Both are 64-bit divisions
 * that only move with the clock or the baud register: divided then, and kept. */
static uint32_t spi_byte_time(const Samd21 *b, uint32_t scale) {
    static struct { uint32_t hz, baud, scale, time; } kept[2];
    const uint32_t hz = sercom4_hz(b), baud = b->spi_baud & 255u, at = scale == 48u;
    if (hz != kept[at].hz || baud != kept[at].baud || scale != kept[at].scale) {
        kept[at].hz = hz;
        kept[at].baud = baud;
        kept[at].scale = scale;
        kept[at].time = (uint32_t)(((uint64_t)scale * (baud + 1u) * SAMD21_CPU_HZ + hz / 2u) / hz);
    }
    return kept[at].time;
}
static uint32_t spi_byte_cycles(const Samd21 *b) { return spi_byte_time(b, 16u); }
static uint32_t spi_byte_thirds(const Samd21 *b) { return spi_byte_time(b, 48u); }
static uint8_t pad_byte_24(uint8_t v) {
    return (uint8_t)(((v >> 1) & 0x3fu) | (v & 1u) << 6 | (v & 0x80u));
}
/* one byte on the bus: the panel, then the pad (PB03 or PA25 low).  The SD
 * card is not modelled yet and reads as an absent device. */
static void sercom4_put(Samd21 *b, uint8_t v, int from_dma) {
    b->ser_data = 0x80;
    st7735(b, v);
    if (!(b->pb_out & (1u << 3)) || !(b->pa_out & (1u << 25))) {
        int fast = sercom4_hz(b) / (2u * ((b->spi_baud & 255u) + 1u)) > 12000000u;
        b->ser_data = fast ? pad_byte_24(b->buttons) : b->buttons;
    }
    uint64_t done;
    if (from_dma) done = b->m.cpu.cycles;
    else done = (b->spi_last > b->m.cpu.cycles ? b->spi_last : b->m.cpu.cycles) + spi_byte_cycles(b);
    b->spi_prev = b->spi_last;
    b->spi_last = done;
    if (b->rx_n < 2) { b->rx[b->rx_n].d = b->ser_data; b->rx[b->rx_n].done = done; ++b->rx_n; }
}

/* ----------------------------------------------------------------- DMAC */

static void dma_raise(Samd21 *b, unsigned ch, uint8_t bits) { (void)ch; (void)bits; touch(b); }

static void dma_load(Samd21 *b, unsigned ch, uint32_t desc) {
    if (!desc) { b->dma_on[ch] = 0; return; }
    uint16_t ctrl = (uint16_t)rd(b, desc, 2);
    if (!(ctrl & 1u)) {                         /* end of chain: suspended */
        b->dma_on[ch] = 0;
        b->dma_flag[ch] |= 0x04;
        b->dma_ferr[ch] = 1;
        b->dma_resume[ch] = desc;
        dma_raise(b, ch, 0x04);
        return;
    }
    b->dma_ferr[ch] = 0;
    b->dma_ctrl[ch] = ctrl;
    b->dma_cnt[ch] = (uint16_t)rd(b, desc + 2, 2);
    b->dma_src[ch] = rd(b, desc + 4, 4);
    b->dma_dst[ch] = rd(b, desc + 8, 4);
    b->dma_next[ch] = rd(b, desc + 12, 4);
    b->dma_idx[ch] = 0;
    b->dma_on[ch] = 1;
}
static void dma_wrb(Samd21 *b, unsigned ch) {
    if (!b->dmac_wrb) return;
    uint32_t a = b->dmac_wrb + ch * 16u;
    wr(b, a, (uint16_t)(b->dma_ctrl[ch] & ~1u), 2);
    wr(b, a + 2, 0, 2);
    wr(b, a + 4, b->dma_src[ch], 4);
    wr(b, a + 8, b->dma_dst[ch], 4);
    wr(b, a + 12, b->dma_next[ch], 4);
}
static void dma_beat(Samd21 *b, unsigned ch) {
    if (!b->dma_on[ch]) return;
    if (b->dma_cnt[ch] == 0) { dma_wrb(b, ch); dma_load(b, ch, b->dma_next[ch]); return; }
    uint16_t ctrl = b->dma_ctrl[ch];
    uint32_t size = 1u << ((ctrl >> 8) & 3u), src = b->dma_src[ch], dst = b->dma_dst[ch];
    if (ctrl & (1u << 10)) src += ((uint32_t)b->dma_idx[ch] - b->dma_cnt[ch]) * size;
    if (ctrl & (1u << 11)) dst += ((uint32_t)b->dma_idx[ch] - b->dma_cnt[ch]) * size;
    if (dst == SERCOM4_DATA && size == 1) sercom4_put(b, (uint8_t)rd(b, src, 1), 1);
    else wr(b, dst, rd(b, src, size), size);
    if (++b->dma_idx[ch] >= b->dma_cnt[ch]) {
        b->dma_flag[ch] |= 0x02;
        dma_wrb(b, ch);
        dma_raise(b, ch, 0x02);
        if (((ctrl >> 3) & 3u) >= 2u && !b->dma_skip[ch]) {
            b->dma_on[ch] = 0;
            b->dma_flag[ch] |= 0x04;
            b->dma_resume[ch] = b->dma_next[ch];
        } else {
            b->dma_skip[ch] = 0;
            dma_load(b, ch, b->dma_next[ch]);
        }
    }
}
static void dma_chan_reset(Samd21 *b, unsigned ch) {
    b->dma_on[ch] = 0;
    if (b->spi_dma == (int)ch) b->spi_dma = -1;
    b->dma_flag[ch] = b->dma_en[ch] = b->dma_ferr[ch] = b->dma_trig[ch] = 0;
    b->dma_resume[ch] = 0;
    b->dma_skip[ch] = 0;
    touch(b);
}
static void spi_arm(Samd21 *b, unsigned ch) {
    b->spi_beat_thirds = spi_byte_thirds(b);
    if (b->spi_beat_thirds < 3) b->spi_beat_thirds = 3;
    b->spi_acc = 0;
    b->spi_dma = (int)ch;
}
/* the memory-to-memory copy a software-armed channel performs at once */
static void dma_copy_now(Samd21 *b, unsigned ch, uint32_t desc) {
    uint16_t ctrl = (uint16_t)rd(b, desc, 2), n = (uint16_t)rd(b, desc + 2, 2);
    uint32_t src = rd(b, desc + 4, 4), dst = rd(b, desc + 8, 4), nxt = rd(b, desc + 12, 4);
    uint32_t size = 1u << ((ctrl >> 8) & 3u);
    for (uint16_t i = 0; i < n; ++i) {
        uint32_t s = (ctrl & (1u << 10)) ? src + ((uint32_t)i - n) * size : src;
        uint32_t d = (ctrl & (1u << 11)) ? dst + ((uint32_t)i - n) * size : dst;
        wr(b, d, rd(b, s, size), size);
    }
    b->dma_ctrl[ch] = ctrl; b->dma_src[ch] = src; b->dma_dst[ch] = dst; b->dma_next[ch] = nxt;
    dma_wrb(b, ch);
    b->dma_flag[ch] |= 0x02;
    touch(b);
}
static void dmac_chctrla(Samd21 *b, unsigned ch, uint32_t v) {
    if (v & 1u) { dma_chan_reset(b, ch); return; }
    int tc = b->dma_trig[ch] == TRIG_TC4 || b->dma_trig[ch] == TRIG_TC5;
    if (tc) {
        if ((v & 3u) == 2u) { if (!b->dma_on[ch]) dma_load(b, ch, b->dmac_base + ch * 16u); }
        else b->dma_on[ch] = 0;
        return;
    }
    if ((v & 3u) != 2u) {
        if (b->spi_dma == (int)ch) b->spi_dma = -1;
        b->dma_on[ch] = 0;
        return;
    }
    if (b->dma_on[ch]) return;
    uint32_t desc = b->dmac_base + ch * 16u;
    if (b->dma_resume[ch]) { desc = b->dma_resume[ch]; b->dma_resume[ch] = 0; }
    if (b->dma_trig[ch] == TRIG_SERCOM4_RX) { dma_load(b, ch, desc); return; }
    if (b->dma_trig[ch] == TRIG_SERCOM4_TX || rd(b, desc + 8, 4) == SERCOM4_DATA) {
        dma_load(b, ch, desc);
        if (b->dma_on[ch]) spi_arm(b, ch);
        return;
    }
    dma_copy_now(b, ch, desc);
}
static void dmac_chctrlb(Samd21 *b, unsigned ch, uint32_t v, uint32_t m) {
    uint32_t cmd = (v >> 24) & 3u;
    if ((m >> 24) && cmd == 2u) {
        if (b->dma_on[ch]) b->dma_skip[ch] = 1;
        else if (b->dma_resume[ch]) {
            uint32_t at = b->dma_resume[ch];
            b->dma_resume[ch] = 0;
            dma_load(b, ch, at);
            if (b->dma_on[ch] && (b->dma_dst[ch] == SERCOM4_DATA || b->dma_trig[ch] == TRIG_SERCOM4_TX)) spi_arm(b, ch);
        }
    }
    if (m & 0xff00u) b->dma_trig[ch] = (uint8_t)((v >> 8) & 0x3fu);
}

/* ---------------------------------------------------------------- timers */

static uint32_t tc_period(const struct Samd21Tc *t) {
    static const uint16_t presc[8] = {1, 2, 4, 8, 16, 64, 256, 1024};
    uint32_t top = ((t->ctrlA >> 2) & 3u) != 2u ? (uint32_t)t->top : t->top;
    uint32_t per = (uint32_t)presc[(t->ctrlA >> 8) & 7u] * (top + 1u);
    return per < 2u ? 2u : per;
}
static int tc_live(const struct Samd21Tc *t) { return t->enabled && t->top > 0; }


/* The display's stream in bulk: a block of RGB565 pixels from SRAM to the
 * panel's window (RAMWR, 16 bpp, panel selected, data mode, no pad read in
 * the same bytes).  Same effect as that many dma_beat calls, minus the last
 * beat of the block, which dma_beat finishes (flags, write-back, next
 * descriptor).  Returns the beats done. */
static uint32_t spi_fast_pixels(Samd21 *b, unsigned ch, uint32_t due) {
    uint16_t ctrl = b->dma_ctrl[ch];
    if (((ctrl >> 8) & 3u) != 0 || !(ctrl & (1u << 10)) || (ctrl & (1u << 11)) || b->dma_dst[ch] != SERCOM4_DATA) return 0;
    if ((b->pb_out & ((1u << 22) | (1u << 23) | (1u << 3))) != ((1u << 23) | (1u << 3)) || !(b->pa_out & (1u << 25))) return 0;
    if (b->lcd_cmd != 0x2c || b->lcd_colmod != 5 || b->dma_cnt[ch] <= b->dma_idx[ch] + 1u) return 0;
    uint32_t left = b->dma_cnt[ch] - b->dma_idx[ch] - 1u, n = due < left ? due : left;
    uint32_t src = b->dma_src[ch] - (uint32_t)(b->dma_cnt[ch] - b->dma_idx[ch]), o = src - b->m.ram_base;
    if (!n || o > M0_MACHINE_RAM_BYTES || n > M0_MACHINE_RAM_BYTES - o) return 0;
    const uint8_t *p = b->m.ram + o;
    for (uint32_t i = 0; i < n; ++i) {
        if (b->lcd_arg & 1) lcd_put(b, (uint16_t)((b->lcd_tmp << 8) | p[i]));
        else b->lcd_tmp = p[i];
        ++b->lcd_arg;
    }
    b->dma_idx[ch] += (uint16_t)n;
    b->ser_data = 0x80;
    b->spi_prev = b->spi_last = b->m.cpu.cycles;
    for (uint32_t i = 0; i < n && b->rx_n < 2; ++i) {
        b->rx[b->rx_n].d = 0x80; b->rx[b->rx_n].done = b->m.cpu.cycles; ++b->rx_n;
    }
    return n;
}

static void sync(Samd21 *b) {
    static int busy;
    if (busy) return;
    busy = 1;
    for (int i = 0; i < 2; ++i) {
        struct Samd21Tc *t = &b->tc[i];
        if (!tc_live(t)) continue;
        uint32_t per = tc_period(t);
        while (t->next <= b->m.cpu.cycles) {
            t->next += per;
            if (t->intEn & 0x33u) {
                t->intFlag |= t->intEn & 0x11u;
                touch(b);
            } else {
                for (unsigned c = 0; c < SAMD21_DMA_CHANNELS; ++c)
                    if (b->dma_on[c] && b->dma_trig[c] == (i ? TRIG_TC5 : TRIG_TC4)) dma_beat(b, c);
            }
        }
    }
    if (b->spi_dma >= 0) {
        uint64_t n = b->m.cpu.cycles - b->spi_from;
        b->spi_acc += (uint32_t)(3u * n);
        while (b->spi_dma >= 0 && b->spi_acc >= b->spi_beat_thirds) {
            unsigned c = (unsigned)b->spi_dma;
            if (!b->dma_on[c]) { b->spi_dma = -1; break; }
            uint32_t done = spi_fast_pixels(b, c, b->spi_acc / b->spi_beat_thirds);
            if (done) { b->spi_acc -= done * b->spi_beat_thirds; continue; }
            b->spi_acc -= b->spi_beat_thirds;
            dma_beat(b, c);
            if (!b->dma_on[c]) b->spi_dma = -1;
        }
    }
    b->spi_from = b->m.cpu.cycles;
    uint64_t next = ~(uint64_t)0;
    for (int i = 0; i < 2; ++i)
        if (tc_live(&b->tc[i]) && b->tc[i].next < next) next = b->tc[i].next;
    if (b->spi_dma >= 0) {
        unsigned c = (unsigned)b->spi_dma;
        uint32_t left = b->dma_cnt[c] > b->dma_idx[c] ? (uint32_t)(b->dma_cnt[c] - b->dma_idx[c]) : 1u;
        if (left > 256u) left = 256u;
        uint64_t need = (uint64_t)left * b->spi_beat_thirds;
        need = need > b->spi_acc ? need - b->spi_acc : 1u;
        uint64_t at = b->m.cpu.cycles + (need + 2u) / 3u;
        if (at < next) next = at;
    }
    b->next_event = next;
    busy = 0;
}

/* ------------------------------------------------------------- registers */

static uint32_t adc_random(Samd21 *b) {
    b->adc_prng ^= b->adc_prng << 13; b->adc_prng ^= b->adc_prng >> 17; b->adc_prng ^= b->adc_prng << 5;
    return b->adc_prng & 0xffffu;
}
static uint32_t port_read(Samd21 *b, unsigned group, uint32_t reg) {
    uint32_t out = group ? b->pb_out : b->pa_out, dir = group ? b->pb_dir : b->pa_dir;
    switch (reg) {
    case 0x00: case 0x04: case 0x08: case 0x0c: return dir;
    case 0x10: case 0x14: case 0x18: case 0x1c: return out;
    case 0x20: return (out & dir) | ~dir;
    }
    return 0;
}
static void port_write(Samd21 *b, unsigned group, uint32_t reg, uint32_t v, uint32_t m) {
    uint32_t *out = group ? &b->pb_out : &b->pa_out, *dir = group ? &b->pb_dir : &b->pa_dir;
    switch (reg) {
    case 0x00: *dir = MERGE(*dir, v, m); break;
    case 0x04: *dir &= ~v; break;
    case 0x08: *dir |= v; break;
    case 0x0c: *dir ^= v; break;
    case 0x10: *out = MERGE(*out, v, m); break;
    case 0x14: *out &= ~v; break;
    case 0x18: *out |= v; break;
    case 0x1c: *out ^= v; break;
    }
}

static uint32_t dmac_chan_read(Samd21 *b, unsigned ch, uint32_t off) {
    if (ch >= SAMD21_DMA_CHANNELS) return 0;
    switch (off) {
    case 0x0: return b->dma_on[ch] ? 2u : 0u;
    case 0x4: return (uint32_t)b->dma_trig[ch] << 8;
    case 0xc: return b->dma_en[ch] * 0x0101u | (uint32_t)b->dma_flag[ch] << 16 |
                     (uint32_t)((b->dma_ferr[ch] ? 4u : 0u) | (b->dma_on[ch] ? 2u : 0u)) << 24;
    }
    return 0;
}
static uint32_t dmac_read(Samd21 *b, uint32_t r) {
    switch (r) {
    case 0x20:
        for (unsigned n = 0; n < SAMD21_DMA_CHANNELS; ++n)
            if (b->dma_flag[n]) return (n & 15u) | ((uint32_t)(b->dma_flag[n] & 7u) << 8);
        return 0;
    case 0x24: {
        uint32_t v = 0;
        for (unsigned n = 0; n < SAMD21_DMA_CHANNELS; ++n)
            if (b->dma_flag[n] & b->dma_en[n] & 7u) v |= 1u << n;
        return v;
    }
    case 0x34: return b->dmac_base;
    case 0x38: return b->dmac_wrb;
    case 0x3c: return b->dmac_chid << 24;
    }
    if (r >= 0x40 && r < 0x100) return dmac_chan_read(b, r < 0x50 ? b->dmac_chid : (r - 0x40) >> 4, r & 0xcu);
    return 0;
}
static void dmac_write(Samd21 *b, uint32_t r, uint32_t v, uint32_t m) {
    switch (r) {
    case 0x34: b->dmac_base = MERGE(b->dmac_base, v, m); return;
    case 0x38: b->dmac_wrb = MERGE(b->dmac_wrb, v, m); return;
    case 0x3c: if (m >> 24) b->dmac_chid = (v >> 24) & 15u; return;
    }
    if (r < 0x40 || r >= 0x100) return;
    unsigned ch = r < 0x50 ? b->dmac_chid : (r - 0x40) >> 4;
    if (ch >= SAMD21_DMA_CHANNELS) return;
    switch (r & 0xcu) {
    case 0x0: if (m & 0xffu) dmac_chctrla(b, ch, v & 0xffu); return;
    case 0x4:
        if (r < 0x50) dmac_chctrlb(b, ch, v, m);
        else if (m & 0xff00u) b->dma_trig[ch] = (uint8_t)((v >> 8) & 0x3fu);
        return;
    case 0xc:
        if (m & 0xffu) b->dma_en[ch] &= (uint8_t)~v;
        if (m & 0xff00u) b->dma_en[ch] |= (uint8_t)(v >> 8);
        if (m & 0xff0000u) b->dma_flag[ch] &= (uint8_t)~(v >> 16);
        touch(b);
        return;
    }
}

static uint32_t periph_read(Samd21 *b, uint32_t a, uint32_t m) {
    uint32_t r = a & 0x3fcu, base = a & ~0x3ffu;
    switch (base) {
    case 0x40000800u:                                       /* SYSCTRL */
        if (r == 0x0c) return 0xd2u | (b->dpll_on ? (3u << 9) : 0u);
        if (r == 0x50) return b->dpll_on ? 3u : 0u;
        return 0;
    case 0x41004000u:                                       /* NVMCTRL */
        switch (r) {
        case 0x08: return 3u << 16 | 0x40000u / 64u;
        case 0x14: return 1;
        }
        return 0;
    case 0x41004400u:                                       /* PORT */
        return r < 0x100 && (r & 0x7fu) < 0x24u ? port_read(b, r >> 7, r & 0x7cu) : 0;
    case 0x41004800u: return dmac_read(b, r);
    case 0x42001800u:                                       /* SERCOM4 */
        if (r == 0x18) {
            uint32_t v = 0;
            if ((int64_t)(b->spi_prev - b->m.cpu.cycles) <= 0) v |= 1u;
            if ((int64_t)(b->spi_last - b->m.cpu.cycles) <= 0) v |= 2u;
            if (b->rx_n && b->rx[0].done <= b->m.cpu.cycles) v |= 4u;
            return v;
        }
        if (r == 0x28) {
            if (!(m & 0xffu)) return 0;
            if (b->rx_n) {
                uint8_t d = b->rx[0].d;
                b->rx[0] = b->rx[1];
                --b->rx_n;
                return d;
            }
            return b->ser_data;
        }
        return 0;
    case 0x42001c00u: return r == 0x18 ? 7u : r == 0x28 ? 0x80u : 0u;
    case 0x42003000u: case 0x42003400u: {
        const struct Samd21Tc *t = &b->tc[base == 0x42003400u];
        return r == 0x0c ? t->intEn * 0x0101u | (uint32_t)t->intFlag << 16 : 0;
    }
    case 0x42004000u:                                       /* ADC */
        return r == 0x18 ? 1u | ((m >> 16) ? adc_random(b) << 16 : 0u) : 0u;
    }
    return 0;
}

static void periph_write(Samd21 *b, uint32_t a, uint32_t v, uint32_t m) {
    uint32_t r = a & 0x3fcu, base = a & ~0x3ffu;
    switch (base) {
    case 0x40000800u:
        if (r == 0x44 && (m & 2u)) b->dpll_on = (uint8_t)((v >> 1) & 1u);
        else if (r == 0x48 && (m & 0xfffu)) b->dpll_ldr = (uint16_t)(v & 0xfffu);
        return;
    case 0x40000c00u:
        if (r == 0x04) {
            uint32_t id = (v >> 8) & 15u, w = MERGE(b->gclk_gen[id], v, m);
            b->gclk_gen[id] = w;
            if (id == 6u) b->gclk6_pll = (w & (1u << 6)) != 0u && ((w >> 16) & 0x1fu) == 8u;
        }
        return;
    case 0x41004400u:
        if (r < 0x100 && (r & 0x7fu) < 0x24u) port_write(b, r >> 7, r & 0x7cu, v, m);
        return;
    case 0x41004800u: dmac_write(b, r, v, m); return;
    case 0x42001800u:
        if (r == 0x00 && ((m & v & 1u) || ((m & 2u) && !(v & 2u)))) b->rx_n = 0;
        else if (r == 0x04 && (m & (1u << 17)) && !(v & (1u << 17))) b->rx_n = 0;
        else if (r == 0x0c && (m & 0xffu)) b->spi_baud = v & 0xffu;
        else if (r == 0x28 && (m & 0xffu)) sercom4_put(b, (uint8_t)v, 0);
        return;
    case 0x42003000u: case 0x42003400u: {
        struct Samd21Tc *t = &b->tc[base == 0x42003400u];
        switch (r) {
        case 0x00:
            if (!(m & 0xffffu)) return;
            t->ctrlA = MERGE(t->ctrlA, v, m & 0xffffu);
            if (((t->ctrlA & 2u) != 0) != (t->enabled != 0)) {
                t->enabled = (t->ctrlA & 2u) != 0;
                if (t->enabled) t->next = b->m.cpu.cycles + tc_period(t);
            } else if (t->enabled) {
                /* prescaler change: keep the phase of the last overflow */
                t->next = b->m.cpu.cycles + tc_period(t);
            }
            break;
        case 0x0c:
            if (m & 0xffu) t->intEn &= (uint8_t)~v;
            if (m & 0xff00u) t->intEn |= (uint8_t)(v >> 8);
            if (m & 0xff0000u) t->intFlag &= (uint8_t)~(v >> 16);
            touch(b);
            break;
        case 0x18: {
            uint32_t old = tc_period(t);
            t->top = (uint16_t)MERGE(t->top, v, m);
            if (t->enabled) t->next = t->next - old + tc_period(t);
            break;
        }
        }
        return;
    }
    case 0x42004800u:                                       /* DAC */
        if (r == 0x08 && (m & 0xffffu)) {
            b->dac = (uint16_t)MERGE(b->dac, v, m & 0xffffu);
            int s = ((int)(b->dac & 0x3ffu) - 512) * 64;
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            if (b->lev_n < sizeof b->lev / sizeof b->lev[0]) {
                b->lev[b->lev_n].t = (uint32_t)(b->m.cpu.cycles - b->audio_base);
                b->lev[b->lev_n++].v = (int16_t)s;
            }
            b->lev_cur = (int16_t)s;
        }
        return;
    }
}


/* ------------------------------------------------------------- profile */

static int reg_known(uint32_t a, int write) {
    uint32_t base = a & ~0x3ffu, r = a & 0x3fcu;
    switch (base) {
    case 0x40000800u: return write ? (r == 0x44 || r == 0x48) : (r == 0x0c || r == 0x50);
    case 0x40000c00u: return write ? r == 0x04 : r == 0x00;    /* GENCTRL; STATUS reads 0: not busy */
    case 0x41004000u: return write ? 0 : (r == 0x08 || r == 0x14);
    case 0x41004400u: return r < 0x100 && (r & 0x7fu) < 0x24u;
    case 0x41004800u: return 1;
    case 0x42001800u: return r == 0x00 || r == 0x04 || r == 0x0c || r == 0x18 || r == 0x28 || (!write && r == 0x1c);
    case 0x42001c00u: return !write && (r == 0x18 || r == 0x28);
    case 0x42003000u: case 0x42003400u: return r == 0x00 || r == 0x0c || r == 0x18;
    case 0x42004000u: return !write && r == 0x18;
    case 0x42004800u: return write ? r == 0x08 : r == 0x04;     /* DATA; STATUS reads 0: not busy */
    }
    return a >= 0x60000000u && a < 0x60000100u;
}
static void mmio_note(Samd21 *b, uint32_t a, int w, unsigned size, uint32_t v) {
    struct Samd21Mmio *m = b->mmio;
    uint32_t h = (a * 2654435761u ^ (uint32_t)w * 40503u ^ size) & 1023u;
    for (int i = 0; i < 1024; ++i, h = (h + 1) & 1023u) {
        typeof(m->e[0]) *e = &m->e[h];
        if (!e->n) { e->a = a; e->w = (uint8_t)w; e->size = (uint8_t)size; e->known = (uint8_t)reg_known(a, w); }
        if (e->a != a || e->w != w || e->size != size) continue;
        ++e->n;
        if (w) {
            uint32_t k = 0;
            while (k < e->nv && e->v[k] != v) ++k;
            if (k == e->nv && e->nv < 4) e->v[e->nv++] = v;
        }
        if (e->pc[0] != m->pc && e->pc[1] != m->pc) e->pc[e->pc[0] ? 1 : 0] = m->pc;
        return;
    }
}
int samd21_profile_start(Samd21 *b) {
    static struct Samd21Mmio table;
    memset(&table, 0, sizeof table);
    b->mmio = &table;
    return 0;
}
void samd21_profile_mmio(const Samd21 *b, FILE *out) {
    if (!b->mmio) return;
    for (int i = 0; i < 1024; ++i) {
        const typeof(b->mmio->e[0]) *e = &b->mmio->e[i];
        if (!e->n) continue;
        /* an unmodelled write is a configuration the board ignores; an
         * unmodelled read returns 0, and its value may matter to the guest */
        fprintf(out, "%08x %c%u n=%u%s", e->a, e->w ? 'w' : 'r', e->size, e->n,
                e->known ? "" : e->w ? " ignored" : " UNMODELLED-READ");
        for (uint32_t k = 0; k < e->nv; ++k) fprintf(out, " v=%x", e->v[k]);
        fprintf(out, " pc=%x,%x\n", e->pc[0], e->pc[1]);
    }
}

/* A poll loop that only an event can end: after two identical iterations
 * the wait up to the next deadline (TC/DMA event, SysTick, frame end) is
 * spent at once.  The polled word must change only at events: RAM, or a
 * TC/DMAC flag; a SERCOM flag follows the clock and is left alone. */
void samd21_idle(Samd21 *b, uint32_t loop_pc, uint32_t addr) {
    M0Machine *m = &b->m;
    int polled = addr - m->ram_base < M0_MACHINE_RAM_BYTES ||
                 (addr >= 0x42003000u && addr < 0x42003800u) || (addr >= 0x41004800u && addr < 0x41004900u);
    uint64_t now = b->m.cpu.cycles;
    if (!polled) { b->idle_run = 0; return; }
    if (loop_pc != b->idle_pc || b->idle_at == 0) {
        b->idle_pc = loop_pc; b->idle_at = now; b->idle_period = 0; b->idle_run = 0;
        return;
    }
    uint64_t period = now - b->idle_at;
    b->idle_at = now;
    if (period == 0 || period != b->idle_period) { b->idle_period = period; b->idle_run = 0; return; }
    if (++b->idle_run < 2) return;
    if (m->cpu.primask ? 0 : (m->nvic_pending & m->nvic_enable) || m->systick_pending || m->pendsv_pending) return;
    uint64_t limit = b->next_event < b->frame_end ? b->next_event : b->frame_end;
    if (m->systick_ctrl & 1u) {
        uint64_t st = m->systick_val ? m->systick_val : (uint64_t)m->systick_load + 1u;
        if (now + st < limit) limit = now + st;
    }
    if (limit <= now + 2 * period) return;
    uint64_t n = (limit - now) / period - 1u;
    if (!n) return;
    m->cpu.cycles += (uint32_t)(n * period);
    b->idle_skipped += n * period;
    b->idle_at += n * period;
}

/* ------------------------------------------------------------------ bus */

static uint32_t board_read(void *ctx, uint32_t a, unsigned width) {
    Samd21 *b = ctx;
    if (a >= 0x40000000u) ++b->m.cpu.stores;                /* a peripheral read is not a repeat */
    if (a < 0x20000000u || (a - 0x00400000u) < 0x40000u) {   /* bootloader area, flash alias */
        uint32_t o = a >= 0x00400000u ? a - 0x00400000u : a, v = 0;
        if (o >= b->m.flash_base && o - b->m.flash_base + width <= b->m.flash_size)
            for (unsigned i = 0; i < width; ++i) v |= (uint32_t)b->m.flash[o - b->m.flash_base + i] << (8 * i);
        else v = width == 4 ? ~0u : (1u << (8 * width)) - 1u;
        return v;
    }
    if (a >= 0x60000000u && a < 0x60000100u) {              /* IOBUS PORT */
        if (b->mmio) mmio_note(b, a, 0, width, 0);
        uint32_t v = port_read(b, (a >> 7) & 1u, a & 0x7cu);
        return (v >> (8 * (a & 3u))) & (width == 4 ? ~0u : (1u << (8 * width)) - 1u);
    }
    if (b->mmio) mmio_note(b, a, 0, width, 0);
    if ((a & 0xfcff0000u) != 0x40000000u) return 0;
    sync(b);
    uint32_t v = periph_read(b, a, LANES(width, a));
    return (v >> (8 * (a & 3u))) & (width == 4 ? ~0u : (1u << (8 * width)) - 1u);
}
static void board_write(void *ctx, uint32_t a, uint32_t v, unsigned width) {
    Samd21 *b = ctx;
    if (b->mmio && a >= 0x40000000u) mmio_note(b, a, 1, width, v);
    if (a >= 0x60000000u && a < 0x60000100u) {
        port_write(b, (a >> 7) & 1u, a & 0x7cu, v << (8 * (a & 3u)), LANES(width, a));
        return;
    }
    if ((a & 0xfcff0000u) != 0x40000000u) return;           /* flash is read-only here */
    sync(b);
    periph_write(b, a, v << (8 * (a & 3u)), LANES(width, a));
    sync(b);
}
static void board_tick(void *ctx, uint32_t delta) {
    Samd21 *b = ctx;
    (void)delta;
    if (b->m.cpu.cycles >= b->next_event) sync(b);
    b->m.nvic_pending |= line_cache;
}

/* ------------------------------------------------------------------ API */

int samd21_init(Samd21 *b, const uint8_t *flash, size_t flash_size, uint32_t flash_base,
                uint16_t *pixels) {
    if (!flash || flash_size < 8 || !pixels) return -1;
    memset(b, 0, sizeof *b);
    m0_machine_init(&b->m, flash, flash_size, flash_base, 0x20000000u);
    b->m.peripheral = b;
    b->m.peripheral_read = board_read;
    b->m.peripheral_write = board_write;
    b->m.peripheral_tick = board_tick;
    b->pix = pixels;
    memset(pixels, 0, (size_t)SAMD21_W * SAMD21_H * 2u);
    b->pa_out = (1u << 27) | (1u << 25);
    b->buttons = 0xff;
    b->dpll_ldr = 95;
    b->spi_dma = -1;
    b->spi_beat_thirds = 20;
    b->adc_prng = 0x2545F491u;
    b->ser_data = 0x80;
    b->next_event = ~(uint64_t)0;
    line_cache = 0;
    memset(b->m.ram, 0xff, sizeof b->m.ram);
    b->frame_end = FRAME_CYCLES;
    return 0;
}
void samd21_buttons(Samd21 *b, uint8_t pressed) { b->buttons = (uint8_t)~pressed; }
void samd21_sd(Samd21 *b, Samd21SdRead r, Samd21SdWrite w) { b->sd_read = r; b->sd_write = w; }

void samd21_frame(Samd21 *b, int (*native_step)(M0Cpu *)) {
    M0Cpu *cpu = &b->m.cpu;
    {   /* the cursor stays where the last frame left it, in this frame's time */
        uint64_t span = (b->m.cpu.cycles - b->audio_base) << 8;
        b->audio_t = b->audio_t > span ? b->audio_t - span : 0;
        b->audio_cur = b->lev_cur;                                /* the level held at the frame's end */
    }
    b->lev_n = 0;
    b->audio_k = 0;
    b->audio_base = b->m.cpu.cycles;
    uint64_t end = b->frame_end;
    while (b->m.cpu.cycles < end && !cpu->fault) {
        if (b->step_hook) {
            uint32_t pc = cpu->r[15], c0 = cpu->cycles;
            if (b->mmio) b->mmio->pc = pc;
            m0_cpu_step(cpu);
            b->step_hook(b->hook_ctx, pc, cpu->cycles - c0);
        } else if (!(native_step && native_step(cpu))) m0_cpu_step(cpu);
    }
    sync(b);
    b->frac += 11u;                                         /* 803649.1.. cycles per frame */
    b->frame_end += FRAME_CYCLES + (b->frac >= 100u ? (b->frac -= 100u, 1u) : 0u);
}

void samd21_audio_rate(Samd21 *b, unsigned hz) {
    b->audio_step = hz ? (uint32_t)(((uint64_t)SAMD21_CPU_HZ << 8) / hz) : 0;   /* cycles per sample, 24.8 */
}
/* Samples of the frame just run, in as many calls as the caller wants: the
 * cursor walks the frame's DAC writes (zero-order hold) and carries over. */
void samd21_audio(Samd21 *b, uint8_t *out, unsigned n) {
    if (!b->audio_step) samd21_audio_rate(b, 11025);
    unsigned k = b->audio_k;
    int16_t cur = b->audio_cur;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t t = (uint32_t)(b->audio_t >> 8);
        while (k < b->lev_n && b->lev[k].t <= t) cur = b->lev[k++].v;
        b->audio_t += b->audio_step;
        b->audio_hp += ((int)cur - b->audio_hp) >> 10;           /* AC coupling */
        out[i] = (uint8_t)(((int)cur - b->audio_hp) / 256 + 128);
    }
    b->audio_k = k; b->audio_cur = cur;
}

uint32_t samd21_room(const Samd21 *b) {
    const M0Machine *m = &b->m;
    if (m->cpu.fault || m0_machine_pending_exception(m)) return 0;
    uint64_t now = m->cpu.cycles;
    uint64_t limit = b->next_event < b->frame_end ? b->next_event : b->frame_end;
    if (m->systick_ctrl & 1u) {
        uint64_t st = m->accounted_cycles + (m->systick_val ? m->systick_val : (uint64_t)m->systick_load + 1u);
        if (st < limit) limit = st;
    }
    if (limit <= now) return 0;
    return limit - now > 0x7fffffffu ? 0x7fffffffu : (uint32_t)(limit - now);
}

/* Registers whose read changes nothing (SERCOM4 INTFLAG, TC4/TC5 INTFLAG,
 * STATUS, COUNT, DAC STATUS): the board only has to know the time. */
static int pure_status(uint32_t a) {
    const uint32_t lo = a & 0x3ffu;
    return ((a & 0xfffffc00u) == 0x42001800u && lo == 0x18u) ||
           ((a & 0xfffff800u) == 0x42003000u && (lo == 0x0eu || lo == 0x0fu || lo == 0x10u)) ||
           ((a & 0xfffffc00u) == 0x42004800u && lo == 0x07u);
}
/* Writes the board only has to see in time: a DAC sample (it is logged with the
 * cycle), the clearing of a TC4/TC5 INTFLAG. */
static int pure_write(uint32_t a) {
    const uint32_t lo = a & 0x3ffu;
    return ((a & 0xfffff800u) == 0x42003000u && lo == 0x0eu) ||
           ((a & 0xfffffc00u) == 0x42004800u && (lo == 0x08u || lo == 0x09u));
}
uint32_t m0_native_ld_slow(M0Cpu *m, uint32_t a, unsigned n, uint32_t *c, int *hw) {
    const M0Machine *b = (const M0Machine *)m->board;
    uint32_t o = a - b->flash_base, v = 0;
    /* code and constants: reading the flash has no side effect and no wait state */
    if (o <= b->flash_size - n) { memcpy(&v, b->flash + o, n); return v; }
    ++m->stores;
    if (pure_status(a)) {
        m->cycles += *c;                /* the board sees the time of the access, the chain goes on */
        v = m->read(m->board, a, n);
        m->cycles -= *c;
        return v;
    }
    m->cycles += *c; *c = 0; *hw = 1;
    return m->read(m->board, a, n);
}
void m0_native_st_slow(M0Cpu *m, uint32_t a, uint32_t v, unsigned n, uint32_t *c, int *hw) {
    if (pure_write(a)) {
        m->cycles += *c;
        m->write(m->board, a, v, n);
        m->cycles -= *c;
        return;
    }
    m->cycles += *c; *c = 0; *hw = 1;
    m->write(m->board, a, v, n);
}

void m0_native_idle(M0Cpu *m, uint32_t pc, uint32_t addr) { samd21_idle((Samd21 *)m->board, pc, addr); }

/* A short loop with no store, visited again with the same registers, flags
 * and machine state: nothing changes until an event, so the iterations up to
 * the next deadline are skipped (cycles only).  `pending` is the chain's
 * cycles not yet on the clock.  Returns the cycles skipped. */
uint32_t m0_native_loop(M0Cpu *m, uint32_t pc, uint32_t pending) {
    Samd21 *b = (Samd21 *)m->board;
    struct Samd21Loop *l = &b->loop;
    uint64_t now = m->cycles + pending;
    const int same = l->pc == pc && l->stores == m->stores && l->flags == (uint32_t)(m->n | m->z << 1 | m->c << 2 | m->v << 3) &&
                     !memcmp(l->r, m->r, sizeof l->r);
    uint64_t period = now - l->at;
    if (!same) {
        l->pc = pc; l->stores = m->stores; l->flags = (uint32_t)(m->n | m->z << 1 | m->c << 2 | m->v << 3);
        memcpy(l->r, m->r, sizeof l->r);
        l->at = now; l->period = 0; l->run = 0;
        return 0;
    }
    l->at = now;
    if (period != l->period) { l->period = period; l->run = 0; return 0; }
    if (++l->run < 2 || !period) return 0;
    if (m0_machine_pending_exception(&b->m)) return 0;
    uint64_t limit = b->next_event < b->frame_end ? b->next_event : b->frame_end;
    if (b->m.systick_ctrl & 1u) {
        uint64_t st = b->m.accounted_cycles + (b->m.systick_val ? b->m.systick_val : (uint64_t)b->m.systick_load + 1u);
        if (st < limit) limit = st;
    }
    if (limit <= now + 2 * period) return 0;
    uint64_t n = (limit - now) / period - 1u;
    if (!n) return 0;
    b->idle_skipped += n * period;
    l->at += n * period;
    return (uint32_t)(n * period);
}
