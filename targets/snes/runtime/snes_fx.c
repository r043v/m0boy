/* The SuperFX (GSU): the raster coprocessor's interpreter.  The core
 * follows the published GSU semantics (the bsnes core was the reading
 * reference; this is our own C): sixteen 16-bit registers (R15 the
 * program counter, R14 the ROM-buffer address), the SFR flags, the
 * two-byte pipeline, the 512-byte instruction cache keyed on CBR, the
 * ROM and RAM buffer registers with their access latencies, the plotting
 * pixel cache and the screen layout of SCBR/SCMR.
 *
 * Clock model: the GSU runs at master/1 against the CPU at master/6, so
 * every CPU cycle credits six GSU ticks (the CLSR flag changes an
 * instruction's tick count, not the wall clock).  The ticks are credited
 * at the same line-slice boundaries the APU uses and on every register
 * access the S-CPU makes; the frame stays deterministic, which is what
 * the native == interpreter contract hashes. */

#include "snes.h"

#include <string.h>

#define FX_Z    0x0002
#define FX_CY   0x0004
#define FX_S    0x0008
#define FX_OV   0x0010
#define FX_G    0x0020
#define FX_R    0x0040
#define FX_ALT1 0x0100
#define FX_ALT2 0x0200
#define FX_B    0x1000
#define FX_IRQ  0x8000

/* r_mod: bit n set when the instruction wrote r[n]; bit 15 doubles as
 * "R15 was written, the fetch must not advance it" */
#define FX_MOD(n) (1u << (n))

static snes_fx_t *const fx = &machine.fx;

static uint8_t fx_read(uint32_t addr);
static void fx_write(uint32_t addr, uint8_t v);

/* ---- the clock: buffer-register latencies, then the budget ---- */

static void fx_tick(uint32_t clocks) {
    if (fx->romcl) {
        fx->romcl -= clocks < fx->romcl ? clocks : fx->romcl;
        if (fx->romcl == 0) {
            fx->sfr &= (uint16_t)~FX_R;
            fx->romdr = fx_read(((uint32_t)fx->rombr << 16) + fx->r[14]);
        }
    }
    if (fx->ramcl) {
        fx->ramcl -= clocks < fx->ramcl ? clocks : fx->ramcl;
        if (fx->ramcl == 0)
            fx_write(0x700000u + ((uint32_t)fx->rambr << 16) + fx->ramar, fx->ramdr);
    }
    fx->bank -= (int32_t)clocks;
}

/* ---- GSU-space memory: ROM through the LoROM formula, RAM below ---- */

static uint8_t fx_read(uint32_t addr) {
    if ((addr & 0xc00000) == 0x000000u) { /* banks $00-$3F */
        const uint32_t fo = ((addr & 0x3f0000u) >> 1) | (addr & 0x7fffu);
        return machine.cpu.rom[fo % machine.cpu.rom_bytes];
    }
    if ((addr & 0xe00000) == 0x400000u) { /* banks $40-$5F: the ROM, linear */
        return machine.cpu.rom[addr % machine.cpu.rom_bytes];
    }
    if ((addr & 0xe00000) == 0x600000u) { /* banks $60-$7F: the GSU's RAM */
        return machine.fx_ram[addr & 0x7FFFu]; /* the GSU's RAM is 32 KiB */
    }
    return 0x00; /* open bus */
}

static void fx_write(uint32_t addr, uint8_t v) {
    if ((addr & 0xe00000) == 0x600000u) {
        machine.fx_ram[addr & 0x7FFFu] = v;
    }
}

/* ---- the ROM and RAM buffer registers ---- */

static void fx_rombuf_update(void) {
    fx->sfr |= FX_R;
    fx->romcl = fx->clsr ? 5 : 6;
}

static uint8_t fx_rombuf_read(void) {
    if (fx->romcl) fx_tick(fx->romcl);
    return fx->romdr;
}

static uint8_t fx_rambuf_read(uint16_t addr) {
    if (fx->ramcl) fx_tick(fx->ramcl);
    return fx_read(0x700000u + ((uint32_t)fx->rambr << 16) + addr);
}

static void fx_rambuf_write(uint16_t addr, uint8_t v) {
    if (fx->ramcl) fx_tick(fx->ramcl);
    fx->ramcl = fx->clsr ? 5 : 6;
    fx->ramar = addr;
    fx->ramdr = v;
}

