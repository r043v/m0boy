/* What recompiled HuC6280 code (pce_gen.c, written by the recompiler's
 * src/system/pce/emitter.cpp) needs from the runtime, and what the
 * dispatcher (huc6280.c under PCE_NATIVE) needs from it.
 *
 * A region is one C function over neighbouring instructions decoded under
 * one MPR context (the eight page registers, packed MPR0 first).  It loads
 * the CPU's registers into locals and copies the bus's window tables, runs
 * with the interpreter's own macros, and stores everything back when it
 * leaves: on the budget's end, on a transfer it cannot follow (RTS, RTI,
 * a jump through a pointer), or by a call into the region that holds a
 * known target.  The budget is the interpreter's: CPU cycles left in the
 * slice, signed.  The dispatcher only enters an entry whose context is
 * still the machine's, so a re-mapped window falls back to the
 * interpreter instead of running the wrong bytes. */
#ifndef PCE_GEN_H
#define PCE_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pce.h"
#include "m0_idle.h"

typedef int32_t (*pce_region_fn)(int32_t budget, unsigned entry);

/* the entry at (pc, MPR context), or -1 (generated) */
int pce_gen_lookup(uint16_t pc, uint64_t mpr);
/* runs from entry `entry`, returns the budget left (generated) */
int32_t pce_gen_enter(int entry, int32_t budget);
/* 0: everything through the interpreter (the reference) */
extern int pce_native;

/* BRA/JMP to themselves (4 cycles a lap), a game waiting for its
 * interrupt: the rest of the slice in laps at once (m0_idle.h), left with
 * pc on the loop as the interpreter would.  b > 0. */
#define PCE_SPIN4(b_) ((b_) - 4 * (int32_t)m0_idle_loops((uint32_t)(b_), 4u))

#ifdef PCE_GEN_IMPL
/* The generated code's state — the registers as locals, the bus's two
 * window tables read in place through pointers to the CPU's own arrays —
 * and the interpreter's own macros (huc6280.c) over them: the same
 * expressions, so a recompiled instruction is its interpreted self, cycle
 * for cycle.  A region copies nothing of the windows: a TAM's
 * huc6280_map rewrites the arrays the pointers reach, the next access
 * sees the new mapping, there is no rebase to run. */

#define PCE_LOAD() \
    uint8_t a_ = machine.cpu.a, x_ = machine.cpu.x, y_ = machine.cpu.y, s_ = machine.cpu.s, \
            p_ = machine.cpu.p; \
    const uint8_t *const *rb_ = machine.cpu.rbase; \
    uint8_t *const *wb_ = machine.cpu.wbase
#define PCE_SAVE() do { \
    machine.cpu.a = a_; machine.cpu.x = x_; machine.cpu.y = y_; \
    machine.cpu.s = s_; machine.cpu.p = p_; \
} while (0)

/* the bus accesses of compiled code see the cycle they happen at (the slice's end minus the budget left) and
 * pay the wait states the access asked for, as the interpreter does */
#define PCE_BUS_SYNC() (machine.cpu.cycles = machine.cpu.gen_until - (uint32_t)budget)
#define PCE_BUS_DONE() do { budget -= (int32_t)machine.cpu.steal; machine.cpu.steal = 0; } while (0)
static inline uint8_t pce_bus_read_charged(int32_t *budget, uint16_t at) {
    const uint8_t v = pce_bus_read(at);
    *budget -= (int32_t)machine.cpu.steal; machine.cpu.steal = 0;
    return v;
}
#define PCE_RD(at) (__extension__({ uint16_t rd_at_ = (at); \
    const uint8_t *rd_b_ = rb_[rd_at_ >> 13]; \
    rd_b_ ? rd_b_[rd_at_ & 0x1FFFu] : (PCE_BUS_SYNC(), pce_bus_read_charged(&budget, rd_at_)); }))
#define PCE_WR(at, v) do { uint16_t wr_at_ = (at); uint8_t *wr_b_ = wb_[wr_at_ >> 13]; \
    if (wr_b_) wr_b_[wr_at_ & 0x1FFFu] = (v); else { const uint8_t wr_v_ = (v); PCE_BUS_SYNC(); pce_bus_write(wr_at_, wr_v_); PCE_BUS_DONE(); } } while (0)
