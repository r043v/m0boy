/* 68000 semantics shared by the interpreter (md_full_cpu.c) and the C the
 * recompiler generates (md_full_code.c): bus accesses with their RAM/ROM
 * fast paths, flags, shifts, BCD, the status register, exceptions.  Each
 * helper takes the operand size as a parameter; generated code passes a
 * constant and the compiler folds the rest away. */
#ifndef M0BOY_MD_M68K_OPS_H
#define M0BOY_MD_M68K_OPS_H
#include "md_full.h"
#include "md_m68k_time.h"
#if defined(MD_PERF)
/* one counting of the emitted code per executed instruction (the
 * interpreter's md_full_execute already does m->instructions++): the
 * bench decompositions read instructions per frame on both paths */
#define MDSW(m) (++(m)->instructions)
#else
#define MDSW(m) ((void)0)
#endif
#ifdef MD_TRACE
#include "md_trace.h"
#define MD_T_RD(m, a, sz) do { if (md_trace.rd && !md_trace.quiet) md_trace.rd(md_trace.user, (m), (a), (sz)); } while (0)
#define MD_T_WR(m, a, sz, v) do { if (md_trace.wr) md_trace.wr(md_trace.user, (m), (a), (sz), (v)); } while (0)
#else
#define MD_T_RD(m, a, sz) ((void)0)
#define MD_T_WR(m, a, sz, v) ((void)0)
#endif

enum { MD_X = 16, MD_N = 8, MD_Z = 4, MD_V = 2, MD_C = 1, MD_S = 0x2000 };

static inline void md_spend(MdFull *m, unsigned cycles) { m->cycles += cycles; m->frame_cycles += cycles; }
static inline uint32_t md_mask(unsigned size) { return size == 1u ? 0xffu : size == 2u ? 0xffffu : 0xffffffffu; }
static inline uint32_t md_sign(unsigned size) { return size == 1u ? 0x80u : size == 2u ? 0x8000u : 0x80000000u; }
static inline uint32_t md_sext(uint32_t v, unsigned size) {
    return size == 1u ? (uint32_t)(int32_t)(int8_t)v : size == 2u ? (uint32_t)(int32_t)(int16_t)v : v;
}

/* ---- Bus: work RAM and ROM directly, the rest through md_full_bus.c -- */
static inline uint8_t md_rd8(MdFull *m, uint32_t a) {
    a &= 0xffffffu;
    MD_T_RD(m, a, 1u);
    if (m->sram_enable && a >= 0x200000u && a < 0x210000u) return m->sram[a & 0xffffu];
    if (a >= 0xff0000u) return m->ram[a & 0xffffu];
    if (a < md_rom_size) return md_rom[a];
    return md_full_read8(m, a);
}
static inline uint16_t md_rd16(MdFull *m, uint32_t a) {
    a &= 0xffffffu;
    MD_T_RD(m, a, 2u);
    if (!(a & 1u)) {
        if (m->sram_enable && a >= 0x200000u && a < 0x20ffffu)
            return (uint16_t)(m->sram[a & 0xffffu] << 8 | m->sram[(a + 1u) & 0xffffu]);
        if (a >= 0xff0000u) return (uint16_t)(m->ram[a & 0xffffu] << 8 | m->ram[(a + 1u) & 0xffffu]);
        if (a + 1u < md_rom_size) return (uint16_t)(md_rom[a] << 8 | md_rom[a + 1u]);
    }
    return md_full_read16(m, a);
}
static inline uint32_t md_rd32(MdFull *m, uint32_t a) {
    const uint32_t hi = md_rd16(m, a);
    return hi << 16 | md_rd16(m, a + 2u);
}
static inline void md_wr8(MdFull *m, uint32_t a, uint8_t v) {
    a &= 0xffffffu;
    MD_T_WR(m, a, 1u, v);
    if (a >= 0xff0000u) m->ram[a & 0xffffu] = v;
    else md_full_write8(m, a, v);
}
static inline void md_wr16(MdFull *m, uint32_t a, uint16_t v) {
    a &= 0xffffffu;
    MD_T_WR(m, a, 2u, v);
    if (a >= 0xff0000u && !(a & 1u)) { m->ram[a & 0xffffu] = (uint8_t)(v >> 8); m->ram[(a + 1u) & 0xffffu] = (uint8_t)v; }
    else md_full_write16(m, a, v);
}
static inline void md_wr32(MdFull *m, uint32_t a, uint32_t v) {
    md_wr16(m, a, (uint16_t)(v >> 16));
    if (!m->fault) md_wr16(m, a + 2u, (uint16_t)v);
}
static inline uint32_t md_rd(MdFull *m, uint32_t a, unsigned size) {
    return size == 1u ? md_rd8(m, a) : size == 2u ? md_rd16(m, a) : md_rd32(m, a);
}
static inline void md_wr(MdFull *m, uint32_t a, unsigned size, uint32_t v) {
    if (size == 1u) md_wr8(m, a, (uint8_t)v);
    else if (size == 2u) md_wr16(m, a, (uint16_t)v);
    else md_wr32(m, a, v);
}
/* Data register write of the low `size` bytes. */
static inline void md_setd(MdFull *m, unsigned r, uint32_t v, unsigned size) {
    m->d[r] = (m->d[r] & ~md_mask(size)) | (v & md_mask(size));
}

