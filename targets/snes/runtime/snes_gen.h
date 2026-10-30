/* What recompiled 65C816 code (snes_gen.c, written by the recompiler's
 * src/system/snes/emitter.cpp) needs from the runtime, and what the
 * dispatcher (snes_cpu.c under SNES_NATIVE) needs from it.
 *
 * A region is one C function over neighbouring instructions decoded under
 * one context — the program bank, the data bank, the direct page and the
 * M/X/E flags packed (snes_ctx_key).  It loads the CPU's registers into
 * locals, runs with the interpreter's own expressions, and stores
 * everything back when it leaves: on the budget's end, on a transfer it
 * cannot follow (RTS, RTL, RTI, a jump through a pointer, BRK, COP), or by
 * a call into the region that holds a known target.  The budget is the
 * interpreter's: CPU cycles left in the slice, signed.  The dispatcher
 * only enters an entry whose context is still the machine's, so a changed
 * flag or bank falls back to the interpreter instead of running the wrong
 * bytes. */
#ifndef SNES_GEN_H
#define SNES_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "snes.h"
#include "m0_idle.h"

typedef int32_t (*snes_region_fn)(int32_t budget, unsigned entry);

/* the entry at (K:PC, context), or -1 (generated) */
int snes_gen_lookup(uint32_t pc24, uint64_t ctx);
/* runs from entry `entry`, returns the budget left (generated) */
int32_t snes_gen_enter(int entry, int32_t budget);
/* 0: everything through the interpreter (the reference) */
extern int snes_native;

/* BRA/JMP to themselves (3 cycles a lap), a game waiting for its
 * interrupt: the rest of the slice in laps at once (m0_idle.h), left with
 * pc on the loop as the interpreter would.  b > 0. */
#define SNES_SPIN3(b_) ((b_) - 3 * (int32_t)m0_idle_loops((uint32_t)(b_), 3u))

#ifdef SNES_GEN_IMPL
/* The generated code's state — the registers as locals, the flags and the
 * emulation bit among them (XCE moves e_, PLP the widths) — and the
 * interpreter's own expressions (snes_cpu.c) over them: the same code, so
 * a recompiled instruction is its interpreted self, cycle for cycle. */

#define SNES_LOAD() \
    uint16_t a_ = machine.cpu.a, x_ = machine.cpu.x, y_ = machine.cpu.y; \
    uint16_t s_ = machine.cpu.s, d_ = machine.cpu.d; \
    uint8_t k_ = machine.cpu.k, dbr_ = machine.cpu.dbr, p_ = machine.cpu.p, e_ = machine.cpu.e; \
    uint16_t pc_ = machine.cpu.pc
#define SNES_SAVE() do { \
    machine.cpu.a = a_; machine.cpu.x = x_; machine.cpu.y = y_; \
    machine.cpu.s = s_; machine.cpu.d = d_; machine.cpu.k = k_; \
    machine.cpu.dbr = dbr_; machine.cpu.p = p_; machine.cpu.e = e_; \
} while (0)

#define SNES_RD(at) snes_bus_read((uint32_t)(at))
#define SNES_WR(at, v) snes_bus_write((uint32_t)(at), (v))

#define SNES_MF (p_ & SNES_P_M)
#define SNES_XF (p_ & SNES_P_X)

#define SNES_OP8(i) SNES_RD((uint32_t)k_ << 16 | (uint16_t)(pc_ + (i)))
#define SNES_OP16(i) (unsigned)(SNES_OP8(i) | SNES_OP8((uint16_t)((pc_ + (i)) + 1)) << 8)

#define SNES_WRAP16(e_) ((uint32_t)(((e_) & 0xFF0000u) | (uint16_t)((e_) + 1)))
#define SNES_RDW(ea) (__extension__({ const uint32_t rdw_addr_ = (ea); \
    (unsigned)(SNES_RD(rdw_addr_) | SNES_RD(SNES_WRAP16(rdw_addr_)) << 8); }))