/* ---- the 512-byte instruction cache ---- */

static void fx_flush_cache(void) {
    memset(fx->cache_valid, 0, sizeof fx->cache_valid);
}

static uint8_t fx_opcode(uint16_t addr) {
    const uint16_t offset = addr - fx->cbr;
    if (offset < 512) {
        if (!fx->cache_valid[offset >> 4]) {
            unsigned dp = offset & 0xfff0;
            uint32_t sp = ((uint32_t)fx->pbr << 16) + ((fx->cbr + dp) & 0xfff0u);
            for (unsigned n = 0; n < 16; n++) {
                fx_tick(fx->clsr ? 5 : 6);
                fx->cache[dp++] = fx_read(sp++);
            }
            fx->cache_valid[offset >> 4] = 1;
        } else {
            fx_tick(fx->clsr ? 1 : 2);
        }
        return fx->cache[offset];
    }
    if (fx->pbr <= 0x5f) {
        if (fx->romcl) fx_tick(fx->romcl);
    } else {
        if (fx->ramcl) fx_tick(fx->ramcl);
    }
    fx_tick(fx->clsr ? 5 : 6);
    return fx_read(((uint32_t)fx->pbr << 16) | addr);
}

static uint8_t fx_peekpipe(void) {
    const uint8_t result = fx->pipeline;
    fx->pipeline = fx_opcode(fx->r[15]);
    fx->r_mod &= (uint16_t)~FX_MOD(15);
    return result;
}

static uint8_t fx_pipe(void) {
    const uint8_t result = fx->pipeline;
    fx->pipeline = fx_opcode(++fx->r[15]);
    fx->r_mod &= (uint16_t)~FX_MOD(15);
    return result;
}

/* ---- plotting: the pixel cache, the screen layout ---- */

static void fx_color_set(uint8_t src) {
    if (fx->por & 0x04) fx->colr = (uint8_t)((fx->colr & 0xf0) | (src >> 4));
    else if (fx->por & 0x08) fx->colr = (uint8_t)((fx->colr & 0xf0) | (src & 0x0f));
    else fx->colr = src;
}

static unsigned fx_tile_column(unsigned x, unsigned y) {
    const unsigned ht = fx->por & 0x10 ? 3
                        : ((fx->scmr & 0x20 ? 2u : 0u) | (fx->scmr & 0x04 ? 1u : 0u));
    switch (ht) {
    case 0: return ((x & 0xf8) << 1) + ((y & 0xf8) >> 3);
    case 1: return ((x & 0xf8) << 1) + ((x & 0xf8) >> 1) + ((y & 0xf8) >> 3);
    case 2: return ((x & 0xf8) << 1) + (x & 0xf8) + ((y & 0xf8) >> 3);
    default: return ((y & 0x80) << 2) + ((x & 0x80) << 1) + ((y & 0x78) << 1) + ((x & 0x78) >> 3);
    }
}

static unsigned fx_bpp(void) {
    const unsigned md = fx->scmr & 3;
    return 2u << (md - (md >> 1)); /* [md] = 2, 4, 4, 8 */
}

static void fx_pixel_flush(unsigned slot) {
    if (!fx->pix_pend[slot]) return;
    const uint16_t offset = fx->pix_offset[slot];
    const unsigned x = (offset << 3) & 0xFFu; /* the column stays 8 bits */
    const unsigned y = offset >> 5;
    const unsigned bpp = fx_bpp();
    const uint32_t addr = 0x700000u + (fx_tile_column(x, y) * (bpp << 3)) +
                          ((uint32_t)fx->scbr << 10) + ((y & 7) * 2);
    for (unsigned n = 0; n < bpp; n++) {
        const unsigned byte = ((n >> 1) << 4) + (n & 1);
        unsigned data = 0;
        for (unsigned p = 0; p < 8; p++) data |= ((fx->pix_data[slot][p] >> n) & 1) << p;
        if (fx->pix_pend[slot] != 0xFFu) {
            fx_tick(fx->clsr ? 5 : 6);
            data &= fx->pix_pend[slot];
            data |= fx_read(addr + byte) & (unsigned)~fx->pix_pend[slot];
        }
        fx_tick(fx->clsr ? 5 : 6);
        fx_write(addr + byte, (uint8_t)data);
    }
    fx->pix_pend[slot] = 0;
}