/* ---- Flags ------------------------------------------------------------ */
static inline void md_logic(MdFull *m, uint32_t v, unsigned size) {
    v &= md_mask(size);
    m->sr = (uint16_t)((m->sr & ~(MD_N | MD_Z | MD_V | MD_C)) | (v ? 0 : MD_Z) | (v & md_sign(size) ? MD_N : 0));
}
static inline uint32_t md_add(MdFull *m, uint32_t a, uint32_t b, unsigned size) {
    const uint32_t ms = md_mask(size), bit = md_sign(size);
    a &= ms; b &= ms;
    const uint32_t r = (a + b) & ms;
    const unsigned carry = (uint64_t)a + b > ms, overflow = (~(a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_N | MD_Z | MD_V | MD_C)) | (r ? 0 : MD_Z) | (r & bit ? MD_N : 0) |
                       (overflow ? MD_V : 0) | (carry ? MD_C | MD_X : 0));
    return r;
}
static inline uint32_t md_sub(MdFull *m, uint32_t a, uint32_t b, unsigned size) {
    const uint32_t ms = md_mask(size), bit = md_sign(size);
    a &= ms; b &= ms;
    const uint32_t r = (a - b) & ms;
    const unsigned borrow = b > a, overflow = ((a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_N | MD_Z | MD_V | MD_C)) | (r ? 0 : MD_Z) | (r & bit ? MD_N : 0) |
                       (overflow ? MD_V : 0) | (borrow ? MD_C | MD_X : 0));
    return r;
}
static inline void md_cmp(MdFull *m, uint32_t a, uint32_t b, unsigned size) {
    const uint16_t x = m->sr & MD_X;
    md_sub(m, a, b, size);
    m->sr = (uint16_t)((m->sr & ~MD_X) | x);
}
/* ADDX/SUBX/NEGX: X in, Z only ever cleared. */
static inline uint32_t md_addx(MdFull *m, uint32_t a, uint32_t b, unsigned size) {
    const uint32_t ms = md_mask(size), bit = md_sign(size), x = !!(m->sr & MD_X);
    a &= ms; b &= ms;
    const uint32_t r = (a + b + x) & ms;
    const unsigned carry = (uint64_t)a + b + x > ms, overflow = (~(a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_N | MD_V | MD_C) & (r ? ~MD_Z : 0xffffu)) | (r & bit ? MD_N : 0) |
                       (overflow ? MD_V : 0) | (carry ? MD_C | MD_X : 0));
    return r;
}
static inline uint32_t md_subx(MdFull *m, uint32_t a, uint32_t b, unsigned size) {
    const uint32_t ms = md_mask(size), bit = md_sign(size), x = !!(m->sr & MD_X);
    a &= ms; b &= ms;
    const uint32_t r = (a - b - x) & ms;
    const unsigned borrow = (uint64_t)b + x > a, overflow = ((a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_N | MD_V | MD_C) & (r ? ~MD_Z : 0xffffu)) | (r & bit ? MD_N : 0) |
                       (overflow ? MD_V : 0) | (borrow ? MD_C | MD_X : 0));
    return r;
}
/* BCD, the documented flags (N and V follow the result as most cores do). */
static inline uint8_t md_abcd(MdFull *m, uint8_t a, uint8_t b) {
    const unsigned x = !!(m->sr & MD_X);
    unsigned lo = (a & 15u) + (b & 15u) + x, r = (a & 0xf0u) + (b & 0xf0u);
    if (lo > 9u) lo += 6u;
    r += lo;
    const unsigned carry = r > 0x99u;
    if (carry) r += 0x60u;
    r &= 0xffu;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_C | MD_N | MD_V) & (r ? ~MD_Z : 0xffffu)) | (carry ? MD_X | MD_C : 0) |
                       (r & 0x80u ? MD_N : 0));
    return (uint8_t)r;
}
static inline uint8_t md_sbcd(MdFull *m, uint8_t a, uint8_t b) {
    const unsigned x = !!(m->sr & MD_X);
    int lo = (int)(a & 15u) - (int)(b & 15u) - (int)x, r = (int)(a & 0xf0u) - (int)(b & 0xf0u);
    if (lo < 0) lo -= 6;
    r += lo;
    const unsigned borrow = r < 0;
    if (borrow) r -= 0x60;
    r &= 0xff;
    m->sr = (uint16_t)((m->sr & ~(MD_X | MD_C | MD_N | MD_V) & (r ? ~MD_Z : 0xffffu)) | (borrow ? MD_X | MD_C : 0) |
                       (r & 0x80 ? MD_N : 0));
    return (uint8_t)r;
}
static inline int md_cc(const MdFull *m, unsigned cc) {
    const unsigned sr = m->sr, n = !!(sr & MD_N), z = !!(sr & MD_Z), v = !!(sr & MD_V), c = !!(sr & MD_C);
    switch (cc & 15u) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !c && !z;
    case 3: return c || z;
    case 4: return !c;
    case 5: return c;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}