#define SNES_RD24(ea) (__extension__({ const uint32_t rd24_addr_ = (ea); \
    (unsigned)(SNES_RD(rd24_addr_) | SNES_RD(rd24_addr_ + 1) << 8); }))
#define SNES_WRW(ea, v) do { const uint32_t wrw_addr_ = (ea); SNES_WR(wrw_addr_, (uint8_t)(v)); \
    SNES_WR(SNES_WRAP16(wrw_addr_), (uint8_t)((v) >> 8)); } while (0)
#define SNES_WR24(ea, v) do { const uint32_t wr24_addr_ = (ea); SNES_WR(wr24_addr_, (uint8_t)(v)); \
    SNES_WR(wr24_addr_ + 1, (uint8_t)((v) >> 8)); } while (0)

#define SNES_PUSH(v_) do { SNES_WR((uint32_t)s_, (v_)); \
    s_ = e_ ? (uint16_t)(0x0100u | ((s_ - 1) & 0xFF)) : (uint16_t)(s_ - 1); } while (0)
#define SNES_POP() (__extension__({ s_ = e_ ? (uint16_t)(0x0100u | ((s_ + 1) & 0xFF)) : (uint16_t)(s_ + 1); \
    SNES_RD((uint32_t)s_); }))
#define SNES_PUSH16(w_) do { const unsigned w16_ = (w_); SNES_PUSH((uint8_t)(w16_ >> 8)); SNES_PUSH((uint8_t)w16_); } while (0)
#define SNES_POP16() (__extension__({ const unsigned lo_ = SNES_POP(); (unsigned)(lo_ | SNES_POP() << 8); }))

#define SNES_ZN8(v_) do { const uint8_t zn_v_ = (v_); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_N)) | (zn_v_ ? (zn_v_ & 0x80u ? SNES_P_N : 0) : SNES_P_Z)); } while (0)
#define SNES_ZN16(v_) do { const uint16_t zn_v_ = (v_); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_N)) | (zn_v_ ? (zn_v_ & 0x8000u ? SNES_P_N : 0) : SNES_P_Z)); } while (0)

/* ADC/SBC: binary, decimal (+1 cycle, the 65C816's BCD in both widths) and
 * the widths compose; the same formulas as the interpreter's */
/* the BCD add, nibble by nibble with the carry chained; the subtract with
 * the borrow chained the same way (the NMOS algorithms) */
#define SNES_ADC_DEC_BYTE(x_, y_, c_, out_, carry_) do { \
    unsigned adc_t_ = ((x_) & 0x0F) + ((y_) & 0x0F) + (c_); \
    if (adc_t_ >= 0x0A) adc_t_ = ((adc_t_ + 0x06) & 0x0F) + 0x10; \
    adc_t_ += ((x_) & 0xF0) + ((y_) & 0xF0); \
    if (adc_t_ >= 0xA0) adc_t_ += 0x60; \
    (out_) = (unsigned)(adc_t_ & 0xFF); (carry_) = (adc_t_ >> 8) & 1; \
} while (0)
#define SNES_SBC_DEC_BYTE(x_, y_, borrow_, out_, bout_) do { \
    unsigned sbc_t_ = ((x_) & 0x0F) - ((y_) & 0x0F) - (borrow_); \
    unsigned sbc_b_ = 0; \
    if (sbc_t_ & 0x10u) { sbc_b_ = 1; sbc_t_ = ((sbc_t_ - 0x06) & 0x0F) - 0x10; } \
    sbc_t_ += ((x_) & 0xF0) - ((y_) & 0xF0); \
    if (sbc_t_ & 0x100u) { sbc_b_ = 1; sbc_t_ -= 0x60; } \
    (out_) = (unsigned)(sbc_t_ & 0xFF); (bout_) = sbc_b_; \
} while (0)
#define SNES_ADC8(m_) do { const uint8_t adc_m_ = (m_); \
    if (p_ & SNES_P_D) { budget -= 1; \
        const unsigned adc_v_ = (a_ & 0xFF) + adc_m_ + (p_ & 1); \
        unsigned adc_t_ = (a_ & 0x0F) + (adc_m_ & 0x0F) + (p_ & 1); \
        if (adc_t_ >= 0x0A) adc_t_ = ((adc_t_ + 0x06) & 0x0F) + 0x10; \
        adc_t_ += (a_ & 0xF0) + (adc_m_ & 0xF0); \
        if (adc_t_ >= 0xA0) adc_t_ += 0x60; \
        p_ = (uint8_t)((p_ & ~SNES_P_C) | (adc_t_ >> 8 ? SNES_P_C : 0) \
            | (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_v_) & 0x80u)) >> 1)); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)adc_t_); SNES_ZN8((uint8_t)adc_t_); \
    } else { const unsigned adc_l_ = (a_ & 0xFF) + adc_m_ + (p_ & 1); \
        p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
            | (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_l_) & 0x80u)) >> 1) \
            | ((adc_l_ >> 8) & 1)); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)adc_l_); SNES_ZN8((uint8_t)adc_l_); } } while (0)