static void fx_pixel_roll(void) { /* slot 1 takes slot 0's line */
    fx->pix_offset[1] = fx->pix_offset[0];
    fx->pix_pend[1] = fx->pix_pend[0];
    memcpy(fx->pix_data[1], fx->pix_data[0], sizeof fx->pix_data[0]);
    fx->pix_pend[0] = 0;
}

static void fx_plot(uint8_t x, uint8_t y) {
    if (!(fx->por & 0x01)) { /* not transparent */
        if ((fx->scmr & 3) == 3) {
            if (fx->por & 0x08) { if ((fx->colr & 0x0f) == 0) return; }
            else if (fx->colr == 0) return;
        } else if ((fx->colr & 0x0f) == 0) {
            return;
        }
    }
    uint8_t color = fx->colr;
    if ((fx->por & 0x02) && (fx->scmr & 3) != 3) {
        if ((x ^ y) & 1) color >>= 4;
        color &= 0x0f;
    }
    const uint16_t offset = (uint16_t)((y << 5) + (x >> 3));
    if (offset != fx->pix_offset[0]) {
        fx_pixel_flush(1);
        fx_pixel_roll();
        fx->pix_offset[0] = offset;
    }
    x = (uint8_t)((x & 7) ^ 7);
    fx->pix_data[0][x] = color;
    fx->pix_pend[0] |= (uint8_t)(1u << x);
    if (fx->pix_pend[0] == 0xFFu) {
        fx_pixel_flush(1);
        fx_pixel_roll();
    }
}

static uint8_t fx_rpix(uint8_t x, uint8_t y) {
    fx_pixel_flush(1);
    fx_pixel_flush(0);
    const unsigned bpp = fx_bpp();
    const uint32_t addr = 0x700000u + (fx_tile_column(x, y) * (bpp << 3)) +
                          ((uint32_t)fx->scbr << 10) + ((y & 7) * 2);
    unsigned data = 0;
    x = (uint8_t)((x & 7) ^ 7);
    for (unsigned n = 0; n < bpp; n++) {
        const unsigned byte = ((n >> 1) << 4) + (n & 1);
        fx_tick(fx->clsr ? 5 : 6);
        data |= ((fx_read(addr + byte) >> x) & 1) << n;
    }
    return (uint8_t)data;
}

/* ---- the instructions ---- */

static void fx_reset_mods(void) {
    fx->sfr &= (uint16_t)~(FX_B | FX_ALT1 | FX_ALT2);
    fx->sreg = 0;
    fx->dreg = 0;
}

static void fx_set(unsigned n, unsigned v) {
    fx->r[n] = (uint16_t)v;
    fx->r_mod |= FX_MOD(n);
}

static uint16_t fx_sr(void) { return fx->r[fx->sreg]; }

static void fx_dr(unsigned v) { fx_set(fx->dreg, v); }

static void fx_flags_ld(uint16_t v) { /* S and Z of the 16-bit result */
    fx->sfr &= (uint16_t)~(FX_S | FX_Z);
    if (v & 0x8000) fx->sfr |= FX_S;
    if (v == 0) fx->sfr |= FX_Z;
}

static void fx_ins_stop(void) {
    if (!(fx->cfgr & 0x80)) {
        fx->sfr |= FX_IRQ;
        machine.fx_irq = 1; /* the CPU's IRQ line rises */
    }
    fx->sfr &= (uint16_t)~FX_G;
    fx->pipeline = 0x01; /* nop */
    fx_reset_mods();
}

static void fx_ins_nop(void) { fx_reset_mods(); }

static void fx_ins_cache(void) {
    if (fx->cbr != (fx->r[15] & 0xfff0u)) {
        fx->cbr = (uint16_t)(fx->r[15] & 0xfff0u);
        fx_flush_cache();
    }
    fx_reset_mods();
}