#define PCE_PUSH(v_) do { PCE_WR((uint16_t)(0x2100u | s_), (v_)); s_--; } while (0)
#define PCE_POP() (__extension__({ s_++; PCE_RD((uint16_t)(0x2100u | s_)); }))

/* the zero page is MPR1's window at $2000; (zp) wraps inside it, there is
 * no page-crossing cost anywhere */
#define PCE_ZP_AT(o) ((uint16_t)(0x2000u | (uint8_t)(o)))

#define PCE_ZN(v_) do { const uint8_t zn_v_ = (v_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_N)) | (zn_v_ ? (zn_v_ & 0x80u ? PCE_P_N : 0) : PCE_P_Z)); } while (0)

/* Flag updates under the liveness mask (N|Z|C|V bits, an immediate the
 * recompiler folds): a dead flag is neither set nor cleared — the mask
 * stays out of the interpreter's clear set too, so a path that skips a
 * flag leaves it untouched, as the interpreter's own narrower clears do
 * (the decimal paths never touch V).  The generated calls pass the
 * region's live set; the ternaries fold away at compile time. */
#define PCE_P_NZCV (PCE_P_N | PCE_P_Z | PCE_P_C | PCE_P_V)
#define PCE_P_NZC (PCE_P_N | PCE_P_Z | PCE_P_C)
#define PCE_P_NZV (PCE_P_N | PCE_P_Z | PCE_P_V)
#define PCE_P_NV (PCE_P_N | PCE_P_V)
#define PCE_P_NZ (PCE_P_N | PCE_P_Z)
#define PCE_ZNOF(v_) ((uint8_t)((v_) ? ((v_) & 0x80u ? PCE_P_N : 0) : PCE_P_Z))

/* ADC/SBC: binary, decimal (+1 cycle) and T mode compose; the T mode's
 * result lands in the zero page at X and leaves the flags of the result.
 * The decimal paths clear only N|Z|C (V survives, as the interpreter's
 * own narrower clear keeps it); the mask's dead bits are not cleared. */
#define PCE_ADC_CORE(m_, lv_) do { const uint8_t adc_m_ = (m_); \
    if (p_ & PCE_P_D) { budget -= 1; \
        unsigned adc_t_ = (a_ & 0x0F) + ((adc_m_) & 0x0F) + (p_ & 1); \
        if (adc_t_ >= 0x0A) adc_t_ += 0x06; \
        adc_t_ += (a_ & 0xF0) + ((adc_m_) & 0xF0); \
        p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
            | ((lv_) & PCE_P_C ? (adc_t_ & 0xFF00u ? PCE_P_C : 0) : 0) \
            | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)adc_t_) : 0)); \
        a_ = (uint8_t)adc_t_; \
    } else { const unsigned adc_l_ = a_ + adc_m_ + (p_ & 1); \
        p_ = (uint8_t)((p_ & ~(PCE_P_NZCV & (lv_))) \
            | ((lv_) & PCE_P_V ? (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_l_) & 0x80u)) >> 1) : 0) \
            | ((lv_) & PCE_P_C ? (adc_l_ >> 8) & 1 : 0) \
            | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)adc_l_) : 0)); \
        a_ = (uint8_t)adc_l_; } } while (0)
#define PCE_SBC_CORE(m_, lv_) do { const uint8_t sbc_m_ = (m_); \
    if (p_ & PCE_P_D) { budget -= 1; \
        uint8_t sbc_lo_ = (uint8_t)((a_ & 0xF) - ((sbc_m_) & 0xF) - ((p_ & 1) ^ 1)); \
        uint8_t sbc_hi_ = (uint8_t)((a_ >> 4) - ((sbc_m_) >> 4) - ((sbc_lo_ >> 4) & 1)); \
        uint8_t sbc_res_ = (uint8_t)((sbc_hi_ << 4) | (sbc_lo_ & 0xF)); \
        if (sbc_lo_ & 0x10u) sbc_res_ -= 0x06; \
        if (sbc_hi_ & 0x10u) sbc_res_ -= 0x60; \
        p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
            | ((lv_) & PCE_P_C ? (((sbc_hi_ >> 4) & 1) ^ 1) : 0) \
            | ((lv_) & PCE_P_NZ ? PCE_ZNOF(sbc_res_) : 0)); \
        a_ = sbc_res_; \
    } else { const unsigned sbc_l_ = a_ - sbc_m_ - ((p_ & 1) ^ 1); \
        p_ = (uint8_t)((p_ & ~(PCE_P_NZCV & (lv_))) \
            | ((lv_) & PCE_P_V ? ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x80u)) >> 1) : 0) \
            | ((lv_) & PCE_P_C ? (((sbc_l_ >> 8) & 1) ^ 1) : 0) \
            | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)sbc_l_) : 0)); \
        a_ = (uint8_t)sbc_l_; } } while (0)