#define SNES_SBC8(m_) do { const uint8_t sbc_m_ = (m_); \
    if (p_ & SNES_P_D) { budget -= 1; \
        uint8_t sbc_lo_ = (uint8_t)((a_ & 0xF) - (sbc_m_ & 0xF) - ((p_ & 1) ^ 1)); \
        uint8_t sbc_hi_ = (uint8_t)((a_ >> 4) - (sbc_m_ >> 4) - ((sbc_lo_ >> 4) & 1)); \
        uint8_t sbc_res_ = (uint8_t)((sbc_hi_ << 4) | (sbc_lo_ & 0xF)); \
        p_ &= (uint8_t)~(SNES_P_Z | SNES_P_N | SNES_P_C); \
        if (sbc_lo_ & 0x10u) sbc_res_ -= 0x06; \
        if (sbc_hi_ & 0x10u) sbc_res_ -= 0x60; \
        a_ = (uint16_t)((a_ & 0xFF00u) | sbc_res_); \
        p_ |= (uint8_t)(((sbc_hi_ >> 4) & 1) ^ 1); SNES_ZN8(sbc_res_); \
    } else { const unsigned sbc_l_ = (a_ & 0xFF) - sbc_m_ - ((p_ & 1) ^ 1); \
        p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
            | ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x80u)) >> 1) \
            | ((((sbc_l_ >> 8) & 1) ^ 1))); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)sbc_l_); SNES_ZN8((uint8_t)sbc_l_); } } while (0)
#define SNES_ADC16(m_) do { const unsigned adc_m_ = (m_); \
    const unsigned adc_l_ = a_ + adc_m_ + (p_ & 1); \
    unsigned adc_r_ = adc_l_, adc_c_ = (unsigned)((adc_l_ >> 16) & 1); \
    if (p_ & SNES_P_D) { budget -= 1; unsigned adc_lo_, adc_hi_; \
        SNES_ADC_DEC_BYTE(a_ & 0xFF, adc_m_ & 0xFF, p_ & 1, adc_lo_, adc_c_); \
        SNES_ADC_DEC_BYTE(a_ >> 8, adc_m_ >> 8, adc_c_, adc_hi_, adc_c_); \
        adc_r_ = (adc_hi_ << 8) | adc_lo_; \
    } \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
        | (((((a_ ^ adc_m_) & 0x8000u) ^ 0x8000u) & ((a_ ^ adc_l_) & 0x8000u)) >> 9) \
        | (p_ & SNES_P_D ? (uint8_t)(adc_c_ ? SNES_P_C : 0) : (uint8_t)((adc_l_ >> 16) & 1))); \
    a_ = (uint16_t)adc_r_; SNES_ZN16(a_); } while (0)