static void fx_ins_lsr(void) {
    fx->sfr = (uint16_t)((fx->sfr & ~FX_CY) | (fx_sr() & 1 ? FX_CY : 0));
    fx_dr(fx_sr() >> 1);
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_rol(void) {
    const unsigned carry = fx_sr() & 0x8000;
    fx_dr((fx_sr() << 1) | (fx->sfr & FX_CY ? 1u : 0u));
    fx_flags_ld(fx->r[fx->dreg]);
    fx->sfr = (uint16_t)((fx->sfr & ~FX_CY) | (carry ? FX_CY : 0));
    fx_reset_mods();
}

static void fx_ins_branch(int take) {
    const int8_t displacement = (int8_t)fx_pipe();
    if (take) fx_set(15, fx->r[15] + displacement);
}

static void fx_ins_to_move(unsigned n) {
    if (!(fx->sfr & FX_B)) {
        fx->dreg = n;
    } else {
        fx_set(n, fx_sr());
        fx_reset_mods();
    }
}

static void fx_ins_with(unsigned n) {
    fx->sreg = n;
    fx->dreg = n;
    fx->sfr |= FX_B;
}

static void fx_ins_store(unsigned n) {
    fx->ramaddr = fx->r[n];
    fx_rambuf_write(fx->ramaddr, (uint8_t)fx_sr());
    if (!(fx->sfr & FX_ALT1)) fx_rambuf_write((uint16_t)(fx->ramaddr ^ 1), (uint8_t)(fx_sr() >> 8));
    fx_reset_mods();
}

static void fx_ins_loop(void) {
    fx_set(12, fx->r[12] - 1);
    fx->sfr &= (uint16_t)~(FX_S | FX_Z);
    if (fx->r[12] & 0x8000) fx->sfr |= FX_S;
    if (fx->r[12] == 0) fx->sfr |= FX_Z;
    if (!(fx->sfr & FX_Z)) fx_set(15, fx->r[13]);
    fx_reset_mods();
}

static void fx_ins_alt1(void) { fx->sfr = (uint16_t)((fx->sfr & ~FX_B) | FX_ALT1); }

static void fx_ins_alt2(void) { fx->sfr = (uint16_t)((fx->sfr & ~FX_B) | FX_ALT2); }

static void fx_ins_alt3(void) {
    fx->sfr = (uint16_t)((fx->sfr & ~FX_B) | FX_ALT1 | FX_ALT2);
}

static void fx_ins_load(unsigned n) {
    fx->ramaddr = fx->r[n];
    unsigned v = fx_rambuf_read(fx->ramaddr);
    if (!(fx->sfr & FX_ALT1)) v |= (unsigned)fx_rambuf_read((uint16_t)(fx->ramaddr ^ 1)) << 8;
    fx_dr(v);
    fx_reset_mods();
}

static void fx_ins_plot_rpix(void) {
    if (!(fx->sfr & FX_ALT1)) {
        fx_plot((uint8_t)fx->r[1], (uint8_t)fx->r[2]);
        fx_set(1, fx->r[1] + 1);
    } else {
        fx_dr(fx_rpix((uint8_t)fx->r[1], (uint8_t)fx->r[2]));
        fx_flags_ld(fx->r[fx->dreg]);
    }
    fx_reset_mods();
}

static void fx_ins_swap(void) {
    fx_dr(fx_sr() >> 8 | fx_sr() << 8);
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_color_cmode(void) {
    if (!(fx->sfr & FX_ALT1)) fx_color_set((uint8_t)fx_sr());
    else fx->por = (uint8_t)fx_sr();
    fx_reset_mods();
}

static void fx_ins_not(void) {
    fx_dr(~fx_sr());
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_add_adc(unsigned n) {
    if (!(fx->sfr & FX_ALT2)) n = fx->r[n];
    const int r = fx_sr() + n + (fx->sfr & FX_ALT1 ? (fx->sfr & FX_CY ? 1u : 0u) : 0u);
    fx->sfr &= (uint16_t)~(FX_OV | FX_S | FX_CY | FX_Z);
    if (~(fx_sr() ^ n) & (n ^ r) & 0x8000) fx->sfr |= FX_OV;
    if (r & 0x8000) fx->sfr |= FX_S;
    if (r >= 0x10000) fx->sfr |= FX_CY;
    if ((uint16_t)r == 0) fx->sfr |= FX_Z;
    fx_dr((unsigned)r);
    fx_reset_mods();
}

static void fx_ins_sub_sbc_cmp(unsigned n) {
    if (!(fx->sfr & FX_ALT2) || (fx->sfr & FX_ALT1)) n = fx->r[n];
    const int r = fx_sr() - (int)n -
                  (!(fx->sfr & FX_ALT2) && (fx->sfr & FX_ALT1) ? !(fx->sfr & FX_CY) : 0);
    fx->sfr &= (uint16_t)~(FX_OV | FX_S | FX_CY | FX_Z);
    if ((fx_sr() ^ n) & (fx_sr() ^ r) & 0x8000) fx->sfr |= FX_OV;
    if (r & 0x8000) fx->sfr |= FX_S;
    if (r >= 0) fx->sfr |= FX_CY;
    if ((uint16_t)r == 0) fx->sfr |= FX_Z;
    if (!(fx->sfr & FX_ALT2) || !(fx->sfr & FX_ALT1)) fx_dr((unsigned)r);
    fx_reset_mods();
}

static void fx_ins_merge(void) {
    fx_dr((fx->r[7] & 0xff00) | (fx->r[8] >> 8));
    const unsigned v = fx->r[fx->dreg];
    fx->sfr &= (uint16_t)~(FX_OV | FX_S | FX_CY | FX_Z);
    if (v & 0xc0c0) fx->sfr |= FX_OV;
    if (v & 0x8080) fx->sfr |= FX_S;
    if (v & 0xe0e0) fx->sfr |= FX_CY;
    if (v & 0xf0f0) fx->sfr |= FX_Z;
    fx_reset_mods();
}

static void fx_ins_and_bic(unsigned n) {
    if (!(fx->sfr & FX_ALT2)) n = fx->r[n];
    fx_dr(fx_sr() & (fx->sfr & FX_ALT1 ? ~n : n));
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_mult_umult(unsigned n) {
    if (!(fx->sfr & FX_ALT2)) n = fx->r[n];
    fx_dr(!(fx->sfr & FX_ALT1) ? (unsigned)((int8_t)fx_sr() * (int8_t)n)
                               : (unsigned)((uint8_t)fx_sr() * (uint8_t)n));
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
    if (!(fx->cfgr & 0x20)) fx_tick(fx->clsr ? 1 : 2); /* MS0 clear: extra cycle */
}

static void fx_ins_sbk(void) {
    fx_rambuf_write(fx->ramaddr, (uint8_t)(fx_sr() >> 0));
    fx_rambuf_write((uint16_t)(fx->ramaddr ^ 1), (uint8_t)(fx_sr() >> 8));
    fx_reset_mods();
}

static void fx_ins_link(unsigned n) {
    fx_set(11, fx->r[15] + n);
    fx_reset_mods();
}

static void fx_ins_sex(void) {
    fx_dr((unsigned)(int16_t)(int8_t)fx_sr());
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_asr_div2(void) {
    fx->sfr = (uint16_t)((fx->sfr & ~FX_CY) | (fx_sr() & 1 ? FX_CY : 0));
    fx_dr((unsigned)((int16_t)fx_sr() >> 1) +
          (fx->sfr & FX_ALT1 ? ((fx_sr() + 1) >> 16) : 0));
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_ror(void) {
    const unsigned carry = fx_sr() & 1;
    fx_dr((fx->sfr & FX_CY ? 0x8000u : 0u) | (fx_sr() >> 1));
    fx_flags_ld(fx->r[fx->dreg]);
    fx->sfr = (uint16_t)((fx->sfr & ~FX_CY) | (carry ? FX_CY : 0));
    fx_reset_mods();
}

static void fx_ins_jmp_ljmp(unsigned n) {
    if (!(fx->sfr & FX_ALT1)) {
        fx_set(15, fx->r[n]);
    } else {
        fx->pbr = (uint8_t)(fx->r[n] & 0x7f);
        fx_set(15, fx_sr());
        fx->cbr = (uint16_t)(fx->r[15] & 0xfff0u);
        fx_flush_cache();
    }
    fx_reset_mods();
}

static void fx_ins_lob(void) {
    fx_dr(fx_sr() & 0xff);
    fx->sfr &= (uint16_t)~(FX_S | FX_Z);
    if (fx->r[fx->dreg] & 0x80) fx->sfr |= FX_S;
    if ((uint8_t)fx->r[fx->dreg] == 0) fx->sfr |= FX_Z;
    fx_reset_mods();
}

static void fx_ins_fmult_lmult(void) {
    const uint32_t result = (uint32_t)((int16_t)fx_sr() * (int16_t)fx->r[6]);
    if (fx->sfr & FX_ALT1) fx_set(4, result);
    fx_dr(result >> 16);
    fx->sfr &= (uint16_t)~(FX_S | FX_CY | FX_Z);
    if (fx->r[fx->dreg] & 0x8000) fx->sfr |= FX_S;
    if (result & 0x8000) fx->sfr |= FX_CY;
    if (fx->r[fx->dreg] == 0) fx->sfr |= FX_Z;
    fx_reset_mods();
    fx_tick((fx->cfgr & 0x20 ? 3u : 7u) * (fx->clsr ? 1u : 2u));
}

static void fx_ins_ibt_lms_sms(unsigned n) {
    if (fx->sfr & FX_ALT1) {
        fx->ramaddr = (uint16_t)(fx_pipe() << 1);
        const unsigned lo = fx_rambuf_read(fx->ramaddr);
        fx_set(n, ((unsigned)fx_rambuf_read((uint16_t)(fx->ramaddr ^ 1)) << 8) | lo);
    } else if (fx->sfr & FX_ALT2) {
        fx->ramaddr = (uint16_t)(fx_pipe() << 1);
        fx_rambuf_write(fx->ramaddr, (uint8_t)fx->r[n]);
        fx_rambuf_write((uint16_t)(fx->ramaddr ^ 1), (uint8_t)(fx->r[n] >> 8));
    } else {
        fx_set(n, (unsigned)(int8_t)fx_pipe());
    }
    fx_reset_mods();
}

static void fx_ins_from_moves(unsigned n) {
    if (!(fx->sfr & FX_B)) {
        fx->sreg = n;
    } else {
        fx_dr(fx->r[n]);
        fx->sfr &= (uint16_t)~(FX_OV | FX_S | FX_Z);
        if (fx->r[fx->dreg] & 0x80) fx->sfr |= FX_OV;
        if (fx->r[fx->dreg] & 0x8000) fx->sfr |= FX_S;
        if (fx->r[fx->dreg] == 0) fx->sfr |= FX_Z;
        fx_reset_mods();
    }
}

static void fx_ins_hib(void) {
    fx_dr(fx_sr() >> 8);
    fx->sfr &= (uint16_t)~(FX_S | FX_Z);
    if (fx->r[fx->dreg] & 0x80) fx->sfr |= FX_S;
    if ((uint8_t)fx->r[fx->dreg] == 0) fx->sfr |= FX_Z;
    fx_reset_mods();
}

static void fx_ins_or_xor(unsigned n) {
    if (!(fx->sfr & FX_ALT2)) n = fx->r[n];
    fx_dr(!(fx->sfr & FX_ALT1) ? fx_sr() | n : fx_sr() ^ n);
    fx_flags_ld(fx->r[fx->dreg]);
    fx_reset_mods();
}

static void fx_ins_inc(unsigned n) {
    fx_set(n, fx->r[n] + 1);
    fx_flags_ld(fx->r[n]);
    fx_reset_mods();
}

static void fx_ins_getc_ramb_romb(void) {
    if (!(fx->sfr & FX_ALT2)) {
        fx_color_set(fx_rombuf_read());
    } else if (!(fx->sfr & FX_ALT1)) {
        if (fx->ramcl) fx_tick(fx->ramcl);
        fx->rambr = fx_sr() & 0x01;
    } else {
        if (fx->romcl) fx_tick(fx->romcl);
        fx->rombr = (uint8_t)(fx_sr() & 0x7f);
    }
    fx_reset_mods();
}

static void fx_ins_dec(unsigned n) {
    fx_set(n, fx->r[n] - 1);
    fx_flags_ld(fx->r[n]);
    fx_reset_mods();
}

static void fx_ins_getb(void) {
    switch ((fx->sfr & FX_ALT2 ? 2u : 0u) | (fx->sfr & FX_ALT1 ? 1u : 0u)) {
    case 0: fx_dr(fx_rombuf_read()); break;
    case 1: fx_dr((fx_rombuf_read() << 8) | (uint8_t)fx_sr()); break;
    case 2: fx_dr((fx_sr() & 0xff00) | fx_rombuf_read()); break;
    default: fx_dr((unsigned)(int16_t)(int8_t)fx_rombuf_read()); break;
    }
    fx_reset_mods();
}

static void fx_ins_iwt_lm_sm(unsigned n) {
    if (fx->sfr & FX_ALT1) {
        fx->ramaddr = fx_pipe();
        fx->ramaddr |= (uint16_t)(fx_pipe() << 8);
        const unsigned lo = fx_rambuf_read(fx->ramaddr);
        fx_set(n, ((unsigned)fx_rambuf_read((uint16_t)(fx->ramaddr ^ 1)) << 8) | lo);
    } else if (fx->sfr & FX_ALT2) {
        fx->ramaddr = fx_pipe();
        fx->ramaddr |= (uint16_t)(fx_pipe() << 8);
        fx_rambuf_write(fx->ramaddr, (uint8_t)fx->r[n]);
        fx_rambuf_write((uint16_t)(fx->ramaddr ^ 1), (uint8_t)(fx->r[n] >> 8));
    } else {
        const unsigned lo = fx_pipe();
        fx_set(n, ((unsigned)fx_pipe() << 8) | lo);
    }
    fx_reset_mods();
}

static void fx_instruction(uint8_t op) {
    #define BRANCH(c) do { fx_ins_branch(c); return; } while (0)
    switch (op) {
    case 0x00: fx_ins_stop(); return;
    case 0x01: fx_ins_nop(); return;
    case 0x02: fx_ins_cache(); return;
    case 0x03: fx_ins_lsr(); return;
    case 0x04: fx_ins_rol(); return;
    case 0x05: BRANCH(1);
    case 0x06: BRANCH(((fx->sfr & FX_S) != 0) == ((fx->sfr & FX_OV) != 0));
    case 0x07: BRANCH(((fx->sfr & FX_S) != 0) != ((fx->sfr & FX_OV) != 0));
    case 0x08: BRANCH(!(fx->sfr & FX_Z));
    case 0x09: BRANCH((fx->sfr & FX_Z) != 0);
    case 0x0a: BRANCH(!(fx->sfr & FX_S));
    case 0x0b: BRANCH((fx->sfr & FX_S) != 0);
    case 0x0c: BRANCH(!(fx->sfr & FX_CY));
    case 0x0d: BRANCH((fx->sfr & FX_CY) != 0);
    case 0x0e: BRANCH(!(fx->sfr & FX_OV));
    case 0x0f: BRANCH((fx->sfr & FX_OV) != 0);
    case 0x3c: fx_ins_loop(); return;
    case 0x3d: fx_ins_alt1(); return;
    case 0x3e: fx_ins_alt2(); return;
    case 0x3f: fx_ins_alt3(); return;
    case 0x4c: fx_ins_plot_rpix(); return;
    case 0x4d: fx_ins_swap(); return;
    case 0x4e: fx_ins_color_cmode(); return;
    case 0x4f: fx_ins_not(); return;
    case 0x70: fx_ins_merge(); return;
    case 0x90: fx_ins_sbk(); return;
    case 0x95: fx_ins_sex(); return;
    case 0x96: fx_ins_asr_div2(); return;
    case 0x97: fx_ins_ror(); return;
    case 0x9e: fx_ins_lob(); return;
    case 0x9f: fx_ins_fmult_lmult(); return;
    case 0xc0: fx_ins_hib(); return;
    case 0xdf: fx_ins_getc_ramb_romb(); return;
    case 0xef: fx_ins_getb(); return;
    default: break;
    }
    const unsigned n = op & 15;
    if (op >= 0x10 && op <= 0x1f) { fx_ins_to_move(n); return; }
    if (op >= 0x20 && op <= 0x2f) { fx_ins_with(n); return; }
    if (op >= 0x30 && op <= 0x3b) { fx_ins_store(n); return; }
    if (op >= 0x40 && op <= 0x4b) { fx_ins_load(n); return; }
    if (op >= 0x50 && op <= 0x5f) { fx_ins_add_adc(n); return; }
    if (op >= 0x60 && op <= 0x6f) { fx_ins_sub_sbc_cmp(n); return; }
    if (op >= 0x71 && op <= 0x7f) { fx_ins_and_bic(n); return; }
    if (op >= 0x80 && op <= 0x8f) { fx_ins_mult_umult(n); return; }
    if (op >= 0x91 && op <= 0x94) { fx_ins_link(n); return; }
    if (op >= 0x98 && op <= 0x9d) { fx_ins_jmp_ljmp(n); return; }
    if (op >= 0xa0 && op <= 0xaf) { fx_ins_ibt_lms_sms(n); return; }
    if (op >= 0xb0 && op <= 0xbf) { fx_ins_from_moves(n); return; }
    if (op >= 0xc1 && op <= 0xcf) { fx_ins_or_xor(n); return; }
    if (op >= 0xd0 && op <= 0xde) { fx_ins_inc(n); return; }
    if (op >= 0xe0 && op <= 0xee) { fx_ins_dec(n); return; }
    if (op >= 0xf0 && op <= 0xff) { fx_ins_iwt_lm_sm(n); return; }
    fx_ins_nop(); /* the undefined opcodes do nothing */
}

/* ---- the CPU's register window ($6000-$7FFF) ---- */

uint8_t snes_fx_reg_read(uint16_t off) {
    const uint32_t addr = 0x3000u | (off & 0x3ffu);
    uint8_t v;
    if (addr >= 0x3100 && addr <= 0x32ff) {
        const uint16_t a = (uint16_t)((addr - 0x3100 + fx->cbr) & 511);
        v = fx->cache[a];
    } else if (addr <= 0x301f) {
        v = (uint8_t)(fx->r[(addr >> 1) & 15] >> ((addr & 1) << 3));
    } else {
        switch (addr) {
        case 0x3030: v = (uint8_t)fx->sfr; break;
        case 0x3031: {
            v = (uint8_t)(fx->sfr >> 8);
            fx->sfr &= (uint16_t)~FX_IRQ;
            machine.fx_irq = 0;
            machine.cpu.irq_line = machine.timeup || machine.sa1_cpu_irq; /* the line drops at once */
            break;
        }
        case 0x3034: v = fx->pbr; break;
        case 0x3036: v = fx->rombr; break;
        case 0x303b: v = 0x04; break; /* the GSU-1's version code */
        case 0x303c: v = fx->rambr; break;
        case 0x303e: v = (uint8_t)fx->cbr; break;
        case 0x303f: v = (uint8_t)(fx->cbr >> 8); break;
        default: v = 0x00; break;
        }
    }
    return v;
}

void snes_fx_reg_write(uint16_t off, uint8_t v) {
    const uint32_t addr = 0x3000u | (off & 0x3ffu);
    if (addr >= 0x3100 && addr <= 0x32ff) {
        const uint16_t a = (uint16_t)((addr - 0x3100 + fx->cbr) & 511);
        fx->cache[a] = v;
        if ((a & 15) == 15) fx->cache_valid[a >> 4] = 1;
        return;
    }
    if (addr <= 0x301f) {
        const unsigned n = (addr >> 1) & 15;
        if (!(addr & 1)) fx->r[n] = (uint16_t)((fx->r[n] & 0xff00) | v);
        else fx->r[n] = (uint16_t)((v << 8) | (fx->r[n] & 0xff));
        if (n == 14) fx_rombuf_update();
        if (addr == 0x301f) fx->sfr |= FX_G; /* the high byte of R15 = go */
        return;
    }
    switch (addr) {
    case 0x3030: {
        const unsigned g = fx->sfr & FX_G;
        fx->sfr = (uint16_t)((fx->sfr & 0xff00) | v);
        if (g && !(fx->sfr & FX_G)) {
            fx->cbr = 0;
            fx_flush_cache();
        }
        break;
    }
    case 0x3031: fx->sfr = (uint16_t)((v << 8) | (fx->sfr & 0x00ff)); break;
    case 0x3033: fx->bramr = v & 0x01; break;
    case 0x3034: fx->pbr = (uint8_t)(v & 0x7f); fx_flush_cache(); break;
    case 0x3037: fx->cfgr = v; break;
    case 0x3038: fx->scbr = v; break;
    case 0x3039: fx->clsr = (uint8_t)(v & 0x01); break;
    case 0x303a: fx->scmr = v; break;
    default: break;
    }
}

/* ---- the clock: credit the elapsed CPU time and drain it ---- */

void snes_fx_run(uint32_t target_cpu) {
    if (!machine.fx_attached) return;
    const int64_t delta = (int64_t)target_cpu - (int64_t)fx->last_cpu;
    if (delta < 0) return; /* this CPU time is already credited */
    fx->last_cpu = target_cpu;
    fx->bank += (int32_t)(delta * 6); /* the GSU runs at six times the CPU */
    while (fx->bank >= 0) {
        if (!(fx->sfr & FX_G)) {
            fx_tick(6);
            continue;
        }
        fx_instruction(fx_peekpipe());
        if (fx->r_mod & FX_MOD(14)) fx_rombuf_update();
        if (!(fx->r_mod & FX_MOD(15))) fx->r[15]++;
        fx->r_mod = 0;
    }
}

void snes_fx_reset(void) {
    memset(fx, 0, sizeof *fx);
    fx->pipeline = 0x01; /* nop */
}
