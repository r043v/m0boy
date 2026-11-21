/* The TLCS-900H interpreter.  The instruction set follows the model the NGP
 * emulation lineage implements (NeoPop, cross-checked against MAME's
 * TLCS-900H tables, which agree byte for byte on the opcode map): four
 * register banks of XWA/XBC/XDE/XHL plus XIX/XIY/XIZ/XSP, the register
 * codes 0x00-0x3F naming every bank's registers (byte = code&3, register =
 * (code>>2)&3, bank = code>>4), 0xE0-0xEF the current bank and 0xF0-0xFF
 * the global registers at every width (words index code>>1, longs code>>2).
 * The 8-bit registers of a pair: A low / W high of XWA, C low / B high of
 * XBC, E low / D high of XDE, L low / H high of XHL.
 * Cycle counts are the lineage's documented approximations; the interpreter
 * and the recompiled code share them, which is what the run_hash contract
 * needs. */
#ifndef M0_TL
#define M0_TL
#endif
#include "ngp.h"
#include "ngp_trace.h"

/* flags in sr */
#define F_S 0x80
#define F_Z 0x40
#define F_H 0x10
#define F_V 0x04
#define F_N 0x02
#define F_C 0x01

/* the decode state of the instruction in flight */
static M0_TL uint8_t first, second, R;
static M0_TL uint32_t mem;
static M0_TL int size;              /* 0 byte, 1 word, 2 long */
static M0_TL int32_t cycles, cycles_extra;
static M0_TL int brCode;
static M0_TL uint8_t rCode;
static M0_TL uint32_t r_err;        /* the cell invalid register codes read */
static M0_TL uint32_t ngp_last_good;

/* ---- register file ---------------------------------------------------------- */

static unsigned rfp(void) { return (machine.cpu.sr >> 8) & 3; }

/* the bank-relative views, gprMap order: bytes {W, A, B, C, D, E, H, L}
 * (each pair's high byte first), words {WA, BC, DE, HL} then the global
 * words, longs {XWA, XBC, XDE, XHL} then XIX XIY XIZ XSP */
static uint32_t *lreg(unsigned r) {
    return r < 4 ? &machine.cpu.bank[rfp()][r] : &machine.cpu.gpr[r - 4];
}

static uint8_t reg_b(unsigned r) {
    const uint32_t v = *lreg(r >> 1);
    return (uint8_t)((r & 1) ? v : (v >> 8)); /* even index: the high byte */
}

static void reg_b_set(unsigned r, uint8_t v) {
    uint32_t *p = lreg(r >> 1);
    if (r & 1) *p = (*p & 0xFFFFFF00u) | v;
    else *p = (*p & 0xFFFF00FFu) | ((uint32_t)v << 8);
}

static uint16_t reg_w(unsigned r) {
    return (uint16_t)(*lreg(r) & 0xFFFF);
}

static void reg_w_set(unsigned r, uint16_t v) {
    uint32_t *p = lreg(r);
    *p = (*p & 0xFFFF0000u) | v;
}

static uint32_t reg_l(unsigned r) { return *lreg(r); }
static void reg_l_set(unsigned r, uint32_t v) { *lreg(r) = v; }

#define REGA   reg_b(1)         /* A, the low byte of XWA (read) */
#define REGWA  reg_w(0)         /* WA */
#define REGBC  reg_w(1)         /* BC, the block-op counter */
#define REGXSP machine.cpu.gpr[3]

/* register-code access; an invalid code reads and writes the shared r_err
 * cell, as the reference model does */
uint8_t ngp_code_b(unsigned r) {
    unsigned bank, reg, byte;
    if (r < 0x40) { bank = r >> 4; reg = (r >> 2) & 3; byte = r & 3; }
    else if (r >= 0xE0 && r < 0xF0) { bank = rfp(); reg = (r >> 2) & 3; byte = r & 3; }
    else if (r >= 0xF0) return (uint8_t)(machine.cpu.gpr[(r >> 2) & 3] >> ((r & 3) * 8));
    else return (uint8_t)r_err;
    return (uint8_t)(machine.cpu.bank[bank][reg] >> (byte * 8));
}

void ngp_code_b_set(unsigned r, uint8_t v) {
    unsigned bank, reg, byte;
    if (r < 0x40) { bank = r >> 4; reg = (r >> 2) & 3; byte = r & 3; }
    else if (r >= 0xE0 && r < 0xF0) { bank = rfp(); reg = (r >> 2) & 3; byte = r & 3; }
    else if (r >= 0xF0) {
        uint32_t *p = &machine.cpu.gpr[(r >> 2) & 3];
        *p = (*p & ~(0xFFu << ((r & 3) * 8))) | ((uint32_t)v << ((r & 3) * 8));
        return;
    }
    else { r_err = v; return; }
    {
        uint32_t *p = &machine.cpu.bank[bank][reg];
        *p = (*p & ~(0xFFu << (byte * 8))) | ((uint32_t)v << (byte * 8));
    }
}

uint16_t ngp_code_w(unsigned r) {
    const unsigned e = r >> 1;
    if (e < 32) return (uint16_t)(machine.cpu.bank[e >> 3][(e >> 1) & 3] >> ((e & 1) * 16));
    if (r >= 0xE0 && r < 0xF0) {
        /* the current bank's eight words: two per register (low, high) */
        const unsigned w = e - 112;
        return (uint16_t)(machine.cpu.bank[rfp()][w >> 1] >> ((w & 1) * 16));
    }
    if (r >= 0xF0) {
        const unsigned w = e - 120;
        return (uint16_t)(machine.cpu.gpr[w >> 1] >> ((w & 1) * 16));
    }
    return (uint16_t)r_err;
}

void ngp_code_w_set(unsigned r, uint16_t v) {
    const unsigned e = r >> 1;
    uint32_t *p;
    unsigned hi;
    if (e < 32) { p = &machine.cpu.bank[e >> 3][(e >> 1) & 3]; hi = e & 1; }
    else if (r >= 0xE0 && r < 0xF0) {
        const unsigned w = e - 112;
        p = &machine.cpu.bank[rfp()][w >> 1];
        hi = w & 1;
    }
    else if (r >= 0xF0) {
        const unsigned w = e - 120;
        p = &machine.cpu.gpr[w >> 1];
        hi = w & 1;
    }
    else { r_err = v; return; }
    if (hi) *p = (*p & 0xFFFFu) | ((uint32_t)v << 16);
    else *p = (*p & 0xFFFF0000u) | v;
}

uint32_t ngp_code_l(unsigned r) {
    const unsigned e = r >> 2;
    if (e < 16) return machine.cpu.bank[e >> 2][e & 3];
    if (r >= 0xE0 && r < 0xF0) {
        const unsigned i = e - 56;
        return i < 4 ? machine.cpu.bank[rfp()][i] : machine.cpu.gpr[i - 4];
    }
    if (r >= 0xF0) return machine.cpu.gpr[e - 60];
    return r_err;
}

void ngp_code_l_set(unsigned r, uint32_t v) {
    const unsigned e = r >> 2;
    if (e < 16) machine.cpu.bank[e >> 2][e & 3] = v;
    else if (r >= 0xE0 && r < 0xF0) {
        const unsigned i = e - 56;
        if (i < 4) machine.cpu.bank[rfp()][i] = v;
        else machine.cpu.gpr[i - 4] = v;
    }
    else if (r >= 0xF0) machine.cpu.gpr[e - 60] = v;
    else r_err = v;
}

/* ---- flags ------------------------------------------------------------------- */

static void set_c(int c) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_C) | (c ? F_C : 0)); }
static void set_v(int v) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_V) | (v ? F_V : 0)); }
static void set_h(int h) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_H) | (h ? F_H : 0)); }
static void set_n(int n) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_N) | (n ? F_N : 0)); }
static void set_z(int z) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_Z) | (z ? F_Z : 0)); }
static void set_s(int s) { machine.cpu.sr = (uint16_t)((machine.cpu.sr & ~F_S) | (s ? F_S : 0)); }
#define GET_C ((machine.cpu.sr & F_C) != 0)
#define GET_Z ((machine.cpu.sr & F_Z) != 0)
#define GET_H ((machine.cpu.sr & F_H) != 0)
#define GET_V ((machine.cpu.sr & F_V) != 0)
#define GET_N ((machine.cpu.sr & F_N) != 0)
#define GET_S ((machine.cpu.sr & F_S) != 0)

static void parity_b(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    set_v(!(v & 1)); /* V holds parity: 1 = even */
}

static void parity_w(uint16_t v) {
    v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    set_v(!(v & 1));
}

static int condition(int cc) {
    switch (cc) {
        case 0: return 0;                           /* F */
        case 1: return GET_S ^ GET_V;               /* LT */
        case 2: return GET_Z | (GET_S ^ GET_V);     /* LE */
        case 3: return GET_C | GET_Z;               /* ULE */
        case 4: return GET_V;                       /* OV */
        case 5: return GET_S;                       /* MI */
        case 6: return GET_Z;                       /* EQ */
        case 7: return GET_C;                       /* ULT */
        case 8: return 1;                           /* T */
        case 9: return !(GET_S ^ GET_V);            /* GE */
        case 10: return !(GET_Z | (GET_S ^ GET_V)); /* GT */
        case 11: return !(GET_C | GET_Z);           /* UGT */
        case 12: return !GET_V;                     /* NOV */
        case 13: return !GET_S;                     /* PL */
        case 14: return !GET_Z;                     /* NE */
        default: return !GET_C;                     /* UGE */
    }
}

/* ---- stack -------------------------------------------------------------------- */

static void push8(uint8_t v)  { REGXSP -= 1; ngp_write(REGXSP, v); }
static void push16(uint16_t v){ REGXSP -= 2; ngp_write16(REGXSP, v); }
static void push32(uint32_t v){ REGXSP -= 4; ngp_write32(REGXSP, v); }
static uint8_t pop8(void)  { const uint8_t v = ngp_read(REGXSP); REGXSP += 1; return v; }
static uint16_t pop16(void){ const uint16_t v = ngp_read16(REGXSP); REGXSP += 2; return v; }
static uint32_t pop32(void){ const uint32_t v = ngp_read32(REGXSP); REGXSP += 4; return v; }

/* ---- fetch --------------------------------------------------------------------- */

#ifdef NGP_TRACE
/* data accesses go through the tracer; the fetch functions mark themselves so
 * the opcode bytes stay unobserved (the kernel counts data accesses only) */