/* the operand is read first, whatever the mode */
#define PCE_LOGIC(opname, get, lv_) do { const uint8_t m_ = (get); \
    if (p_ & PCE_P_T) { const uint8_t aback_ = a_; \
        a_ = PCE_RD(PCE_ZP_AT(x_)); budget -= 2; a_ opname##= m_; \
        if ((lv_) & PCE_P_NZ) PCE_ZN(a_); \
        PCE_WR(PCE_ZP_AT(x_), a_); a_ = aback_; \
    } else { a_ opname##= m_; if ((lv_) & PCE_P_NZ) PCE_ZN(a_); } } while (0)
#define PCE_ARITH(core, get, lv_) do { const uint8_t m_ = (get); \
    if (p_ & PCE_P_T) { const uint8_t aback_ = a_; \
        a_ = PCE_RD(PCE_ZP_AT(x_)); budget -= 2; core(m_, lv_); \
        PCE_WR(PCE_ZP_AT(x_), a_); a_ = aback_; \
    } else { core(m_, lv_); } } while (0)

#define PCE_CMP(r_, m_, lv_) do { const unsigned t_ = (r_) - (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
        | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)t_) : 0) \
        | ((lv_) & PCE_P_C ? (((t_ >> 8) & 1) ^ 1) : 0)); } while (0)
/* BIT and TST: the Z from the and, the N and V from the operand itself */
#define PCE_BIT(m_, lv_) do { const uint8_t m__ = (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZV & (lv_))) \
        | ((lv_) & PCE_P_Z ? (a_ & (m__) ? 0 : PCE_P_Z) : 0) \
        | ((m__) & (lv_) & PCE_P_NV)); } while (0)
#define PCE_TST(m_, t_, lv_) do { const uint8_t m__ = (m_), t__ = (t_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZV & (lv_))) \
        | ((lv_) & PCE_P_Z ? ((t__ & (m__)) ? 0 : PCE_P_Z) : 0) \
        | ((m__) & (lv_) & PCE_P_NV)); } while (0)

#define PCE_ASL(m_, lv_) (__extension__({ unsigned r_ = (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
        | ((lv_) & PCE_P_C ? (r_ >> 7) : 0) \
        | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)((r_ << 1) & 0xFFu)) : 0)); \
    (uint8_t)((r_ << 1) & 0xFFu); }))
#define PCE_LSR(m_, lv_) (__extension__({ unsigned r_ = (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
        | ((lv_) & PCE_P_C ? (r_ & 1) : 0) \
        | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)(r_ >> 1)) : 0)); \
    (uint8_t)(r_ >> 1); }))
#define PCE_ROL(m_, lv_) (__extension__({ const unsigned c_ = p_ & PCE_P_C; unsigned r_ = (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
        | ((lv_) & PCE_P_C ? (r_ >> 7) : 0) \
        | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)(((r_ << 1) | c_) & 0xFFu)) : 0)); \
    (uint8_t)(((r_ << 1) | c_) & 0xFFu); }))
#define PCE_ROR(m_, lv_) (__extension__({ const unsigned c_ = p_ & PCE_P_C; unsigned r_ = (m_); \
    p_ = (uint8_t)((p_ & ~(PCE_P_NZC & (lv_))) \
        | ((lv_) & PCE_P_C ? (r_ & 1) : 0) \
        | ((lv_) & PCE_P_NZ ? PCE_ZNOF((uint8_t)((r_ >> 1) | (c_ << 7))) : 0)); \
    (uint8_t)((r_ >> 1) | (c_ << 7)); }))
#endif /* PCE_GEN_IMPL */

#ifdef __cplusplus
}
#endif

#endif