#define SNES_SBC16(m_) do { const unsigned sbc_m_ = (m_); \
    const unsigned sbc_l_ = a_ - sbc_m_ - ((p_ & 1) ^ 1); \
    unsigned sbc_r_ = sbc_l_, sbc_nb_ = (unsigned)((((sbc_l_ >> 16) & 1) ^ 1)); \
    if (p_ & SNES_P_D) { budget -= 1; unsigned sbc_lo_, sbc_hi_, sbc_b_ = (p_ & 1) ^ 1; \
        SNES_SBC_DEC_BYTE(a_ & 0xFF, sbc_m_ & 0xFF, sbc_b_, sbc_lo_, sbc_b_); \
        SNES_SBC_DEC_BYTE(a_ >> 8, sbc_m_ >> 8, sbc_b_, sbc_hi_, sbc_b_); \
        sbc_r_ = (sbc_hi_ << 8) | sbc_lo_; sbc_nb_ = sbc_b_ ^ 1; \
    } \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
        | ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x8000u)) >> 9) \
        | (sbc_nb_ ? SNES_P_C : 0)); \
    a_ = (uint16_t)sbc_r_; SNES_ZN16(a_); } while (0)
#define SNES_CMP8(r_, m_) do { const unsigned t_ = ((r_) & 0xFF) - ((m_) & 0xFF); SNES_ZN8((uint8_t)t_); \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (((t_ >> 8) & 1) ^ 1)); } while (0)
#define SNES_CMP16(r_, m_) do { const unsigned t_ = (r_) - (m_); SNES_ZN16((uint16_t)t_); \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (((t_ >> 16) & 1) ^ 1)); } while (0)
#define SNES_BIT(m_, w_) do { const unsigned bit_m_ = (m_); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_V | SNES_P_N)) \
        | ((a_ & bit_m_) ? 0 : SNES_P_Z) \
        | (w_ ? ((bit_m_ >> 8) & 0xC0u) : ((bit_m_ & 0x80u) ? SNES_P_N : 0) | ((bit_m_ & 0x40u) ? SNES_P_V : 0))); \
    } while (0)

#define SNES_ASL8(m_) (__extension__({ unsigned r_ = (m_) & 0xFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 7)); \
    (uint8_t)(r_ << 1); }))
#define SNES_LSR8(m_) (__extension__({ unsigned r_ = (m_) & 0xFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); \
    (uint8_t)(r_ >> 1); }))
#define SNES_ROL8(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 7)); (uint8_t)((r_ << 1) | c_); }))
#define SNES_ROR8(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); (uint8_t)((r_ >> 1) | (c_ ? 0x80 : 0)); }))
#define SNES_ASL16(m_) (__extension__({ unsigned r_ = (m_) & 0xFFFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 15)); \
    (uint16_t)(r_ << 1); }))
#define SNES_LSR16(m_) (__extension__({ unsigned r_ = (m_) & 0xFFFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); \
    (uint16_t)(r_ >> 1); }))
#define SNES_ROL16(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFFFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 15)); (uint16_t)((r_ << 1) | c_); }))
#define SNES_ROR16(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFFFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); (uint16_t)((r_ >> 1) | (c_ ? 0x8000 : 0)); }))

/* register writes with the widths applied (the interpreter's WR_A/WR_X) */
#define SNES_WR_A8(v_) do { a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)(v_)); SNES_ZN8((uint8_t)a_); } while (0)
#define SNES_WR_A16(v_) do { a_ = (uint16_t)(v_); SNES_ZN16(a_); } while (0)
#define SNES_WR_X(v_) do { x_ = SNES_XF ? (uint16_t)((v_) & 0xFF) : (uint16_t)(v_); \
    if (SNES_XF) SNES_ZN8((uint8_t)x_); else SNES_ZN16(x_); } while (0)
#define SNES_WR_Y(v_) do { y_ = SNES_XF ? (uint16_t)((v_) & 0xFF) : (uint16_t)(v_); \
    if (SNES_XF) SNES_ZN8((uint8_t)y_); else SNES_ZN16(y_); } while (0)
#endif /* SNES_GEN_IMPL */

#ifdef __cplusplus
}
#endif
#endif