static M0_TL int dp_fetching;
static uint8_t dp_rd8(uint32_t a)  { const uint8_t v = ngp_read(a);   if (!dp_fetching) ngp_trace_read(a, v); return v; }
static uint16_t dp_rd16(uint32_t a){ const uint16_t v = ngp_read16(a); if (!dp_fetching) { ngp_trace_read(a, (uint8_t)v); ngp_trace_read(a + 1, (uint8_t)(v >> 8)); } return v; }
static uint32_t dp_rd32(uint32_t a){ const uint32_t v = ngp_read32(a); if (!dp_fetching) { ngp_trace_read(a, (uint8_t)v); ngp_trace_read(a + 1, (uint8_t)(v >> 8)); ngp_trace_read(a + 2, (uint8_t)(v >> 16)); ngp_trace_read(a + 3, (uint8_t)(v >> 24)); } return v; }
static void dp_wr8(uint32_t a, uint8_t v)   { ngp_write(a, v); ngp_trace_write(a, v); }
static void dp_wr16(uint32_t a, uint16_t v) { ngp_write16(a, v); ngp_trace_write(a, (uint8_t)v); ngp_trace_write(a + 1, (uint8_t)(v >> 8)); }
static void dp_wr32(uint32_t a, uint32_t v) { ngp_write32(a, v); ngp_trace_write(a, (uint8_t)v); ngp_trace_write(a + 1, (uint8_t)(v >> 8)); ngp_trace_write(a + 2, (uint8_t)(v >> 16)); ngp_trace_write(a + 3, (uint8_t)(v >> 24)); }
#define ngp_read(a) dp_rd8(a)
#define ngp_read16(a) dp_rd16(a)
#define ngp_read32(a) dp_rd32(a)
#define ngp_write(a, v) dp_wr8((a), (v))
#define ngp_write16(a, v) dp_wr16((a), (v))
#define ngp_write32(a, v) dp_wr32((a), (v))
#endif

static uint8_t fetch8(void) {
#ifdef NGP_TRACE
    dp_fetching = 1; const uint8_t v = ngp_read(machine.cpu.pc++); dp_fetching = 0; return v;
#else
    return ngp_read(machine.cpu.pc++);
#endif
}
static uint16_t fetch16(void) {
#ifdef NGP_TRACE
    dp_fetching = 1;
#endif
    const uint16_t v = ngp_read16(machine.cpu.pc); machine.cpu.pc += 2;
#ifdef NGP_TRACE
    dp_fetching = 0;
#endif
    return v;
}
static uint32_t fetch24(void) {
#ifdef NGP_TRACE
    dp_fetching = 1;
#endif
    const uint32_t lo = ngp_read16(machine.cpu.pc); machine.cpu.pc += 2;
#ifdef NGP_TRACE
    dp_fetching = 0;
#endif
    return lo | ((uint32_t)fetch8() << 16);
}
static uint32_t fetch32(void) {
#ifdef NGP_TRACE
    dp_fetching = 1;
#endif
    const uint32_t v = ngp_read32(machine.cpu.pc); machine.cpu.pc += 4;
#ifdef NGP_TRACE
    dp_fetching = 0;
#endif
    return v;
}

/* ---- arithmetic ---------------------------------------------------------------- */

static uint8_t add_b(uint8_t d, uint8_t s) {
    const uint8_t half = (uint8_t)((d & 0xF) + (s & 0xF));
    const uint32_t rc = (uint32_t)d + s;
    const uint8_t r = (uint8_t)rc;
    set_s(r & 0x80); set_z(r == 0); set_h(half > 0xF);
    set_v(((d & 0x80) == (s & 0x80)) && ((r & 0x80) != (d & 0x80)));
    set_n(0); set_c(rc > 0xFF);
    return r;
}
static uint16_t add_w(uint16_t d, uint16_t s) {
    const uint8_t half = (uint8_t)((d & 0xF) + (s & 0xF));
    const uint32_t rc = (uint32_t)d + s;
    const uint16_t r = (uint16_t)rc;
    set_s(r & 0x8000); set_z(r == 0); set_h(half > 0xF);
    set_v(((d & 0x8000) == (s & 0x8000)) && ((r & 0x8000) != (d & 0x8000)));
    set_n(0); set_c(rc > 0xFFFF);
    return r;
}
static uint32_t add_l(uint32_t d, uint32_t s) {
    const uint64_t rc = (uint64_t)d + s;
    const uint32_t r = (uint32_t)rc;
    set_s(r >> 31); set_z(r == 0);
    set_v(((d >> 31) == (s >> 31)) && ((r >> 31) != (d >> 31)));
    set_n(0); set_c(rc > 0xFFFFFFFFu);
    return r;
}
static uint8_t adc_b(uint8_t d, uint8_t s) { return add_b(d, (uint8_t)(s + GET_C)); }
static uint16_t adc_w(uint16_t d, uint16_t s) { return add_w(d, (uint16_t)(s + GET_C)); }
static uint32_t adc_l(uint32_t d, uint32_t s) { return add_l(d, s + GET_C); }

static uint8_t sub_b(uint8_t d, uint8_t s) {
    const uint8_t half = (uint8_t)((d & 0xF) - (s & 0xF));
    const uint32_t rc = (uint32_t)d - s;
    const uint8_t r = (uint8_t)rc;
    set_s(r & 0x80); set_z(r == 0); set_h(half > 0xF);
    set_v(((d & 0x80) != (s & 0x80)) && ((r & 0x80) != (d & 0x80)));
    set_n(1); set_c(rc > 0xFF);
    return r;
}
static uint16_t sub_w(uint16_t d, uint16_t s) {
    const uint8_t half = (uint8_t)((d & 0xF) - (s & 0xF));
    const uint32_t rc = (uint32_t)d - s;
    const uint16_t r = (uint16_t)rc;
    set_s(r & 0x8000); set_z(r == 0); set_h(half > 0xF);
    set_v(((d & 0x8000) != (s & 0x8000)) && ((r & 0x8000) != (d & 0x8000)));
    set_n(1); set_c(rc > 0xFFFF);
    return r;
}
static uint32_t sub_l(uint32_t d, uint32_t s) {
    const uint64_t rc = (uint64_t)d - s;
    const uint32_t r = (uint32_t)rc;
    set_s(r >> 31); set_z(r == 0);
    set_v(((d >> 31) != (s >> 31)) && ((r >> 31) != (d >> 31)));
    set_n(1); set_c(rc > 0xFFFFFFFFu);
    return r;
}
static uint8_t sbc_b(uint8_t d, uint8_t s) { return sub_b(d, (uint8_t)(s + GET_C)); }
static uint16_t sbc_w(uint16_t d, uint16_t s) { return sub_w(d, (uint16_t)(s + GET_C)); }
static uint32_t sbc_l(uint32_t d, uint32_t s) { return sub_l(d, s + GET_C); }

static void logic_flags(uint32_t r, int width) {
    set_s(width == 0 ? r & 0x80 : width == 1 ? (r & 0x8000) != 0 : r >> 31);
    set_z(r == 0);
    if (width == 0) parity_b((uint8_t)r);
    else if (width == 1) parity_w((uint16_t)r);
}

/* DIV: the quotient lands in the low half, the remainder in the high half;
 * V flags an overflow or a zero divisor */
static uint16_t div_b(uint16_t val, uint8_t div) {
    if (!div) { set_v(1); return (uint16_t)((val << 8) | ((val >> 8) ^ 0xFF)); }
    const uint16_t q = (uint16_t)(val / div), rm = (uint16_t)(val % div);
    set_v(q > 0xFF);
    return (uint16_t)((q & 0xFF) | (rm << 8));
}
static uint32_t div_w(uint32_t val, uint16_t div) {
    if (!div) { set_v(1); return (val << 16) | ((val >> 16) ^ 0xFFFF); }
    const uint32_t q = val / div, rm = val % div;
    set_v(q > 0xFFFF);
    return (q & 0xFFFF) | (rm << 16);
}
static uint16_t divs_b(int16_t val, int8_t div) {
    if (!div) { set_v(1); return (uint16_t)(((uint16_t)val << 8) | (((uint16_t)val >> 8) ^ 0xFF)); }
    const int16_t q = (int16_t)(val / div), rm = (int16_t)(val % div);
    set_v(q > 0xFF || q < -256);
    return (uint16_t)((q & 0xFF) | ((rm & 0xFF) << 8));
}
static uint32_t divs_w(int32_t val, int16_t div) {
    if (!div) { set_v(1); return ((uint32_t)val << 16) | (((uint32_t)val >> 16) ^ 0xFFFF); }
    const int32_t q = val / div, rm = val % div;
    set_v(q > 0xFFFF || q < -65536);
    return (uint32_t)((q & 0xFFFF) | ((rm & 0xFFFF) << 16));
}

/* the MUL/DIV pair register: named by the first byte's low bits (WA BC DE
 * HL XIX XIY XIZ XSP at word width, the longs at long width); byte-width
 * MUL/DIV write a word */
/* the MUL/DIV pair register: the byte forms name a pair with the odd
 * nibbles (1 WA, 3 BC, 5 DE, 7 HL), the word forms the whole nibble
 * (0-3 WA BC DE HL, 4-7 XIX XIY XIZ XSP); byte-width MUL/DIV write a word.
 * The register forms name it from the first byte, the (mem) forms from the
 * second byte's low bits. */
static uint8_t rr_nibble(unsigned n) {
    switch (n & 7) {
        case 0: return size == 1 ? 0xE0 : 0x80;
        case 1: return size == 0 ? 0xE0 : size == 1 ? 0xE4 : 0x80;
        case 2: return size == 1 ? 0xE8 : 0x80;
        case 3: return size == 0 ? 0xE4 : size == 1 ? 0xEC : 0x80;
        case 4: return size == 1 ? 0xF0 : 0x80;
        case 5: return size == 0 ? 0xE8 : size == 1 ? 0xF4 : 0x80;
        case 6: return size == 1 ? 0xF8 : 0x80;
        default: return size == 0 ? 0xEC : size == 1 ? 0xFC : 0x80;
    }
}
static uint8_t rr_target(int from_second) {
    if (size == 0 && first == 0xC7) return rCode;
    return rr_nibble(from_second ? second : first);
}

/* ---- shifts --------------------------------------------------------------------- */

/* kind: 0 RLC 1 RRC 2 RL 3 RR 4 SLA 5 SRA 6 SLL 7 SRL */
static void shift_val(uint32_t *v, int width, unsigned kind, unsigned count) {
    const uint32_t msb = width == 0 ? 0x80u : width == 1 ? 0x8000u : 0x80000000u;
    const uint32_t mask = width == 0 ? 0xFFu : width == 1 ? 0xFFFFu : 0xFFFFFFFFu;
    for (unsigned i = 0; i < count; i++) {
        const int old = GET_C;
        switch (kind) {
            case 0: /* RLC */
                set_c((*v & msb) != 0);
                *v = ((*v << 1) & mask) | (GET_C ? 1 : 0);
                break;
            case 1: /* RRC */
                set_c((*v & 1) != 0);
                *v = ((*v >> 1) & mask) | (GET_C ? msb : 0);
                break;
            case 2: { /* RL */
                const int ms = (*v & msb) != 0;
                *v = ((*v << 1) & mask) | (old ? 1 : 0);
                set_c(ms);
                break;
            }
            case 3: { /* RR */
                const int ls = (*v & 1) != 0;
                *v = ((*v >> 1) & mask) | (old ? msb : 0);
                set_c(ls);
                break;
            }
            case 4: case 6: /* SLA / SLL */
                set_c((*v & msb) != 0);
                *v = (*v << 1) & mask;
                break;
            case 5: /* SRA */
                set_c((*v & 1) != 0);
                *v = ((*v >> 1) & mask) | ((*v & msb) ? msb : 0);
                break;
            default: /* SRL */
                set_c((*v & 1) != 0);
                *v = (*v >> 1) & mask;
                break;
        }
    }
}