/* ASd/LSd/ROXd/ROd (type 0..3) by `count`; ASL sets V when the sign changes. */
static inline uint32_t md_shift(MdFull *m, unsigned type, int left, uint32_t v, unsigned count, unsigned size) {
    const uint32_t ms = md_mask(size), bit = md_sign(size);
    const unsigned bits = size * 8u;
    unsigned last = type == 2u ? !!(m->sr & MD_X) : 0u, overflow = 0, x = !!(m->sr & MD_X);
    v &= ms;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned out = left ? !!(v & bit) : (v & 1u);
        uint32_t next = left ? v << 1 : v >> 1;
        if (type == 0u && !left) next |= v & bit;
        if (type == 2u) { next |= left ? x : x << (bits - 1u); x = out; }
        if (type == 3u) next |= left ? out : (uint32_t)out << (bits - 1u);
        next &= ms;
        if (type == 0u && left && ((next ^ v) & bit)) overflow = 1;
        v = next;
        last = out;
    }
    uint16_t sr = (uint16_t)(m->sr & ~(MD_N | MD_Z | MD_V | MD_C));
    sr |= (v ? 0 : MD_Z) | (v & bit ? MD_N : 0) | (overflow ? MD_V : 0);
    if (type == 2u) sr = (uint16_t)((sr & ~MD_X) | (x ? MD_X | MD_C : 0));
    else if (count) {
        sr |= last ? MD_C : 0;
        if (type != 3u) sr = (uint16_t)((sr & ~MD_X) | (last ? MD_X : 0));
    }
    m->sr = sr;
    return v;
}