static void shift_reg(unsigned kind, unsigned count) {
    uint32_t v;
    if (size == 0) { v = ngp_code_b(rCode); shift_val(&v, 0, kind, count); ngp_code_b_set(rCode, (uint8_t)v); }
    else if (size == 1) { v = ngp_code_w(rCode); shift_val(&v, 1, kind, count); ngp_code_w_set(rCode, (uint16_t)v); }
    else { v = ngp_code_l(rCode); shift_val(&v, 2, kind, count); ngp_code_l_set(rCode, v); }
    set_s(size == 0 ? v & 0x80 : size == 1 ? (v & 0x8000) != 0 : v >> 31);
    set_z(v == 0);
    if (size == 0) parity_b((uint8_t)v);
    else if (size == 1) parity_w((uint16_t)v);
    set_h(0); set_n(0);
}

static void shift_mem(unsigned kind) {
    uint32_t v;
    if (size == 0) { v = ngp_read(mem); shift_val(&v, 0, kind, 1); ngp_write(mem, (uint8_t)v); }
    else { v = ngp_read16(mem); shift_val(&v, 1, kind, 1); ngp_write16(mem, (uint16_t)v); }
    set_s(size == 0 ? v & 0x80 : (v & 0x8000) != 0);
    set_z(v == 0);
    if (size == 0) parity_b((uint8_t)v); else parity_w((uint16_t)v);
    set_h(0); set_n(0);
}

/* ---- addressing ---------------------------------------------------------------- */

/* the memory operand of an instruction from its first byte: the (reg) and
 * (reg+d8) modes of 0x80-0xBF, the explicit modes of 0xC0-0xC5 and
 * 0xF0-0xF5, and the register-code operand of 0xC7-0xE7 */
static void decode_addr(uint8_t f) {
    if (f >= 0x80 && f < 0xC0) {
        const uint32_t base = reg_l(f & 7);
        if (f & 8) { mem = base + (int8_t)fetch8(); cycles_extra = 2; }
        else mem = base;
        return;
    }
    /* the explicit modes belong to the src group's 0xC0-0xC5/0xD0-0xD5/
     * 0xE0-0xE5 and the dst group's 0xF0-0xF5, plus the register-code
     * operand of 0xC7/0xD7/0xE7; the reg group (x8-xF) has none */
    if (f < 0xC0 || f > 0xF5) return;
    if ((f >= 0xC8 && f <= 0xCF) || (f >= 0xD8 && f <= 0xDF) || (f >= 0xE8 && f <= 0xEF)) return;
    switch (f & 7) {
        case 0: mem = fetch8(); cycles_extra = 2; return;
        case 1: mem = fetch16(); cycles_extra = 2; return;
        case 2: mem = fetch24(); cycles_extra = 3; return;
        case 3: { /* the indexed modes */
            const uint8_t data = fetch8();
            if (data == 0x03) {
                const uint8_t r32 = fetch8(), rI = fetch8();
                mem = ngp_code_l(r32) + (int8_t)ngp_code_b(rI);
                cycles_extra = 8;
            } else if (data == 0x07) {
                const uint8_t r32 = fetch8(), rI = fetch8();
                mem = ngp_code_l(r32) + (int16_t)ngp_code_w(rI);
                cycles_extra = 8;
            } else if (data == 0x13) { /* undocumented: PC relative */
                const int16_t d = (int16_t)fetch16();
                mem = machine.cpu.pc + (uint32_t)d;
                cycles_extra = 8;
            } else {
                cycles_extra = 5;
                mem = ngp_code_l(data);
                if ((data & 3) == 1) mem += (int16_t)fetch16();
            }
            return;
        }
        case 4: { /* (r32-dec) */
            const uint8_t data = fetch8();
            const uint8_t r32 = data & 0xFC;
            cycles_extra = 3;
            switch (data & 3) {
                case 0: ngp_code_l_set(r32, ngp_code_l(r32) - 1); mem = ngp_code_l(r32); break;
                case 1: ngp_code_l_set(r32, ngp_code_l(r32) - 2); mem = ngp_code_l(r32); break;
                case 2: ngp_code_l_set(r32, ngp_code_l(r32) - 4); mem = ngp_code_l(r32); break;
                default: mem = ngp_code_l(r32); break;
            }
            return;
        }
        case 5: { /* (r32-inc) */
            const uint8_t data = fetch8();
            const uint8_t r32 = data & 0xFC;
            cycles_extra = 3;
            switch (data & 3) {
                case 0: mem = ngp_code_l(r32); ngp_code_l_set(r32, ngp_code_l(r32) + 1); break;
                case 1: mem = ngp_code_l(r32); ngp_code_l_set(r32, ngp_code_l(r32) + 2); break;
                case 2: mem = ngp_code_l(r32); ngp_code_l_set(r32, ngp_code_l(r32) + 4); break;
                default: mem = ngp_code_l(r32); break;
            }
            return;
        }
        case 7:
            if (f >= 0xC7 && f < 0xF0) { brCode = 1; rCode = fetch8(); cycles_extra = 1; }
            return;
        default: return;
    }
}

/* ---- the src group (0x80-0xAF, 0xC0-0xC5, 0xD0-0xD5, 0xE0-0xE5) ------------------ */

static void block_op(uint8_t op) {
    /* LDI/LDIR/LDD/LDDR and CPI/CPIR/CPD/CPDR; LDI variants use XDE/XHL,
     * or XIX/XIY when the first byte names (XIY) as its mode */
    const int rep = op & 1, dec = op & 2, cmp = op & 4;
    /* base form (XDE) -> (XHL); the (first&0xF)==5 form is (XIX) -> (XIY).
     * The old 6/7 mapping wrote XIZ and walked the stack, and the next
     * RET executed a garbage address. */
    const unsigned dst = (first & 0xF) == 5 ? 4 : 2;
    const unsigned src = (first & 0xF) == 5 ? 5 : 3;
    const unsigned ptr = first & 7;
    const uint32_t step = size == 0 ? 1 : 2;
    cycles = 10;
    for (;;) {
        if (!cmp) {
            if (size == 0) ngp_write(reg_l(dst), ngp_read(reg_l(src)));
            else ngp_write16(reg_l(dst), ngp_read16(reg_l(src)));
            reg_l_set(dst, reg_l(dst) + (dec ? -step : step));
            reg_l_set(src, reg_l(src) + (dec ? -step : step));
        } else {
            if (size == 0) sub_b(REGA, ngp_read(reg_l(ptr)));
            else sub_w(REGWA, ngp_read16(reg_l(ptr)));
            reg_l_set(ptr, reg_l(ptr) + (dec ? -step : step));
        }
        reg_w_set(1, (uint16_t)(REGBC - 1));
        set_v(REGBC != 0);
        if (!cmp) { set_h(0); set_n(0); }
        if (!rep || !GET_V || (cmp && GET_Z)) break;
        cycles += 14;
    }
    if (!rep) cycles = 10;
}

static void src_ops(uint8_t op) {
    R = op & 7;
    switch (op) {
        case 0x04: /* PUSH (mem) */
            if (size == 0) push8(ngp_read(mem));
            else if (size == 1) push16(ngp_read16(mem));
            else push32(ngp_read32(mem));
            cycles = 7;
            return;
        case 0x06: { /* RLD A,(mem) */
            const uint8_t al = REGA & 0xF, m = ngp_read(mem);
            reg_b_set(1, (uint8_t)((REGA & 0xF0) | (m >> 4)));
            ngp_write(mem, (uint8_t)(((m & 0x0F) << 4) | al));
            set_s(REGA & 0x80); set_z(REGA == 0); set_h(0); set_n(0); parity_b(REGA);
            cycles = 12;
            return;
        }
        case 0x07: { /* RRD A,(mem) */
            const uint8_t m = ngp_read(mem);
            reg_b_set(1, (uint8_t)((REGA & 0xF0) | (m & 0x0F)));
            ngp_write(mem, (uint8_t)(((REGA & 0x0F) << 4) | (m >> 4)));
            set_s(REGA & 0x80); set_z(REGA == 0); set_h(0); set_n(0); parity_b(REGA);
            cycles = 12;
            return;
        }
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
            block_op(op);
            return;
        case 0x19: /* LD (nn),(mem) */
            if (size == 0) ngp_write(fetch16(), ngp_read(mem));
            else ngp_write16(fetch16(), ngp_read16(mem));
            cycles = 8;
            return;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: /* LD r,(mem) */
            if (size == 0) { reg_b_set(R, ngp_read(mem)); cycles = 4; }
            else if (size == 1) { reg_w_set(R, ngp_read16(mem)); cycles = 4; }
            else { reg_l_set(R, ngp_read32(mem)); cycles = 6; }
            return;
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37: /* EX (mem),r */
            if (size == 0) { const uint8_t t = reg_b(R); reg_b_set(R, ngp_read(mem)); ngp_write(mem, t); }
            else { const uint16_t t = reg_w(R); reg_w_set(R, ngp_read16(mem)); ngp_write16(mem, t); }
            cycles = 6;
            return;
        case 0x38: case 0x39: case 0x3A: case 0x3B: /* ADD/ADC/SUB/SBC (mem),# */
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: { /* AND/XOR/OR/CP (mem),# */
            const unsigned kind = op & 7;
            if (kind == 0) { if (size == 0) { ngp_write(mem, add_b(ngp_read(mem), fetch8())); cycles = 7; } else { ngp_write16(mem, add_w(ngp_read16(mem), fetch16())); cycles = 8; } }
            else if (kind == 1) { if (size == 0) { ngp_write(mem, adc_b(ngp_read(mem), fetch8())); cycles = 7; } else { ngp_write16(mem, adc_w(ngp_read16(mem), fetch16())); cycles = 8; } }
            else if (kind == 2) { if (size == 0) { ngp_write(mem, sub_b(ngp_read(mem), fetch8())); cycles = 7; } else { ngp_write16(mem, sub_w(ngp_read16(mem), fetch16())); cycles = 8; } }
            else if (kind == 3) { if (size == 0) { ngp_write(mem, sbc_b(ngp_read(mem), fetch8())); cycles = 7; } else { ngp_write16(mem, sbc_w(ngp_read16(mem), fetch16())); cycles = 8; } }
            else if (kind == 4) {
                if (size == 0) { const uint8_t r = ngp_read(mem) & fetch8(); ngp_write(mem, r); logic_flags(r, 0); }
                else { const uint16_t r = ngp_read16(mem) & fetch16(); ngp_write16(mem, r); logic_flags(r, 1); }
                set_h(1); set_n(0); set_c(0);
                cycles = size == 0 ? 7 : 8;
            }
            else if (kind == 5) {
                if (size == 0) { const uint8_t r = ngp_read(mem) ^ fetch8(); ngp_write(mem, r); logic_flags(r, 0); }
                else { const uint16_t r = ngp_read16(mem) ^ fetch16(); ngp_write16(mem, r); logic_flags(r, 1); }
                set_h(0); set_n(0); set_c(0);
                cycles = size == 0 ? 7 : 8;
            }
            else if (kind == 6) {
                if (size == 0) { const uint8_t r = ngp_read(mem) | fetch8(); ngp_write(mem, r); logic_flags(r, 0); }
                else { const uint16_t r = ngp_read16(mem) | fetch16(); ngp_write16(mem, r); logic_flags(r, 1); }
                set_h(0); set_n(0); set_c(0);
                cycles = size == 0 ? 7 : 8;
            }
            else { if (size == 0) { sub_b(ngp_read(mem), fetch8()); cycles = 6; } else { sub_w(ngp_read16(mem), fetch16()); cycles = 6; } }
            return;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: /* MUL RR,(mem) */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: { /* MULS RR,(mem) */
            const uint8_t t = rr_target(1);
            const int sign = (op & 0x08) != 0;
            if (t == 0x80 || size == 2) { machine.cpu.fault = 1; cycles = 2; return; }
            if (size == 0) {
                const uint16_t a = ngp_code_w(t) & 0xFF, b = ngp_read(mem);
                ngp_code_w_set(t, sign ? (uint16_t)((int16_t)(int8_t)a * (int8_t)b) : (uint16_t)(a * b));
                cycles = 18;
            } else {
                const uint32_t a = ngp_code_l(t) & 0xFFFF, b = ngp_read16(mem);
                ngp_code_l_set(t, sign ? (uint32_t)((int32_t)(int16_t)a * (int16_t)b) : a * b);
                cycles = 26;
            }
            return;
        }
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: /* DIV RR,(mem) */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: { /* DIVS RR,(mem) */
            const uint8_t t = rr_target(1);
            const int sign = (op & 0x08) != 0;
            if (t == 0x80 || size == 2) { machine.cpu.fault = 1; cycles = 2; return; }
            if (size == 0) { ngp_code_w_set(t, sign ? divs_b((int16_t)ngp_code_w(t), (int8_t)ngp_read(mem)) : div_b(ngp_code_w(t), ngp_read(mem))); cycles = sign ? 24 : 22; }
            else { ngp_code_l_set(t, sign ? divs_w((int32_t)ngp_code_l(t), (int16_t)ngp_read16(mem)) : div_w(ngp_code_l(t), ngp_read16(mem))); cycles = sign ? 32 : 30; }
            return;
        }
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: case 0x67: /* INC #3,(mem) */
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: { /* DEC #3,(mem) */
            uint8_t val = R;
            const int dec = (op & 0x08) != 0;
            if (!val) val = 8;
            if (size == 0) {
                const uint8_t d = ngp_read(mem);
                const uint8_t half = dec ? (uint8_t)((d & 0xF) - val) : (uint8_t)((d & 0xF) + val);
                const uint8_t r = dec ? (uint8_t)(d - val) : (uint8_t)(d + val);
                set_z(r == 0); set_h(half > 0xF); set_s(r & 0x80); set_n(dec);
                set_v(dec ? ((d & 0x80) && !(r & 0x80)) : (!(d & 0x80) && (r & 0x80)));
                ngp_write(mem, r);
            } else {
                const uint16_t d = ngp_read16(mem);
                const uint8_t half = dec ? (uint8_t)((d & 0xF) - val) : (uint8_t)((d & 0xF) + val);
                const uint16_t r = dec ? (uint16_t)(d - val) : (uint16_t)(d + val);
                set_z(r == 0); set_h(half > 0xF); set_s(r & 0x8000); set_n(dec);
                set_v(dec ? ((d & 0x8000) && !(r & 0x8000)) : (!(d & 0x8000) && (r & 0x8000)));
                ngp_write16(mem, r);
            }
            cycles = 6;
            return;
        }
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: /* shifts on memory */
            shift_mem(op & 7);
            cycles = 8;
            return;
        case 0x80: case 0x81: case 0x82: case 0x83: /* ADD R,(mem) */
        case 0x84: case 0x85: case 0x86: case 0x87:
            if (size == 0) { reg_b_set(R, add_b(reg_b(R), ngp_read(mem))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, add_w(reg_w(R), ngp_read16(mem))); cycles = 4; }
            else { reg_l_set(R, add_l(reg_l(R), ngp_read32(mem))); cycles = 6; }
            return;
        case 0x88: case 0x89: case 0x8A: case 0x8B: /* ADD (mem),R */
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            if (size == 0) { ngp_write(mem, add_b(ngp_read(mem), reg_b(R))); cycles = 6; }
            else if (size == 1) { ngp_write16(mem, add_w(ngp_read16(mem), reg_w(R))); cycles = 6; }
            else { ngp_write32(mem, add_l(ngp_read32(mem), reg_l(R))); cycles = 10; }
            return;
        case 0x90: case 0x91: case 0x92: case 0x93: /* ADC R,(mem) */
        case 0x94: case 0x95: case 0x96: case 0x97:
            if (size == 0) { reg_b_set(R, adc_b(reg_b(R), ngp_read(mem))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, adc_w(reg_w(R), ngp_read16(mem))); cycles = 4; }
            else { reg_l_set(R, adc_l(reg_l(R), ngp_read32(mem))); cycles = 6; }
            return;
        case 0x98: case 0x99: case 0x9A: case 0x9B: /* ADC (mem),R */
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            if (size == 0) { ngp_write(mem, adc_b(ngp_read(mem), reg_b(R))); cycles = 6; }
            else if (size == 1) { ngp_write16(mem, adc_w(ngp_read16(mem), reg_w(R))); cycles = 6; }
            else { ngp_write32(mem, adc_l(ngp_read32(mem), reg_l(R))); cycles = 10; }
            return;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: /* SUB R,(mem) */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
            if (size == 0) { reg_b_set(R, sub_b(reg_b(R), ngp_read(mem))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, sub_w(reg_w(R), ngp_read16(mem))); cycles = 4; }
            else { reg_l_set(R, sub_l(reg_l(R), ngp_read32(mem))); cycles = 6; }
            return;
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: /* SUB (mem),R */
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            if (size == 0) { ngp_write(mem, sub_b(ngp_read(mem), reg_b(R))); cycles = 6; }
            else if (size == 1) { ngp_write16(mem, sub_w(ngp_read16(mem), reg_w(R))); cycles = 6; }
            else { ngp_write32(mem, sub_l(ngp_read32(mem), reg_l(R))); cycles = 10; }
            return;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: /* AND R,(mem) */
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
            if (size == 0) { const uint8_t r = reg_b(R) & ngp_read(mem); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) & ngp_read16(mem); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) & ngp_read32(mem); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(1); set_n(0); set_c(0);
            cycles = size == 2 ? 6 : 4;
            return;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: /* AND (mem),R */
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            if (size == 0) { const uint8_t r = ngp_read(mem) & reg_b(R); ngp_write(mem, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = ngp_read16(mem) & reg_w(R); ngp_write16(mem, r); logic_flags(r, 1); }
            else { const uint32_t r = ngp_read32(mem) & reg_l(R); ngp_write32(mem, r); logic_flags(r, 2); }
            set_h(1); set_n(0); set_c(0);
            cycles = size == 2 ? 10 : 6;
            return;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: /* XOR R,(mem) */
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            if (size == 0) { const uint8_t r = reg_b(R) ^ ngp_read(mem); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) ^ ngp_read16(mem); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) ^ ngp_read32(mem); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 6 : 4;
            return;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: /* XOR (mem),R */
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            if (size == 0) { const uint8_t r = ngp_read(mem) ^ reg_b(R); ngp_write(mem, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = ngp_read16(mem) ^ reg_w(R); ngp_write16(mem, r); logic_flags(r, 1); }
            else { const uint32_t r = ngp_read32(mem) ^ reg_l(R); ngp_write32(mem, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 10 : 6;
            return;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: /* OR R,(mem) */
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            if (size == 0) { const uint8_t r = reg_b(R) | ngp_read(mem); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) | ngp_read16(mem); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) | ngp_read32(mem); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 6 : 4;
            return;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: /* OR (mem),R */
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            if (size == 0) { const uint8_t r = ngp_read(mem) | reg_b(R); ngp_write(mem, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = ngp_read16(mem) | reg_w(R); ngp_write16(mem, r); logic_flags(r, 1); }
            else { const uint32_t r = ngp_read32(mem) | reg_l(R); ngp_write32(mem, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 10 : 6;
            return;
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: /* CP R,(mem) */
        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
            if (size == 0) { sub_b(reg_b(R), ngp_read(mem)); cycles = 4; }
            else if (size == 1) { sub_w(reg_w(R), ngp_read16(mem)); cycles = 4; }
            else { sub_l(reg_l(R), ngp_read32(mem)); cycles = 6; }
            return;
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: /* CP (mem),R */
        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            if (size == 0) { sub_b(ngp_read(mem), reg_b(R)); cycles = 6; }
            else if (size == 1) { sub_w(ngp_read16(mem), reg_w(R)); cycles = 6; }
            else { sub_l(ngp_read32(mem), reg_l(R)); cycles = 10; }
            return;
        default: /* 0x00-0x03, 0x05, 0x08-0x0F, 0x18-0x18, 0x1A-0x1F, 0x28-0x2F, 0x70-0x77 */
            machine.cpu.fault = 1;
            cycles = 2;
            return;
    }
}

/* ---- the dst group (0xB0-0xBF, 0xF0-0xF5) ---------------------------------------- */

static void dst_ops(uint8_t op) {
    R = op & 7;
    switch (op) {
        case 0x00: ngp_write(mem, fetch8()); cycles = 5; return;    /* LD (mem),#8 */
        case 0x02: ngp_write16(mem, fetch16()); cycles = 6; return; /* LD (mem),#16 */
        case 0x04: ngp_write(mem, pop8()); cycles = 6; return;      /* POP (mem) */
        case 0x06: ngp_write16(mem, pop16()); cycles = 6; return;
        case 0x14: ngp_write(mem, ngp_read(fetch16())); cycles = 8; return;    /* LD (mem),(nn) */
        case 0x16: ngp_write16(mem, ngp_read16(fetch16())); cycles = 8; return;
        case 0x20: case 0x21: case 0x22: case 0x23: /* LDA r,(mem) word */
        case 0x24: case 0x25: case 0x26: case 0x27:
            reg_w_set(R, (uint16_t)mem); cycles = 4; return;
        case 0x28: { /* ANDCF A,(mem) */
            const uint8_t bit = REGA & 0xF, mb = (ngp_read(mem) >> bit) & 1;
            if (bit < 8) set_c(mb & GET_C);
            cycles = 8; return;
        }
        case 0x29: {
            const uint8_t bit = REGA & 0xF, mb = (ngp_read(mem) >> bit) & 1;
            if (bit < 8) set_c(mb | GET_C);
            cycles = 8; return;
        }
        case 0x2A: {
            const uint8_t bit = REGA & 0xF, mb = (ngp_read(mem) >> bit) & 1;
            if (bit < 8) set_c(mb ^ GET_C);
            cycles = 8; return;
        }
        case 0x2B: {
            const uint8_t bit = REGA & 0xF;
            if (bit < 8) set_c(ngp_read(mem) & (1 << bit));
            cycles = 8; return;
        }
        case 0x2C: {
            const uint8_t bit = REGA & 0xF;
            if (bit < 8) ngp_write(mem, (uint8_t)((ngp_read(mem) & ~(1 << bit)) | (GET_C << bit)));
            cycles = 8; return;
        }
        case 0x30: case 0x31: case 0x32: case 0x33: /* LDA r,(mem) long */
        case 0x34: case 0x35: case 0x36: case 0x37:
            reg_l_set(R, mem); cycles = 4; return;
        case 0x40: case 0x41: case 0x42: case 0x43: /* LD (mem),r byte */
        case 0x44: case 0x45: case 0x46: case 0x47:
            ngp_write(mem, reg_b(R)); cycles = 4; return;
        case 0x50: case 0x51: case 0x52: case 0x53: /* LD (mem),r word */
        case 0x54: case 0x55: case 0x56: case 0x57:
            ngp_write16(mem, reg_w(R)); cycles = 4; return;
        case 0x60: case 0x61: case 0x62: case 0x63: /* LD (mem),r long */
        case 0x64: case 0x65: case 0x66: case 0x67:
            ngp_write32(mem, reg_l(R)); cycles = 6; return;
        case 0x80: { const uint8_t mb = (ngp_read(mem) >> R) & 1; set_c(mb & GET_C); cycles = 8; return; } /* ANDCF #,(mem) */
        case 0x88: { const uint8_t mb = (ngp_read(mem) >> R) & 1; set_c(mb | GET_C); cycles = 8; return; } /* ORCF */
        case 0x90: { const uint8_t mb = (ngp_read(mem) >> R) & 1; set_c(mb ^ GET_C); cycles = 8; return; } /* XORCF */
        case 0x98: { set_c(ngp_read(mem) & (1 << R)); cycles = 8; return; }                                 /* LDCF */
        case 0xA0: { ngp_write(mem, (uint8_t)((ngp_read(mem) & ~(1 << R)) | (GET_C << R))); cycles = 8; return; } /* STCF */
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: /* TSET #,(mem) */
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            set_z(!(ngp_read(mem) & (1 << R)));
            ngp_write(mem, (uint8_t)(ngp_read(mem) | (1 << R)));
            set_h(1); set_n(0); cycles = 10; return;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: /* RES #,(mem) */
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            ngp_write(mem, (uint8_t)(ngp_read(mem) & ~(1 << R))); cycles = 8; return;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: /* SET #,(mem) */
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            ngp_write(mem, (uint8_t)(ngp_read(mem) | (1 << R))); cycles = 8; return;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: /* CHG #,(mem) */
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
            ngp_write(mem, (uint8_t)(ngp_read(mem) ^ (1 << R))); cycles = 8; return;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: /* BIT #,(mem) */
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            set_z(!(ngp_read(mem) & (1 << R))); set_h(1); set_n(0); cycles = 8; return;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: /* JP cc,(mem) */
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            cycles = 6;
            if (condition(op & 0xF)) { machine.cpu.pc = mem; cycles += 3; }
            return;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: /* CALL cc,(mem) */
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            cycles = 6;
            if (condition(op & 0xF)) { push32(machine.cpu.pc); machine.cpu.pc = mem; cycles += 6; }
            return;
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: /* RET cc */
        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            cycles = 6;
            if (condition(op & 0xF)) { machine.cpu.pc = pop32(); cycles += 6; }
            return;
        default: /* 0x01, 0x03, 0x05, 0x07, 0x10-0x13, 0x15, 0x17-0x1F, 0x38-0x3F, 0x48-0x4F, 0x58-0x5F, 0x68-0x7F */
            machine.cpu.fault = 1;
            cycles = 2;
            return;
    }
}

/* ---- the reg group (0xC7-0xCF, 0xD7-0xDF, 0xE7-0xEF) ------------------------------ */

static void ldc_cr(uint8_t cr, int store) {
    /* LDC cr,r / LDC r,cr: the CPU's control registers are the micro-DMA
     * channels' S (0x00+4c), D (0x10+4c), C (0x20+2c) and M (0x22+4c) */
    if (store) {
        if ((cr & 0xE7) == 0x22 && cr >= 0x22 && cr <= 0x2E && ((cr - 0x22) & 3) == 0)
            machine.dma_m[(cr - 0x22) >> 2] = ngp_code_b(rCode);
        else if ((cr & 0xE7) == 0x20 && cr >= 0x20 && cr <= 0x2C && ((cr - 0x20) & 3) == 0)
            machine.dma_c[(cr - 0x20) >> 2] = (uint16_t)ngp_code_w(rCode);
        else if (cr < 0x10 && (cr & 3) == 0) machine.dma_s[cr >> 2] = ngp_code_l(rCode);
        else if (cr >= 0x10 && cr < 0x20 && (cr & 3) == 0) machine.dma_d[(cr - 0x10) >> 2] = ngp_code_l(rCode);
    } else {
        if ((cr & 0xE7) == 0x22 && cr >= 0x22 && cr <= 0x2E && ((cr - 0x22) & 3) == 0)
            ngp_code_b_set(rCode, machine.dma_m[(cr - 0x22) >> 2]);
        else if ((cr & 0xE7) == 0x20 && cr >= 0x20 && cr <= 0x2C && ((cr - 0x20) & 3) == 0)
            ngp_code_w_set(rCode, machine.dma_c[(cr - 0x20) >> 2]);
        else if (cr < 0x10 && (cr & 3) == 0) ngp_code_l_set(rCode, machine.dma_s[cr >> 2]);
        else if (cr >= 0x10 && cr < 0x20 && (cr & 3) == 0) ngp_code_l_set(rCode, machine.dma_d[(cr - 0x10) >> 2]);
        else ngp_code_l_set(rCode, 0);
    }
}

static void reg_ops(uint8_t op) {
    R = op & 7;
    if (!brCode) {
        /* the register operand is the first byte's low bits: bytes name
         * A W B C D E L H in that nibble order, words/longs the registers */
        static const uint8_t conv_b[8] = { 0xE1, 0xE0, 0xE5, 0xE4, 0xE9, 0xE8, 0xED, 0xEC };
        static const uint8_t conv_w[8] = { 0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC };
        brCode = 1;
        rCode = size == 0 ? conv_b[first & 7] : conv_w[first & 7];
    }
    switch (op) {
        case 0x03: /* LD r,# */
            if (size == 0) { ngp_code_b_set(rCode, fetch8()); cycles = 4; }
            else if (size == 1) { ngp_code_w_set(rCode, fetch16()); cycles = 4; }
            else { ngp_code_l_set(rCode, fetch32()); cycles = 6; }
            return;
        case 0x04: /* PUSH r */
            if (size == 0) { push8(ngp_code_b(rCode)); cycles = 5; }
            else if (size == 1) { push16(ngp_code_w(rCode)); cycles = 5; }
            else { push32(ngp_code_l(rCode)); cycles = 7; }
            return;
        case 0x05: /* POP r */
            if (size == 0) { ngp_code_b_set(rCode, pop8()); cycles = 6; }
            else if (size == 1) { ngp_code_w_set(rCode, pop16()); cycles = 6; }
            else { ngp_code_l_set(rCode, pop32()); cycles = 8; }
            return;
        case 0x06: /* CPL */
            if (size == 0) ngp_code_b_set(rCode, (uint8_t)~ngp_code_b(rCode));
            else if (size == 1) ngp_code_w_set(rCode, (uint16_t)~ngp_code_w(rCode));
            set_h(1); set_n(1); cycles = 4;
            return;
        case 0x07: /* NEG */
            if (size == 0) ngp_code_b_set(rCode, sub_b(0, ngp_code_b(rCode)));
            else if (size == 1) ngp_code_w_set(rCode, sub_w(0, ngp_code_w(rCode)));
            cycles = 5;
            return;
        case 0x08: /* MUL r,# */
        case 0x09: { /* MULS r,# */
            const uint8_t t = rr_target(0);
            const int sign = (op & 1) != 0;
            if (size == 0) {
                const uint16_t a = ngp_code_w(t) & 0xFF, b = fetch8();
                ngp_code_w_set(t, sign ? (uint16_t)((int16_t)(int8_t)a * (int8_t)b) : (uint16_t)(a * b));
                cycles = 18;
            } else {
                const uint32_t a = ngp_code_l(t) & 0xFFFF, b = fetch16();
                ngp_code_l_set(t, sign ? (uint32_t)((int32_t)(int16_t)a * (int16_t)b) : a * b);
                cycles = 26;
            }
            return;
        }
        case 0x0A: /* DIV r,# */
        case 0x0B: { /* DIVS r,# */
            const uint8_t t = rr_target(0);
            const int sign = (op & 1) != 0;
            if (size == 0) { ngp_code_w_set(t, sign ? divs_b((int16_t)ngp_code_w(t), (int8_t)fetch8()) : div_b(ngp_code_w(t), fetch8())); cycles = sign ? 24 : 22; }
            else { ngp_code_l_set(t, sign ? divs_w((int32_t)ngp_code_l(t), (int16_t)fetch16()) : div_w(ngp_code_l(t), fetch16())); cycles = sign ? 32 : 30; }
            return;
        }
        case 0x0C: { /* LINK r,dd */
            const int16_t d = (int16_t)fetch16();
            push32(ngp_code_l(rCode));
            ngp_code_l_set(rCode, REGXSP);
            REGXSP += d;
            cycles = 10;
            return;
        }
        case 0x0D: /* UNLK r */
            REGXSP = ngp_code_l(rCode);
            ngp_code_l_set(rCode, pop32());
            cycles = 8;
            return;
        case 0x0E: { /* BS1F A,r */
            const uint16_t data = ngp_code_w(rCode);
            set_v(0);
            for (unsigned i = 0; i < 16; i++)
                if (data & (1u << i)) { reg_b_set(1, (uint8_t)i); cycles = 4; return; }
            set_v(1); cycles = 4;
            return;
        }
        case 0x0F: { /* BS1B A,r */
            const uint16_t data = ngp_code_w(rCode);
            set_v(0);
            for (int i = 15; i >= 0; i--)
                if (data & (1u << i)) { reg_b_set(1, (uint8_t)i); cycles = 4; return; }
            set_v(1); cycles = 4;
            return;
        }
        case 0x10: /* DAA */
            if (size == 0) {
                const uint8_t src = ngp_code_b(rCode);
                uint8_t added = 0;
                int setC = 0;
                const uint8_t up = src & 0xF0, lo = src & 0x0F;
                if (GET_C) {
                    setC = 1;
                    added = GET_H ? 0x66 : (lo < 0x0A ? 0x60 : 0x66);
                } else if (GET_H) {
                    added = src < 0x9A ? 0x06 : 0x66;
                } else if ((up < 0x90) && (lo > 0x9)) added = 0x06;
                else if ((up > 0x80) && (lo > 0x9)) added = 0x66;
                else if ((up > 0x90) && (lo < 0xA)) added = 0x60;
                const uint16_t rc = GET_N ? (uint16_t)(src - added) : (uint16_t)(src + added);
                const uint8_t r = (uint8_t)rc;
                set_s(r & 0x80); set_z(r == 0);
                set_h(((src & 0xF) + (GET_N ? -(added & 0xF) : (added & 0xF))) > 0xF);
                if (GET_N) set_c(r > src || setC);
                else set_c(r < src || setC);
                parity_b(r);
                ngp_code_b_set(rCode, r);
            }
            cycles = 6;
            return;
        case 0x12: /* EXTZ */
            if (size == 1) ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) & 0xFF));
            else if (size == 2) ngp_code_l_set(rCode, ngp_code_l(rCode) & 0xFFFF);
            cycles = 4;
            return;
        case 0x13: /* EXTS */
            if (size == 1) ngp_code_w_set(rCode, (uint16_t)((ngp_code_w(rCode) & 0x80) ? ngp_code_w(rCode) | 0xFF00 : ngp_code_w(rCode) & 0x00FF));
            else if (size == 2) ngp_code_l_set(rCode, (ngp_code_l(rCode) & 0x8000) ? ngp_code_l(rCode) | 0xFFFF0000u : ngp_code_l(rCode) & 0xFFFF);
            cycles = 5;
            return;
        case 0x14: /* PAA r */
            if (size == 1) { if (ngp_code_w(rCode) & 1) ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) + 1)); }
            else if (size == 2) { if (ngp_code_l(rCode) & 1) ngp_code_l_set(rCode, ngp_code_l(rCode) + 1); }
            cycles = 4;
            return;
        case 0x16: { /* MIRR r */
            const uint16_t src = ngp_code_w(rCode);
            uint16_t dst = 0;
            for (unsigned b = 0; b < 16; b++)
                if (src & (1u << b)) dst |= (uint16_t)(1u << (15 - b));
            ngp_code_w_set(rCode, dst);
            cycles = 4;
            return;
        }
        case 0x19: { /* MULA rr: rr += XDE * XHL (signed) */
            const uint32_t src = (uint32_t)((int32_t)(int16_t)ngp_read16(reg_l(2)) * (int16_t)ngp_read16(reg_l(3)));
            const uint32_t dst = ngp_code_l(rCode), r = dst + src;
            set_s(r >> 31); set_z(r == 0);
            set_v(((dst >> 31) == (src >> 31)) && ((r >> 31) != (dst >> 31)));
            set_n(0); set_c(0);
            ngp_code_l_set(rCode, r);
            cycles = 31;
            return;
        }
        case 0x1C: { /* DJNZ r,d */
            const int8_t off = (int8_t)fetch8();
            cycles = 7;
            if (size == 0) {
                const uint8_t v = (uint8_t)(ngp_code_b(rCode) - 1);
                ngp_code_b_set(rCode, v);
                if (v) { cycles = 11; machine.cpu.pc += off; }
            } else if (size == 1) {
                const uint16_t v = (uint16_t)(ngp_code_w(rCode) - 1);
                ngp_code_w_set(rCode, v);
                if (v) { cycles = 11; machine.cpu.pc += off; }
            }
            return;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: { /* ANDCF..STCF #,r */
            const uint8_t bit = fetch8() & 0xF;
            if (op == 0x20) { if (size == 0) { if (bit < 8) set_c(GET_C & ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C & ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x21) { if (size == 0) { if (bit < 8) set_c(GET_C | ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C | ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x22) { if (size == 0) { if (bit < 8) set_c(GET_C ^ ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C ^ ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x23) { if (size == 0) { if (bit < 8) set_c(ngp_code_b(rCode) & (1 << bit)); } else set_c(ngp_code_w(rCode) & (1 << bit)); }
            else {
                if (size == 0) { if (bit < 8) ngp_code_b_set(rCode, (uint8_t)((ngp_code_b(rCode) & ~(1 << bit)) | (GET_C << bit))); }
                else ngp_code_w_set(rCode, (uint16_t)((ngp_code_w(rCode) & ~(1 << bit)) | (GET_C << bit)));
            }
            cycles = 4;
            return;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: { /* the A forms */
            const uint8_t bit = REGA & 0xF;
            if (op == 0x28) { if (size == 0) { if (bit < 8) set_c(GET_C & ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C & ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x29) { if (size == 0) { if (bit < 8) set_c(GET_C | ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C | ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x2A) { if (size == 0) { if (bit < 8) set_c(GET_C ^ ((ngp_code_b(rCode) >> bit) & 1)); } else set_c(GET_C ^ ((ngp_code_w(rCode) >> bit) & 1)); }
            else if (op == 0x2B) { if (size == 0) { if (bit < 8) set_c(ngp_code_b(rCode) & (1 << bit)); } else set_c(ngp_code_w(rCode) & (1 << bit)); }
            else {
                if (size == 0) { if (bit < 8) ngp_code_b_set(rCode, (uint8_t)((ngp_code_b(rCode) & ~(1 << bit)) | (GET_C << bit))); }
                else ngp_code_w_set(rCode, (uint16_t)((ngp_code_w(rCode) & ~(1 << bit)) | (GET_C << bit)));
            }
            cycles = 4;
            return;
        }
        case 0x2E: ldc_cr(fetch8(), 1); cycles = 8; return; /* LDC cr,r */
        case 0x2F: ldc_cr(fetch8(), 0); cycles = 8; return; /* LDC r,cr */
        case 0x30: { /* RES #,r: the bit byte is fetched whatever the size */
            const uint8_t bit = fetch8() & 0xF;
            if (size == 0) ngp_code_b_set(rCode, (uint8_t)(ngp_code_b(rCode) & ~(1u << bit)));
            else if (size == 1) ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) & ~(1u << bit)));
            cycles = 4;
            return;
        }
        case 0x31: { /* SET #,r */
            const uint8_t bit = fetch8() & 0xF;
            if (size == 0) ngp_code_b_set(rCode, (uint8_t)(ngp_code_b(rCode) | (1 << bit)));
            else if (size == 1) ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) | (1 << bit)));
            cycles = 4; return;
        }
        case 0x32: { /* CHG #,r */
            const uint8_t bit = fetch8() & 0xF;
            if (size == 0) ngp_code_b_set(rCode, (uint8_t)(ngp_code_b(rCode) ^ (1 << bit)));
            else if (size == 1) ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) ^ (1 << bit)));
            cycles = 4; return;
        }
        case 0x33: { /* BIT #,r */
            const uint8_t bit = fetch8() & 0xF;
            set_h(1); set_n(0);
            if (size == 0) set_z(!(ngp_code_b(rCode) & (1 << bit)));
            else set_z(!(ngp_code_w(rCode) & (1 << bit)));
            cycles = 4; return;
        }
        case 0x34: { /* TSET #,r */
            const uint8_t bit = fetch8() & 0xF;
            if (size == 0) { set_z(!(ngp_code_b(rCode) & (1 << bit))); ngp_code_b_set(rCode, (uint8_t)(ngp_code_b(rCode) | (1 << bit))); }
            else if (size == 1) { set_z(!(ngp_code_w(rCode) & (1 << bit))); ngp_code_w_set(rCode, (uint16_t)(ngp_code_w(rCode) | (1 << bit))); }
            set_h(1); set_n(0); cycles = 6; return;
        }
        case 0x38: case 0x39: case 0x3A: { /* MINC 1/2/4 #,r */
            const uint16_t n = (uint16_t)(fetch16() + (op == 0x38 ? 1 : op == 0x39 ? 2 : 4));
            if (size == 1 && n) {
                const uint16_t v = ngp_code_w(rCode);
                ngp_code_w_set(rCode, (uint16_t)((v % n) == n - (op == 0x38 ? 1 : op == 0x39 ? 2 : 4) ? v - (n - (op == 0x38 ? 1 : op == 0x39 ? 2 : 4)) : v + (op == 0x38 ? 1 : op == 0x39 ? 2 : 4)));
            }
            cycles = 8; return;
        }
        case 0x3C: case 0x3D: case 0x3E: { /* MDEC 1/2/4 #,r */
            const uint16_t n = (uint16_t)(fetch16() + (op == 0x3C ? 1 : op == 0x3D ? 2 : 4));
            if (size == 1 && n) {
                const uint16_t v = ngp_code_w(rCode);
                ngp_code_w_set(rCode, (uint16_t)((v % n) == 0 ? v + (n - (op == 0x3C ? 1 : op == 0x3D ? 2 : 4)) : v - (op == 0x3C ? 1 : op == 0x3D ? 2 : 4)));
            }
            cycles = 7; return;
        }
        case 0x60: case 0x61: case 0x62: case 0x63: /* INC #3,r */
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: /* DEC #3,r */
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            uint8_t val = R;
            const int dec = (op & 0x08) != 0;
            if (!val) val = 8;
            if (size == 0) {
                const uint8_t d = ngp_code_b(rCode);
                const uint8_t half = dec ? (uint8_t)((d & 0xF) - val) : (uint8_t)((d & 0xF) + val);
                const uint8_t r = dec ? (uint8_t)(d - val) : (uint8_t)(d + val);
                set_s(r & 0x80); set_h(half > 0xF); set_z(r == 0); set_n(dec);
                set_v(dec ? ((d & 0x80) && !(r & 0x80)) : (!(d & 0x80) && (r & 0x80)));
                ngp_code_b_set(rCode, r);
                cycles = 4;
            } else if (size == 1) {
                ngp_code_w_set(rCode, dec ? (uint16_t)(ngp_code_w(rCode) - val) : (uint16_t)(ngp_code_w(rCode) + val));
                cycles = 4;
            } else {
                ngp_code_l_set(rCode, dec ? ngp_code_l(rCode) - val : ngp_code_l(rCode) + val);
                cycles = dec ? 5 : 7;
            }
            return;
        }
        case 0x70: case 0x71: case 0x72: case 0x73: /* SETCC cc,r */
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            if (size == 0) ngp_code_b_set(rCode, (uint8_t)(condition(op & 0xF) ? 1 : 0));
            else if (size == 1) ngp_code_w_set(rCode, (uint16_t)(condition(op & 0xF) ? 1 : 0));
            cycles = 6;
            return;
        case 0x80: case 0x81: case 0x82: case 0x83: /* ADD R,r */
        case 0x84: case 0x85: case 0x86: case 0x87:
            if (size == 0) { reg_b_set(R, add_b(reg_b(R), ngp_code_b(rCode))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, add_w(reg_w(R), ngp_code_w(rCode))); cycles = 4; }
            else { reg_l_set(R, add_l(reg_l(R), ngp_code_l(rCode))); cycles = 7; }
            return;
        case 0x88: case 0x89: case 0x8A: case 0x8B: /* LD R,r */
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            if (size == 0) { reg_b_set(R, ngp_code_b(rCode)); cycles = 4; }
            else if (size == 1) { reg_w_set(R, ngp_code_w(rCode)); cycles = 4; }
            else { reg_l_set(R, ngp_code_l(rCode)); cycles = 4; }
            return;
        case 0x90: case 0x91: case 0x92: case 0x93: /* ADC R,r */
        case 0x94: case 0x95: case 0x96: case 0x97:
            if (size == 0) { reg_b_set(R, adc_b(reg_b(R), ngp_code_b(rCode))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, adc_w(reg_w(R), ngp_code_w(rCode))); cycles = 4; }
            else { reg_l_set(R, adc_l(reg_l(R), ngp_code_l(rCode))); cycles = 7; }
            return;
        case 0x98: case 0x99: case 0x9A: case 0x9B: /* LD r,R */
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            if (size == 0) { ngp_code_b_set(rCode, reg_b(R)); cycles = 4; }
            else if (size == 1) { ngp_code_w_set(rCode, reg_w(R)); cycles = 4; }
            else { ngp_code_l_set(rCode, reg_l(R)); cycles = 4; }
            return;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: /* SUB R,r */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
            if (size == 0) { reg_b_set(R, sub_b(reg_b(R), ngp_code_b(rCode))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, sub_w(reg_w(R), ngp_code_w(rCode))); cycles = 4; }
            else { reg_l_set(R, sub_l(reg_l(R), ngp_code_l(rCode))); cycles = 7; }
            return;
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: /* LD r,#short */
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            if (size == 0) { ngp_code_b_set(rCode, R); cycles = 4; }
            else if (size == 1) { ngp_code_w_set(rCode, R); cycles = 4; }
            else { ngp_code_l_set(rCode, R); cycles = 4; }
            return;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: /* SBC R,r */
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            if (size == 0) { reg_b_set(R, sbc_b(reg_b(R), ngp_code_b(rCode))); cycles = 4; }
            else if (size == 1) { reg_w_set(R, sbc_w(reg_w(R), ngp_code_w(rCode))); cycles = 4; }
            else { reg_l_set(R, sbc_l(reg_l(R), ngp_code_l(rCode))); cycles = 7; }
            return;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: /* EX R,r */
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (size == 0) { const uint8_t t = reg_b(R); reg_b_set(R, ngp_code_b(rCode)); ngp_code_b_set(rCode, t); }
            else if (size == 1) { const uint16_t t = reg_w(R); reg_w_set(R, ngp_code_w(rCode)); ngp_code_w_set(rCode, t); }
            else { const uint32_t t = reg_l(R); reg_l_set(R, ngp_code_l(rCode)); ngp_code_l_set(rCode, t); }
            cycles = 5;
            return;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: /* AND R,r */
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
            if (size == 0) { const uint8_t r = reg_b(R) & ngp_code_b(rCode); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) & ngp_code_w(rCode); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) & ngp_code_l(rCode); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(1); set_n(0); set_c(0);
            cycles = size == 2 ? 7 : 4;
            return;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: /* ADD/ADC/SUB/SBC r,# */
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: { /* AND/XOR/OR/CP r,# */
            const unsigned kind = op & 7;
            if (kind == 0 || kind == 1) {
                if (size == 0) { const uint8_t v = fetch8(); ngp_code_b_set(rCode, kind ? adc_b(ngp_code_b(rCode), v) : add_b(ngp_code_b(rCode), v)); }
                else if (size == 1) { const uint16_t v = fetch16(); ngp_code_w_set(rCode, kind ? adc_w(ngp_code_w(rCode), v) : add_w(ngp_code_w(rCode), v)); }
                else { const uint32_t v = fetch32(); ngp_code_l_set(rCode, kind ? adc_l(ngp_code_l(rCode), v) : add_l(ngp_code_l(rCode), v)); }
            } else if (kind == 2 || kind == 3) {
                if (size == 0) { const uint8_t v = fetch8(); ngp_code_b_set(rCode, kind == 3 ? sbc_b(ngp_code_b(rCode), v) : sub_b(ngp_code_b(rCode), v)); }
                else if (size == 1) { const uint16_t v = fetch16(); ngp_code_w_set(rCode, kind == 3 ? sbc_w(ngp_code_w(rCode), v) : sub_w(ngp_code_w(rCode), v)); }
                else { const uint32_t v = fetch32(); ngp_code_l_set(rCode, kind == 3 ? sbc_l(ngp_code_l(rCode), v) : sub_l(ngp_code_l(rCode), v)); }
            } else if (kind == 4) {
                if (size == 0) { const uint8_t r = ngp_code_b(rCode) & fetch8(); ngp_code_b_set(rCode, r); logic_flags(r, 0); }
                else if (size == 1) { const uint16_t r = ngp_code_w(rCode) & fetch16(); ngp_code_w_set(rCode, r); logic_flags(r, 1); }
                else { const uint32_t r = ngp_code_l(rCode) & fetch32(); ngp_code_l_set(rCode, r); logic_flags(r, 2); }
                set_h(1); set_n(0); set_c(0);
            } else if (kind == 5) {
                if (size == 0) { const uint8_t r = ngp_code_b(rCode) ^ fetch8(); ngp_code_b_set(rCode, r); logic_flags(r, 0); }
                else if (size == 1) { const uint16_t r = ngp_code_w(rCode) ^ fetch16(); ngp_code_w_set(rCode, r); logic_flags(r, 1); }
                else { const uint32_t r = ngp_code_l(rCode) ^ fetch32(); ngp_code_l_set(rCode, r); logic_flags(r, 2); }
                set_h(0); set_n(0); set_c(0);
            } else if (kind == 6) {
                if (size == 0) { const uint8_t r = ngp_code_b(rCode) | fetch8(); ngp_code_b_set(rCode, r); logic_flags(r, 0); }
                else if (size == 1) { const uint16_t r = ngp_code_w(rCode) | fetch16(); ngp_code_w_set(rCode, r); logic_flags(r, 1); }
                else { const uint32_t r = ngp_code_l(rCode) | fetch32(); ngp_code_l_set(rCode, r); logic_flags(r, 2); }
                set_h(0); set_n(0); set_c(0);
            } else {
                if (size == 0) sub_b(ngp_code_b(rCode), fetch8());
                else if (size == 1) sub_w(ngp_code_w(rCode), fetch16());
                else sub_l(ngp_code_l(rCode), fetch32());
            }
            cycles = size == 2 ? 7 : 4;
            return;
        }
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: /* XOR R,r */
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            if (size == 0) { const uint8_t r = reg_b(R) ^ ngp_code_b(rCode); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) ^ ngp_code_w(rCode); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) ^ ngp_code_l(rCode); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 7 : 4;
            return;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: /* CP r,#short */
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            if (size == 0) sub_b(ngp_code_b(rCode), R);
            else if (size == 1) sub_w(ngp_code_w(rCode), R);
            cycles = 4;
            return;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: /* OR R,r */
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            if (size == 0) { const uint8_t r = reg_b(R) | ngp_code_b(rCode); reg_b_set(R, r); logic_flags(r, 0); }
            else if (size == 1) { const uint16_t r = reg_w(R) | ngp_code_w(rCode); reg_w_set(R, r); logic_flags(r, 1); }
            else { const uint32_t r = reg_l(R) | ngp_code_l(rCode); reg_l_set(R, r); logic_flags(r, 2); }
            set_h(0); set_n(0); set_c(0);
            cycles = size == 2 ? 7 : 4;
            return;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: /* shift by count */
        case 0xEC: case 0xED: case 0xEE: case 0xEF: {
            unsigned sa = fetch8() & 0xF;
            if (!sa) sa = 16;
            shift_reg(op & 7, sa);
            cycles = 6 + 2 * (int32_t)sa;
            return;
        }
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: /* CP R,r */
        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
            if (size == 0) { sub_b(reg_b(R), ngp_code_b(rCode)); cycles = 4; }
            else if (size == 1) { sub_w(reg_w(R), ngp_code_w(rCode)); cycles = 4; }
            else { sub_l(reg_l(R), ngp_code_l(rCode)); cycles = 7; }
            return;
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: /* shift by A */
        case 0xFC: case 0xFD: case 0xFE: case 0xFF: {
            unsigned sa = REGA & 0xF;
            if (!sa) sa = 16;
            shift_reg(op & 7, sa);
            cycles = 6 + 2 * (int32_t)sa;
            return;
        }
        default: /* 0x00-0x02, 0x11, 0x15, 0x17, 0x18, 0x1A, 0x1B, 0x1D-0x1F, 0x25-0x27, 0x2D, 0x35-0x37, 0x3B, 0x3F */
            machine.cpu.fault = 1;
            cycles = 2;
            return;
    }
}