/* ---- Status register, stack, exceptions ------------------------------- */
/* Any change of SR ends the generated region: the dispatcher looks at the
 * interrupt mask again before the next instruction (the interpreter does
 * so after each one anyway). */
static inline void md_set_sr(MdFull *m, uint16_t v) {
    v &= 0xa71fu;
    if ((v ^ m->sr) & MD_S) { const uint32_t t = m->a[7]; m->a[7] = m->usp; m->usp = t; }
    m->sr = v;
    m->slice_limit = 0;
}
static inline void md_push16(MdFull *m, uint16_t v) { m->a[7] -= 2u; md_wr16(m, m->a[7], v); }
static inline void md_push32(MdFull *m, uint32_t v) { m->a[7] -= 4u; md_wr32(m, m->a[7], v); }
static inline uint16_t md_pop16(MdFull *m) { const uint16_t v = md_rd16(m, m->a[7]); m->a[7] += 2u; return v; }
static inline uint32_t md_pop32(MdFull *m) { const uint32_t v = md_rd32(m, m->a[7]); m->a[7] += 4u; return v; }
/* Exception: supervisor mode, PC and SR stacked, the vector loaded.  `pc`
 * is the address stacked (the next instruction for traps). */
static inline void md_exception(MdFull *m, unsigned vector, uint32_t pc) {
    const uint16_t old = m->sr;
    md_set_sr(m, (uint16_t)((old | MD_S) & ~0x8000u));
    md_push32(m, pc);
    md_push16(m, old);
    m->pc = md_rd32(m, vector * 4u);
}

/* ---- Multiply and divide (with their data-dependent time) ------------- */
static inline void md_mulu(MdFull *m, unsigned reg, uint16_t src) {
    md_spend(m, md_m68k_mulu_cycles(src));
    m->d[reg] = (uint32_t)(uint16_t)m->d[reg] * src;
    md_logic(m, m->d[reg], 4);
}
static inline void md_muls(MdFull *m, unsigned reg, uint16_t src) {
    md_spend(m, md_m68k_muls_cycles(src));
    m->d[reg] = (uint32_t)((int32_t)(int16_t)m->d[reg] * (int16_t)src);
    md_logic(m, m->d[reg], 4);
}
/* Returns 0 after a divide by zero (the exception is taken, `next` stacked). */
static inline int md_divu(MdFull *m, unsigned reg, uint16_t src, uint32_t next) {
    if (!src) { md_spend(m, 38); md_exception(m, 5, next); return 0; }
    const uint32_t dividend = m->d[reg];
    md_spend(m, md_m68k_divu_cycles(dividend, src));
    const uint32_t q = dividend / src, r = dividend % src;
    if (q > 0xffffu) m->sr = (uint16_t)((m->sr & ~MD_C) | MD_V | MD_N);
    else { m->d[reg] = r << 16 | q; md_logic(m, q, 2); }
    return 1;
}
static inline int md_divs(MdFull *m, unsigned reg, uint16_t src, uint32_t next) {
    if (!src) { md_spend(m, 38); md_exception(m, 5, next); return 0; }
    const int32_t dividend = (int32_t)m->d[reg], divisor = (int16_t)src;
    md_spend(m, md_m68k_divs_cycles(dividend, (int16_t)divisor));
    if (dividend == INT32_MIN && divisor == -1) { m->sr = (uint16_t)((m->sr & ~MD_C) | MD_V | MD_N); return 1; }
    const int32_t q = dividend / divisor, r = dividend % divisor;
    if (q < -32768 || q > 32767) m->sr = (uint16_t)((m->sr & ~MD_C) | MD_V | MD_N);
    else { m->d[reg] = (uint32_t)r << 16 | (uint16_t)q; md_logic(m, (uint32_t)q, 2); }
    return 1;
}

#endif