/* ---- the single-byte instructions and the primary dispatch ------------------------ */

/* the primary switch over the decode state the caller prepared (brCode,
 * rCode, cycles_extra, cycles, size, first reset; decode_addr run); the
 * interpreter's step and the recompiled code's one-shot entry share it */
static int32_t step_body(uint8_t f) {
    first = f;
    switch (first) {
        case 0x00: cycles = 2; break;                                    /* NOP */
        case 0x01: cycles = 4; break;                                    /* NORMAL */
        case 0x02: push16(machine.cpu.sr); cycles = 4; break;            /* PUSH SR */
        case 0x03: machine.cpu.sr = pop16(); ngp_check_pending(); cycles = 6; break; /* POP SR */
        case 0x04: cycles = 4; break;                                    /* MAX */
        case 0x05: machine.cpu.halted = 1; cycles = 8; break;            /* HALT */
        case 0x06: { /* EI #n (07 = DI) */
            const uint8_t n = fetch8();
            machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0x8FFF) | ((n & 7) << 12));
            ngp_check_pending();
            cycles = 5;
            break;
        }
        case 0x07: /* RETI */
            machine.cpu.sr = pop16();
            machine.cpu.pc = pop32();
            ngp_check_pending();
            cycles = 12;
            break;
        case 0x08: { const uint8_t d = fetch8(); ngp_write(d, fetch8()); cycles = 5; break; }
        case 0x09: push8(fetch8()); cycles = 4; break;                   /* PUSH #8 */
        case 0x0A: { const uint8_t d = fetch8(); ngp_write16(d, fetch16()); cycles = 6; break; }
        case 0x0B: push16(fetch16()); cycles = 5; break;                 /* PUSH #16 */
        case 0x0C: /* INCF */
            machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xF8FF) | ((((machine.cpu.sr >> 8) + 1) & 3) << 8));
            cycles = 2;
            break;
        case 0x0D: /* DECF */
            machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xF8FF) | ((((machine.cpu.sr >> 8) - 1) & 3) << 8));
            cycles = 2;
            break;
        case 0x0E: machine.cpu.pc = pop32(); cycles = 9; break;          /* RET */
        case 0x0F: { /* RETD d16 */
            const int16_t d = (int16_t)fetch16();
            machine.cpu.pc = pop32();
            REGXSP += d;
            cycles = 9;
            break;
        }
        case 0x10: set_n(0); set_v(0); set_c(0); cycles = 2; break;      /* RCF */
        case 0x11: set_h(0); set_n(0); set_c(1); cycles = 2; break;      /* SCF */
        case 0x12: set_n(0); set_c(!GET_C); cycles = 2; break;           /* CCF */
        case 0x13: set_n(0); set_c(!GET_Z); cycles = 2; break;           /* ZCF */
        case 0x14: push8(reg_b(1)); cycles = 3; break;                   /* PUSH A */
        case 0x15: reg_b_set(1, pop8()); cycles = 4; break;              /* POP A */
        case 0x16: { /* EX F,F' */
            const uint8_t f = (uint8_t)(machine.cpu.sr & 0xFF);
            machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xFF00) | machine.cpu.f_dash);
            machine.cpu.f_dash = f;
            cycles = 2;
            break;
        }
        case 0x17: /* LDF #n */
            machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xF8FF) | ((fetch8() & 3) << 8));
            cycles = 2;
            break;
        case 0x18: push8((uint8_t)machine.cpu.sr); cycles = 3; break;    /* PUSH F */
        case 0x19: machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xFF00) | pop8()); cycles = 4; break; /* POP F */
        case 0x1A: machine.cpu.pc = fetch16(); cycles = 7; break;        /* JP (nn16) */
        case 0x1B: machine.cpu.pc = fetch24(); cycles = 7; break;        /* JP (nn24) */
        case 0x1C: { const uint32_t t = fetch16(); push32(machine.cpu.pc); machine.cpu.pc = t; cycles = 12; break; } /* CALL */
        case 0x1D: { const uint32_t t = fetch24(); push32(machine.cpu.pc); machine.cpu.pc = t; cycles = 12; break; }
        case 0x1E: { /* CALR d16 */
            const int16_t d = (int16_t)fetch16();
            const uint32_t t = machine.cpu.pc + (uint32_t)d;
            push32(machine.cpu.pc);
            machine.cpu.pc = t;
            cycles = 12;
            break;
        }
        case 0x1F: /* the BIOS HLE hook: an undefined opcode on real silicon,
                    * the synthesised BIOS plants it at its entry points */
            if ((machine.cpu.pc & 0xFF0000) == 0xFF0000) {
                machine.cpu.pc--;
                ngp_bios_hle();
                cycles = 8;
            } else {
                machine.cpu.fault = 1;
                cycles = 2;
            }
            break;
        case 0x20: case 0x21: case 0x22: case 0x23: /* LD r,#8 */
        case 0x24: case 0x25: case 0x26: case 0x27:
            reg_b_set(first & 7, fetch8());
            cycles = 2;
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B: /* PUSH rr */
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            push16(reg_w(first & 7));
            cycles = 3;
            break;
        case 0x30: case 0x31: case 0x32: case 0x33: /* LD rr,#16 */
        case 0x34: case 0x35: case 0x36: case 0x37:
            reg_w_set(first & 7, fetch16());
            cycles = 3;
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B: /* PUSH xrr */
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            push32(reg_l(first & 7));
            cycles = 5;
            break;
        case 0x40: case 0x41: case 0x42: case 0x43: /* LD xrr,#32 */
        case 0x44: case 0x45: case 0x46: case 0x47:
            reg_l_set(first & 7, fetch32());
            cycles = 5;
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B: /* POP rr */
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            reg_w_set(first & 7, pop16());
            cycles = 4;
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: /* POP xrr */
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            reg_l_set(first & 7, pop32());
            cycles = 6;
            break;
        case 0x60: case 0x61: case 0x62: case 0x63: /* JR cc,d8 */
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            if (condition(first & 0xF)) {
                machine.cpu.pc += (int8_t)fetch8();
                cycles = 8;
            } else {
                fetch8();
                cycles = 4;
            }
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: /* JRL cc,d16 */
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            if (condition(first & 0xF)) {
                machine.cpu.pc += (int16_t)fetch16();
                cycles = 8;
            } else {
                fetch16();
                cycles = 4;
            }
            break;
        case 0x80: case 0x81: case 0x82: case 0x83: /* src, byte */
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            size = 0; second = fetch8(); src_ops(second);
            break;
        case 0x90: case 0x91: case 0x92: case 0x93: /* src, word */
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            size = 1; second = fetch8(); src_ops(second);
            break;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: /* src, long */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            size = 2; second = fetch8(); src_ops(second);
            break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: /* dst */
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            second = fetch8(); dst_ops(second);
            break;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: /* src, byte */
        case 0xC4: case 0xC5:
            size = 0; second = fetch8(); src_ops(second);
            break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: /* src, word */
        case 0xD4: case 0xD5:
            size = 1; second = fetch8(); src_ops(second);
            break;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: /* src, long */
        case 0xE4: case 0xE5:
            size = 2; second = fetch8(); src_ops(second);
            break;
        case 0xC6: case 0xD6: case 0xE6: machine.cpu.fault = 1; cycles = 2; break;
        case 0xC7: case 0xC8: case 0xC9: case 0xCA: /* reg, byte */
        case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            size = 0; second = fetch8(); reg_ops(second);
            break;
        case 0xD7: case 0xD8: case 0xD9: case 0xDA: /* reg, word */
        case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            size = 1; second = fetch8(); reg_ops(second);
            break;
        case 0xE7: case 0xE8: case 0xE9: case 0xEA: /* reg, long */
        case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
            size = 2; second = fetch8(); reg_ops(second);
            break;
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: /* dst, explicit modes */
        case 0xF4: case 0xF5:
            second = fetch8(); dst_ops(second);
            break;
        case 0xF6: machine.cpu.fault = 1; cycles = 2; break;
        case 0xF7: { /* LDX (mem8),(mem8) */
            uint8_t dst, src;
            fetch8();
            dst = fetch8();
            fetch8();
            src = fetch8();
            fetch8();
            ngp_write(dst, src);
            cycles = 9;
            break;
        }
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: /* SWI n */
        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            cycles = 16;
            switch (first & 7) {
                case 1: /* the system call: through the BIOS's table */
                    push32(machine.cpu.pc);
                    machine.cpu.pc = ngp_read32(0xFFFE00 + ((ngp_code_b(0x31) & 0x1F) << 2));
                    break;
                case 3: ngp_set_int(0, 1); break;
                case 4: ngp_set_int(1, 1); break;
                case 5: ngp_set_int(2, 1); break;
                case 6: ngp_set_int(3, 1); break;
                default: break;
            }
            break;
        default:
            machine.cpu.fault = 1;
            cycles = 2;
            break;
    }
    return cycles + cycles_extra;
}

int32_t ngp_step(void) {
    /* the recompiled entry at pc (one translated instruction per call; the
     * hint the previous entry seeded is checked first, so sequential flow
     * costs no hash search): the same cycles back, ticked the same way */
#if defined(NGP_NATIVE) && !defined(NGP_TRACE)
    if (!machine.cpu.halted) {
        const int e = ngp_gen_lookup(machine.cpu.pc, ngp_gen_hint);
        if (e >= 0) return ngp_gen_enter(e);
        ngp_gen_hint = -1;
    }
#endif
    brCode = 0;
    rCode = 0;
    cycles_extra = 0;
    cycles = 0;
    size = 0;
    /* where code may live: the work RAM, the cartridge windows and the BIOS;
     * a fetch elsewhere means the flow already went through garbage — the
     * harness reports the last sane pc (a recompiled pc never arrives here
     * wild: the dispatcher's lookup missed first) */
    {
        const uint32_t pc = machine.cpu.pc;
        const int ok = (pc >= 0x4000 && pc < 0x8000) || (pc >= 0x200000 && pc < 0x400000) ||
                       (pc >= 0x800000 && pc < 0xA00000) || (pc & 0xFF0000) == 0xFF0000;
        if (!ok) {
            machine.cpu.fault = 3;
            machine.last_pc = pc;
            machine.last_good_pc = ngp_last_good;
            machine.cpu.halted = 1;
            return 8;
        }
        ngp_last_good = pc;
    }
    {
        const uint32_t pc = machine.cpu.pc;
        first = fetch8();
        NGP_TRACE_HOOK(pc, first);
        decode_addr(first);
    }
    return step_body(first);
}

#ifdef NGP_NATIVE
/* the recompiled code's one instruction: the decode state handed in
 * precomputed (the emitter folded the instruction's bytes), the
 * interpreter's own handlers run — the same expressions, cycle for cycle.
 * ngp_gen_exec runs a two-byte group form (src_ops/dst_ops/reg_ops, the
 * (mem) address and its extra cycles given); ngp_gen_exec1 runs a
 * single-byte form the way ngp_step does, pc left just past the first
 * byte so the handler's own fetches read the right operands. */
int32_t ngp_gen_exec(uint8_t f, uint8_t s, int sz, uint8_t rc, uint8_t brc, uint32_t at, int extra) {
    brCode = brc;
    rCode = rc;
    cycles_extra = extra;
    cycles = 0;
    size = sz;
    first = f;
    second = s;
    R = 0;
    mem = at;
    if ((f >= 0x80 && f < 0xB0) || (f >= 0xC0 && f < 0xC6) || (f >= 0xD0 && f < 0xD6) ||
        (f >= 0xE0 && f < 0xE6))
        src_ops(s);
    else if ((f >= 0xB0 && f < 0xC0) || (f >= 0xF0 && f <= 0xF5))
        dst_ops(s);
    else
        reg_ops(s);
    return cycles + cycles_extra;
}

int32_t ngp_gen_exec1(uint8_t f) {
    brCode = 0;
    rCode = 0;
    cycles_extra = 0;
    cycles = 0;
    size = 0;
    first = f;
    decode_addr(f); /* a no-op for every single-byte opcode (f < 0xC0, or 0xF6/0xF7) */
    return step_body(f);
}
#endif

