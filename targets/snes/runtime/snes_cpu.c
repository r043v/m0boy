/* 5A22 (65C816) interpreter, after the HuC6280 core's model: one loop, one
 * instruction per turn, the budget signed so an instruction may run past
 * `until`.  The opcode set is the WDC manual's — every byte decodes, $42
 * WDM is a two-byte NOP.  The flags live in p_ as N V M X D I Z C; the B
 * bit exists only on the stack (PHP/BRK/COP push it set, interrupts clear,
 * PLP and RTI read it back as the X flag).  The E flag is a separate local:
 * XCE moves it, RTI keeps it (the emulation mode's RTI pulls bit 5 of P,
 * which is always set), everything else preserves it; the emulation mode
 * forces M and X, clamps S into page $01 and zeroes the index registers'
 * high bytes.  The cycle counts carry the WDC matrix's penalties: the
 * direct page's low byte, the 16-bit widths, and the page crossings of the
 * indexed reads (the 16-bit index takes one cycle instead).  The
 * recompiled code (snes_gen.c) computes the same penalties as constants
 * from its context — the two must stay word for word the same. */
#if __has_include("snes_config.h") /* a generated project's: SNES_NATIVE */
#include "snes_config.h"
#endif
#include "snes.h"
/* the debug tracer's per-instruction hook (the harness builds with
 * -DSNES_TRACE and provides snes_trace_step); the dyn hook marks the
 * addresses a dynamic transfer (RTS, RTL, RTI, a jump through a pointer,
 * an interrupt) landed on */
#ifdef SNES_TRACE
extern void snes_trace_step(uint32_t pc24);
extern void snes_trace_dyn(uint32_t pc24);
#define SNES_TRACE_HOOK(pc24) snes_trace_step(pc24)
#define SNES_DYN_HOOK(pc24) snes_trace_dyn(pc24)
#else
#define SNES_TRACE_HOOK(pc24) ((void)0)
#define SNES_DYN_HOOK(pc24) ((void)0)
#endif

/* the hot state — K, PC, the stack pointer, the registers, the banks —
 * lives in snes_interp's locals.  The macros' locals carry prefixes so an
 * argument never shadows them. */
#define rd(at) snes_bus_read((uint32_t)(at))
#define wr(at, v) snes_bus_write((uint32_t)(at), (v))

#define MF (p_ & SNES_P_M)
#define XF (p_ & SNES_P_X)
#define DPPEN ((d_ & 0xFF) ? 1 : 0)

/* the instruction's bytes at k_:pc_ (the operands are always literal
 * offsets: 1, 2, 3) */
#define OP8(i) rd((uint32_t)k_ << 16 | (uint16_t)(pc_ + (i)))
#define OP16(i) (unsigned)(OP8(i) | OP8((i) + 1) << 8)
#define OP24(i) ((uint32_t)OP8(i) | (uint32_t)OP8((i) + 1) << 8 | (uint32_t)OP8((i) + 2) << 16)

/* the second byte of a 16-bit access wraps inside the bank; a long
 * access's carries into the next one */
#define WRAP16(e_) ((uint32_t)(((e_) & 0xFF0000u) | (uint16_t)((e_) + 1)))
#define RDW(ea) (__extension__({ const uint32_t rdw_addr_ = (ea); \
    (unsigned)(rd(rdw_addr_) | rd(WRAP16(rdw_addr_)) << 8); }))
#define RD24(ea) (__extension__({ const uint32_t rd24_addr_ = (ea); \
    (unsigned)(rd(rd24_addr_) | rd(rd24_addr_ + 1) << 8); }))
#define WRW(ea, v) do { const uint32_t wrw_addr_ = (ea); wr(wrw_addr_, (uint8_t)(v)); \
    wr(WRAP16(wrw_addr_), (uint8_t)((v) >> 8)); } while (0)
#define WR24(ea, v) do { const uint32_t wr24_addr_ = (ea); wr(wr24_addr_, (uint8_t)(v)); \
    wr(wr24_addr_ + 1, (uint8_t)((v) >> 8)); } while (0)

/* addressing: the direct page lives in bank $00, its index wraps at 8
 * bits while the X flag is set; the absolute forms stay inside the data
 * bank, the long ones carry into the next */
#define A_DP(i) ((uint32_t)(uint16_t)(d_ + OP8(i)))
#define A_DPX(i) ((uint32_t)(uint16_t)(d_ + ((e_ && !(d_ & 0xFF)) ? (uint8_t)(OP8(i) + x_) : (OP8(i) + x_))))
#define A_DPY(i) ((uint32_t)(uint16_t)(d_ + ((e_ && !(d_ & 0xFF)) ? (uint8_t)(OP8(i) + y_) : (OP8(i) + y_))))
#define A_INDP(i) (__extension__({ const uint32_t q_ = A_DP(i); \
    (uint32_t)dbr_ << 16 | (rd(q_) | rd(WRAP16(q_)) << 8); }))
#define A_INDPX(i) (__extension__({ const uint32_t q_ = A_DPX(i); \
    (uint32_t)dbr_ << 16 | (rd(q_) | rd(WRAP16(q_)) << 8); }))
#define A_INDPY(i) (__extension__({ const uint32_t q_ = A_DP(i); \
    const unsigned w_ = rd(q_) | rd(WRAP16(q_)) << 8; \
    (uint32_t)dbr_ << 16 | (uint16_t)(w_ + y_); }))
#define A_INDLP(i) (__extension__({ const uint32_t q_ = A_DP(i); \
    (uint32_t)rd(q_) | (uint32_t)rd(WRAP16(q_)) << 8 | (uint32_t)rd((uint32_t)(uint16_t)(q_ + 2)) << 16; }))
#define A_INDLPY(i) (A_INDLP(i) + y_)
#define A_ABS(i) ((uint32_t)dbr_ << 16 | OP16(i))
#define A_ABSX(i) ((uint32_t)dbr_ << 16 | (uint16_t)(OP16(i) + (XF ? (x_ & 0xFF) : x_)))
#define A_ABSY(i) ((uint32_t)dbr_ << 16 | (uint16_t)(OP16(i) + (XF ? (y_ & 0xFF) : y_)))
#define A_LONG(i) OP24(i)
#define A_LONGX(i) (OP24(i) + (XF ? (x_ & 0xFF) : x_))
#define A_LONGY(i) (OP24(i) + (XF ? (y_ & 0xFF) : y_))
#define A_SR(i) ((uint32_t)(uint16_t)(s_ + OP8(i)))
#define A_SRIY(i) (__extension__({ const uint32_t q_ = A_SR(i); \
    const unsigned w_ = rd(q_) | rd(WRAP16(q_)) << 8; \
    (uint32_t)dbr_ << 16 | (uint16_t)(w_ + y_); }))

/* the page-cross penalty of an indexed read: the 8-bit index pays one
 * cycle when the page crosses, the 16-bit index pays one always */
#define CROSS(base, idx) (XF ? ((((base) & 0xFFu) + ((idx) & 0xFFu)) > 0xFFu ? 1 : 0) : 1)

#define set_zn8(v) do { const uint8_t zn_v_ = (v); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_N)) | (zn_v_ ? (zn_v_ & 0x80u ? SNES_P_N : 0) : SNES_P_Z)); } while (0)
#define set_zn16(v) do { const uint16_t zn_v_ = (v); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_N)) | (zn_v_ ? (zn_v_ & 0x8000u ? SNES_P_N : 0) : SNES_P_Z)); } while (0)

/* the stack: bank $00, S; the emulation mode wraps it inside page $01 */
#define PUSH(v) do { wr((uint32_t)s_, (v)); \
    s_ = e_ ? (uint16_t)(0x0100u | ((s_ - 1) & 0xFF)) : (uint16_t)(s_ - 1); } while (0)
#define POP() (__extension__({ s_ = e_ ? (uint16_t)(0x0100u | ((s_ + 1) & 0xFF)) : (uint16_t)(s_ + 1); \
    rd((uint32_t)s_); }))
#define PUSH16(w_) do { const unsigned w16_ = (w_); PUSH((uint8_t)(w16_ >> 8)); PUSH((uint8_t)w16_); } while (0)
#define POP16() (__extension__({ const unsigned lo_ = POP(); (unsigned)(lo_ | POP() << 8); }))

/* register writes respect the widths: the 8-bit accumulator writes A's
 * low byte, an 8-bit index write leaves the high byte zero */
#define WR_A8(v_) do { a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)(v_)); set_zn8((uint8_t)a_); } while (0)
#define WR_A16(v_) do { a_ = (uint16_t)(v_); set_zn16(a_); } while (0)
#define WR_X(v_) do { x_ = XF ? (uint16_t)((v_) & 0xFF) : (uint16_t)(v_); \
    if (XF) set_zn8((uint8_t)x_); else set_zn16(x_); } while (0)
#define WR_Y(v_) do { y_ = XF ? (uint16_t)((v_) & 0xFF) : (uint16_t)(v_); \
    if (XF) set_zn8((uint8_t)y_); else set_zn16(y_); } while (0)

/* ADC/SBC: binary and decimal compose, the decimal only in 8 bits (the
 * 16-bit forms are always binary); the formulas are the HuC6280 core's */
#define ADC8(m_) do { const uint8_t adc_m_ = (m_); \
    if (p_ & SNES_P_D) { left++; \
        const unsigned adc_v_ = (a_ & 0xFF) + adc_m_ + (p_ & 1); \
        unsigned adc_t_ = (a_ & 0x0F) + (adc_m_ & 0x0F) + (p_ & 1); \
        if (adc_t_ >= 0x0A) adc_t_ = ((adc_t_ + 0x06) & 0x0F) + 0x10; \
        adc_t_ += (a_ & 0xF0) + (adc_m_ & 0xF0); \
        if (adc_t_ >= 0xA0) adc_t_ += 0x60; \
        p_ = (uint8_t)((p_ & ~SNES_P_C) | (adc_t_ >> 8 ? SNES_P_C : 0) \
            | (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_v_) & 0x80u)) >> 1)); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)adc_t_); set_zn8((uint8_t)adc_t_); \
    } else { const unsigned adc_l_ = (a_ & 0xFF) + adc_m_ + (p_ & 1); \
        p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
            | (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_l_) & 0x80u)) >> 1) \
            | ((adc_l_ >> 8) & 1)); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)adc_l_); set_zn8((uint8_t)adc_l_); } } while (0)
#define SBC8(m_) do { const uint8_t sbc_m_ = (m_); \
    if (p_ & SNES_P_D) { left++; \
        uint8_t sbc_lo_ = (uint8_t)((a_ & 0xF) - (sbc_m_ & 0xF) - ((p_ & 1) ^ 1)); \
        uint8_t sbc_hi_ = (uint8_t)((a_ >> 4) - (sbc_m_ >> 4) - ((sbc_lo_ >> 4) & 1)); \
        uint8_t sbc_res_ = (uint8_t)((sbc_hi_ << 4) | (sbc_lo_ & 0xF)); \
        p_ &= (uint8_t)~(SNES_P_Z | SNES_P_N | SNES_P_C); \
        if (sbc_lo_ & 0x10u) sbc_res_ -= 0x06; \
        if (sbc_hi_ & 0x10u) sbc_res_ -= 0x60; \
        a_ = (uint16_t)((a_ & 0xFF00u) | sbc_res_); \
        p_ |= (uint8_t)(((sbc_hi_ >> 4) & 1) ^ 1); set_zn8(sbc_res_); \
    } else { const unsigned sbc_l_ = (a_ & 0xFF) - sbc_m_ - ((p_ & 1) ^ 1); \
        p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
            | ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x80u)) >> 1) \
            | ((((sbc_l_ >> 8) & 1) ^ 1))); \
        a_ = (uint16_t)((a_ & 0xFF00u) | (uint8_t)sbc_l_); set_zn8((uint8_t)sbc_l_); } } while (0)
/* the decimal modes: the 65C816's BCD add, nibble by nibble with the
 * carry chained — N and Z from the BCD result, C from its carry, V from
 * the plain binary sum (the WDC behaviour the test ROMs check) */
#define ADC_DEC_BYTE(x_, y_, c_, out_, carry_) do { \
    unsigned adc_t_ = ((x_) & 0x0F) + ((y_) & 0x0F) + (c_); \
    if (adc_t_ >= 0x0A) adc_t_ = ((adc_t_ + 0x06) & 0x0F) + 0x10; \
    adc_t_ += ((x_) & 0xF0) + ((y_) & 0xF0); \
    if (adc_t_ >= 0xA0) adc_t_ += 0x60; \
    (out_) = (unsigned)(adc_t_ & 0xFF); (carry_) = (adc_t_ >> 8) & 1; \
} while (0)
#define ADC16(m_) do { const unsigned adc_m_ = (m_); \
    const unsigned adc_l_ = a_ + adc_m_ + (p_ & 1); \
    unsigned adc_c_ = 0, adc_r_; \
    if (p_ & SNES_P_D) { left++; unsigned adc_lo_, adc_hi_; \
        ADC_DEC_BYTE(a_ & 0xFF, adc_m_ & 0xFF, p_ & 1, adc_lo_, adc_c_); \
        ADC_DEC_BYTE(a_ >> 8, adc_m_ >> 8, adc_c_, adc_hi_, adc_c_); \
        adc_r_ = (adc_hi_ << 8) | adc_lo_; \
    } else adc_r_ = adc_l_; \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
        | (((((a_ ^ adc_m_) & 0x8000u) ^ 0x8000u) & ((a_ ^ adc_l_) & 0x8000u)) >> 9) \
        | (p_ & SNES_P_D ? (uint8_t)(adc_c_ ? SNES_P_C : 0) : (uint8_t)((adc_l_ >> 16) & 1))); \
    a_ = (uint16_t)adc_r_; set_zn16(a_); } while (0)
/* the BCD subtract: the borrow chained the same way (the NMOS algorithm
 * the 8-bit form already follows) */
#define SBC_DEC_BYTE(x_, y_, borrow_, out_, bout_) do { \
    unsigned sbc_t_ = ((x_) & 0x0F) - ((y_) & 0x0F) - (borrow_); \
    unsigned sbc_b_ = 0; \
    if (sbc_t_ & 0x10u) { sbc_b_ = 1; sbc_t_ = ((sbc_t_ - 0x06) & 0x0F) - 0x10; } \
    sbc_t_ += ((x_) & 0xF0) - ((y_) & 0xF0); \
    if (sbc_t_ & 0x100u) { sbc_b_ = 1; sbc_t_ -= 0x60; } \
    (out_) = (unsigned)(sbc_t_ & 0xFF); (bout_) = sbc_b_; \
} while (0)
#define SBC16(m_) do { const unsigned sbc_m_ = (m_); \
    const unsigned sbc_l_ = a_ - sbc_m_ - ((p_ & 1) ^ 1); \
    unsigned sbc_r_, sbc_nb_; \
    if (p_ & SNES_P_D) { left++; unsigned sbc_lo_, sbc_hi_, sbc_b_ = (p_ & 1) ^ 1; \
        SBC_DEC_BYTE(a_ & 0xFF, sbc_m_ & 0xFF, sbc_b_, sbc_lo_, sbc_b_); \
        SBC_DEC_BYTE(a_ >> 8, sbc_m_ >> 8, sbc_b_, sbc_hi_, sbc_b_); \
        sbc_r_ = (sbc_hi_ << 8) | sbc_lo_; sbc_nb_ = sbc_b_ ^ 1; \
    } else { sbc_r_ = sbc_l_; sbc_nb_ = ((sbc_l_ >> 16) & 1) ^ 1; } \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_C | SNES_P_N | SNES_P_V)) \
        | ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x8000u)) >> 9) \
        | (sbc_nb_ ? SNES_P_C : 0)); \
    a_ = (uint16_t)sbc_r_; set_zn16(a_); } while (0)
#define CMP8(r_, m_) do { const unsigned t_ = ((r_) & 0xFF) - ((m_) & 0xFF); set_zn8((uint8_t)t_); \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (((t_ >> 8) & 1) ^ 1)); } while (0)
#define CMP16(r_, m_) do { const unsigned t_ = (r_) - (m_); set_zn16((uint16_t)t_); \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (((t_ >> 16) & 1) ^ 1)); } while (0)

/* BIT: the Z from the and, the N and V from the operand's own top bits */
#define BITW(m_, w_) do { const unsigned bit_m_ = (m_); \
    p_ = (uint8_t)((p_ & ~(SNES_P_Z | SNES_P_V | SNES_P_N)) \
        | ((a_ & bit_m_) ? 0 : SNES_P_Z) \
        | (w_ ? ((bit_m_ >> 8) & 0xC0u) : ((bit_m_ & 0x80u) ? SNES_P_N : 0) | ((bit_m_ & 0x40u) ? SNES_P_V : 0))); \
    } while (0)

/* shifts, on the accumulator or in memory, 8 or 16 bits wide */
#define ASL8(m_) (__extension__({ unsigned r_ = (m_) & 0xFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 7)); \
    (uint8_t)(r_ << 1); }))
#define LSR8(m_) (__extension__({ unsigned r_ = (m_) & 0xFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); \
    (uint8_t)(r_ >> 1); }))
#define ROL8(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 7)); (uint8_t)((r_ << 1) | c_); }))
#define ROR8(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); (uint8_t)((r_ >> 1) | (c_ ? 0x80 : 0)); }))
#define ASL16(m_) (__extension__({ unsigned r_ = (m_) & 0xFFFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 15)); \
    (uint16_t)(r_ << 1); }))
#define LSR16(m_) (__extension__({ unsigned r_ = (m_) & 0xFFFF; p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); \
    (uint16_t)(r_ >> 1); }))
#define ROL16(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFFFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ >> 15)); (uint16_t)((r_ << 1) | c_); }))
#define ROR16(m_) (__extension__({ const unsigned c_ = p_ & SNES_P_C; unsigned r_ = (m_) & 0xFFFF; \
    p_ = (uint8_t)((p_ & ~SNES_P_C) | (r_ & 1)); (uint16_t)((r_ >> 1) | (c_ ? 0x8000 : 0)); }))

/* the interrupt sequence, in the interpreter's locals (the dispatcher has
 * its own, on the machine's fields) */
#define INTERRUPT(vector) do { \
    if (!e_) PUSH(k_); \
    PUSH((uint8_t)(pc_ >> 8)); PUSH((uint8_t)pc_); \
    PUSH((uint8_t)(e_ ? ((p_ | 0x20u) & ~SNES_P_X) : p_)); \
    p_ |= SNES_P_I; p_ &= (uint8_t)~SNES_P_D; \
    k_ = 0; \
    if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); \
    pc_ = (uint16_t)(rd((uint32_t)(vector)) | rd((uint32_t)(vector) + 1) << 8); \
} while (0)

/* BRK and COP push the past-signature address and the B bit set */
#define SOFTWARE(vector) do { \
    if (!e_) PUSH(k_); \
    PUSH((uint8_t)(pc_ >> 8)); PUSH((uint8_t)pc_); \
    PUSH((uint8_t)(e_ ? (p_ | 0x30u) : p_)); \
    p_ |= SNES_P_I; p_ &= (uint8_t)~SNES_P_D; \
    k_ = 0; \
    if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); \
    pc_ = (uint16_t)(rd((uint32_t)(vector)) | rd((uint32_t)(vector) + 1) << 8); \
} while (0)

#ifdef SNES_NATIVE
#define SNES_INTERP_FN static void snes_interp
#else
#define SNES_INTERP_FN void snes_run
#endif
SNES_INTERP_FN(snes_cpu_t *cpu, uint32_t until) {
    (void)cpu;
    int32_t budget = (int32_t)(until - machine.cpu.cycles);
    uint16_t pc_ = machine.cpu.pc;
    uint16_t a_ = machine.cpu.a, x_ = machine.cpu.x, y_ = machine.cpu.y;
    uint16_t s_ = machine.cpu.s, d_ = machine.cpu.d;
    uint8_t k_ = machine.cpu.k, dbr_ = machine.cpu.dbr, p_ = machine.cpu.p;
    uint8_t e_ = machine.cpu.e;
    uint32_t left;
    int pc_set; /* the case put an address in pc_ by itself: the step below
        must not move it */
    while (budget > 0) {
        if (machine.cpu.halted) { budget = 0; break; }
        if (machine.cpu.waiting) {
            if (!machine.cpu.nmi_pending && !machine.cpu.irq_line) { budget = 0; break; }
            machine.cpu.waiting = 0;
        }
        pc_set = 0;
        const uint16_t pc0 = pc_;
        if (machine.cpu.nmi_pending) {
            machine.cpu.nmi_pending = 0;
            INTERRUPT(e_ ? 0xFFEA : 0xFFFA);
            budget -= e_ ? 7 : 8;
            SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_);
            continue;
        }
        if (machine.cpu.irq_line && !(p_ & SNES_P_I)) {
            INTERRUPT(e_ ? 0xFFEE : 0xFFFE);
            budget -= e_ ? 7 : 8;
            SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_);
            continue;
        }
        SNES_TRACE_HOOK((uint32_t)k_ << 16 | pc_);
#ifdef SNES_TRACE
        /* the trace's per-instruction cycles: the hot cycles commit only
         * at the loop's exit, the hook reads them stale without this */
        machine.cpu.cycles = (uint32_t)(until - budget);
#endif
        left = 2;
        switch (OP8(0)) {
        /* ---- ORA/AND/EOR/ADC/SBC/LDA/CMP, the A operand ---- */
        /* ORA */
        case 0x09: left = 2 + (MF ? 0 : 1); if (MF) WR_A8(a_ | OP8(1)); else WR_A16(a_ | OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0x05: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_DP(1))); else WR_A16(a_ | RDW(A_DP(1))); pc_ += 2; break;
        case 0x15: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_DPX(1))); else WR_A16(a_ | RDW(A_DPX(1))); pc_ += 2; break;
        case 0x0D: left = 4 + (MF ? 0 : 1); if (MF) WR_A8(a_ | rd(A_ABS(1))); else WR_A16(a_ | RDW(A_ABS(1))); pc_ += 3; break;
        case 0x1D: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) WR_A8(a_ | rd(A_ABSX(1))); else WR_A16(a_ | RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0x19: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) WR_A8(a_ | rd(A_ABSY(1))); else WR_A16(a_ | RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0x12: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_INDP(1))); else WR_A16(a_ | RDW(A_INDP(1))); pc_ += 2; break;
        case 0x01: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_INDPX(1))); else WR_A16(a_ | RDW(A_INDPX(1))); pc_ += 2; break;
        case 0x11: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ | rd(A_INDPY(1))); else WR_A16(a_ | RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0x07: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_INDLP(1))); else WR_A16(a_ | RD24(A_INDLP(1))); pc_ += 2; break;
        case 0x17: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_INDLPY(1))); else WR_A16(a_ | RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0x03: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ | rd(A_SR(1))); else WR_A16(a_ | RDW(A_SR(1))); pc_ += 2; break;
        case 0x13: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ | rd(A_SRIY(1))); else WR_A16(a_ | RDW(A_SRIY(1))); pc_ += 2; break; }
        /* ---- the long forms: the operand itself is the 24-bit address ---- */
        case 0x0F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ | rd(A_LONG(1))); else WR_A16(a_ | RD24(A_LONG(1))); pc_ += 4; break;
        case 0x1F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ | rd(A_LONGX(1))); else WR_A16(a_ | RD24(A_LONGX(1))); pc_ += 4; break;
        case 0x2F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ & rd(A_LONG(1))); else WR_A16(a_ & RD24(A_LONG(1))); pc_ += 4; break;
        case 0x3F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ & rd(A_LONGX(1))); else WR_A16(a_ & RD24(A_LONGX(1))); pc_ += 4; break;
        case 0x4F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ ^ rd(A_LONG(1))); else WR_A16(a_ ^ RD24(A_LONG(1))); pc_ += 4; break;
        case 0x5F: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(a_ ^ rd(A_LONGX(1))); else WR_A16(a_ ^ RD24(A_LONGX(1))); pc_ += 4; break;
        case 0x6F: left = 5 + (MF ? 0 : 1); if (MF) ADC8(rd(A_LONG(1))); else ADC16(RD24(A_LONG(1))); pc_ += 4; break;
        case 0x7F: left = 5 + (MF ? 0 : 1); if (MF) ADC8(rd(A_LONGX(1))); else ADC16(RD24(A_LONGX(1))); pc_ += 4; break;
        case 0xEF: left = 5 + (MF ? 0 : 1); if (MF) SBC8(rd(A_LONG(1))); else SBC16(RD24(A_LONG(1))); pc_ += 4; break;
        case 0xFF: left = 5 + (MF ? 0 : 1); if (MF) SBC8(rd(A_LONGX(1))); else SBC16(RD24(A_LONGX(1))); pc_ += 4; break;
        case 0xCF: left = 5 + (MF ? 0 : 1); if (MF) CMP8(a_, rd(A_LONG(1))); else CMP16(a_, RD24(A_LONG(1))); pc_ += 4; break;
        case 0xDF: left = 5 + (MF ? 0 : 1); if (MF) CMP8(a_, rd(A_LONGX(1))); else CMP16(a_, RD24(A_LONGX(1))); pc_ += 4; break;
        case 0xAF: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(rd(A_LONG(1))); else WR_A16(RD24(A_LONG(1))); pc_ += 4; break;
        case 0xBF: left = 5 + (MF ? 0 : 1); if (MF) WR_A8(rd(A_LONGX(1))); else WR_A16(RD24(A_LONGX(1))); pc_ += 4; break;
        case 0x8F: left = 5 + (MF ? 0 : 1); if (MF) wr(A_LONG(1), (uint8_t)a_); else WR24(A_LONG(1), a_); pc_ += 4; break;
        case 0x9F: left = 5 + (MF ? 0 : 1); if (MF) wr(A_LONGX(1), (uint8_t)a_); else WR24(A_LONGX(1), a_); pc_ += 4; break;
        /* AND */
        case 0x29: left = 2 + (MF ? 0 : 1); if (MF) WR_A8(a_ & OP8(1)); else WR_A16(a_ & OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0x25: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_DP(1))); else WR_A16(a_ & RDW(A_DP(1))); pc_ += 2; break;
        case 0x35: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_DPX(1))); else WR_A16(a_ & RDW(A_DPX(1))); pc_ += 2; break;
        case 0x2D: left = 4 + (MF ? 0 : 1); if (MF) WR_A8(a_ & rd(A_ABS(1))); else WR_A16(a_ & RDW(A_ABS(1))); pc_ += 3; break;
        case 0x3D: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) WR_A8(a_ & rd(A_ABSX(1))); else WR_A16(a_ & RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0x39: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) WR_A8(a_ & rd(A_ABSY(1))); else WR_A16(a_ & RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0x32: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_INDP(1))); else WR_A16(a_ & RDW(A_INDP(1))); pc_ += 2; break;
        case 0x21: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_INDPX(1))); else WR_A16(a_ & RDW(A_INDPX(1))); pc_ += 2; break;
        case 0x31: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ & rd(A_INDPY(1))); else WR_A16(a_ & RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0x27: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_INDLP(1))); else WR_A16(a_ & RD24(A_INDLP(1))); pc_ += 2; break;
        case 0x37: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_INDLPY(1))); else WR_A16(a_ & RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0x23: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ & rd(A_SR(1))); else WR_A16(a_ & RDW(A_SR(1))); pc_ += 2; break;
        case 0x33: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ & rd(A_SRIY(1))); else WR_A16(a_ & RDW(A_SRIY(1))); pc_ += 2; break; }
        /* EOR */
        case 0x49: left = 2 + (MF ? 0 : 1); if (MF) WR_A8(a_ ^ OP8(1)); else WR_A16(a_ ^ OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0x45: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_DP(1))); else WR_A16(a_ ^ RDW(A_DP(1))); pc_ += 2; break;
        case 0x55: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_DPX(1))); else WR_A16(a_ ^ RDW(A_DPX(1))); pc_ += 2; break;
        case 0x4D: left = 4 + (MF ? 0 : 1); if (MF) WR_A8(a_ ^ rd(A_ABS(1))); else WR_A16(a_ ^ RDW(A_ABS(1))); pc_ += 3; break;
        case 0x5D: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) WR_A8(a_ ^ rd(A_ABSX(1))); else WR_A16(a_ ^ RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0x59: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) WR_A8(a_ ^ rd(A_ABSY(1))); else WR_A16(a_ ^ RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0x52: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_INDP(1))); else WR_A16(a_ ^ RDW(A_INDP(1))); pc_ += 2; break;
        case 0x41: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_INDPX(1))); else WR_A16(a_ ^ RDW(A_INDPX(1))); pc_ += 2; break;
        case 0x51: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ ^ rd(A_INDPY(1))); else WR_A16(a_ ^ RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0x47: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_INDLP(1))); else WR_A16(a_ ^ RD24(A_INDLP(1))); pc_ += 2; break;
        case 0x57: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_INDLPY(1))); else WR_A16(a_ ^ RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0x43: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(a_ ^ rd(A_SR(1))); else WR_A16(a_ ^ RDW(A_SR(1))); pc_ += 2; break;
        case 0x53: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(a_ ^ rd(A_SRIY(1))); else WR_A16(a_ ^ RDW(A_SRIY(1))); pc_ += 2; break; }
        /* ADC */
        case 0x69: left = 2 + (MF ? 0 : 1); if (MF) ADC8(OP8(1)); else ADC16(OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0x65: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_DP(1))); else ADC16(RDW(A_DP(1))); pc_ += 2; break;
        case 0x75: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_DPX(1))); else ADC16(RDW(A_DPX(1))); pc_ += 2; break;
        case 0x6D: left = 4 + (MF ? 0 : 1); if (MF) ADC8(rd(A_ABS(1))); else ADC16(RDW(A_ABS(1))); pc_ += 3; break;
        case 0x7D: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) ADC8(rd(A_ABSX(1))); else ADC16(RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0x79: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) ADC8(rd(A_ABSY(1))); else ADC16(RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0x72: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_INDP(1))); else ADC16(RDW(A_INDP(1))); pc_ += 2; break;
        case 0x61: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_INDPX(1))); else ADC16(RDW(A_INDPX(1))); pc_ += 2; break;
        case 0x71: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) ADC8(rd(A_INDPY(1))); else ADC16(RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0x67: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_INDLP(1))); else ADC16(RD24(A_INDLP(1))); pc_ += 2; break;
        case 0x77: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_INDLPY(1))); else ADC16(RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0x63: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) ADC8(rd(A_SR(1))); else ADC16(RDW(A_SR(1))); pc_ += 2; break;
        case 0x73: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) ADC8(rd(A_SRIY(1))); else ADC16(RDW(A_SRIY(1))); pc_ += 2; break; }
        /* SBC */
        case 0xE9: left = 2 + (MF ? 0 : 1); if (MF) SBC8(OP8(1)); else SBC16(OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0xE5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_DP(1))); else SBC16(RDW(A_DP(1))); pc_ += 2; break;
        case 0xF5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_DPX(1))); else SBC16(RDW(A_DPX(1))); pc_ += 2; break;
        case 0xED: left = 4 + (MF ? 0 : 1); if (MF) SBC8(rd(A_ABS(1))); else SBC16(RDW(A_ABS(1))); pc_ += 3; break;
        case 0xFD: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) SBC8(rd(A_ABSX(1))); else SBC16(RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0xF9: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) SBC8(rd(A_ABSY(1))); else SBC16(RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0xF2: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_INDP(1))); else SBC16(RDW(A_INDP(1))); pc_ += 2; break;
        case 0xE1: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_INDPX(1))); else SBC16(RDW(A_INDPX(1))); pc_ += 2; break;
        case 0xF1: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) SBC8(rd(A_INDPY(1))); else SBC16(RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0xE7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_INDLP(1))); else SBC16(RD24(A_INDLP(1))); pc_ += 2; break;
        case 0xF7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_INDLPY(1))); else SBC16(RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0xE3: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) SBC8(rd(A_SR(1))); else SBC16(RDW(A_SR(1))); pc_ += 2; break;
        case 0xF3: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) SBC8(rd(A_SRIY(1))); else SBC16(RDW(A_SRIY(1))); pc_ += 2; break; }
        /* CMP */
        case 0xC9: left = 2 + (MF ? 0 : 1); if (MF) CMP8(a_, OP8(1)); else CMP16(a_, OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0xC5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_DP(1))); else CMP16(a_, RDW(A_DP(1))); pc_ += 2; break;
        case 0xD5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_DPX(1))); else CMP16(a_, RDW(A_DPX(1))); pc_ += 2; break;
        case 0xCD: left = 4 + (MF ? 0 : 1); if (MF) CMP8(a_, rd(A_ABS(1))); else CMP16(a_, RDW(A_ABS(1))); pc_ += 3; break;
        case 0xDD: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) CMP8(a_, rd(A_ABSX(1))); else CMP16(a_, RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0xD9: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) CMP8(a_, rd(A_ABSY(1))); else CMP16(a_, RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0xD2: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_INDP(1))); else CMP16(a_, RDW(A_INDP(1))); pc_ += 2; break;
        case 0xC1: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_INDPX(1))); else CMP16(a_, RDW(A_INDPX(1))); pc_ += 2; break;
        case 0xD1: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) CMP8(a_, rd(A_INDPY(1))); else CMP16(a_, RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0xC7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_INDLP(1))); else CMP16(a_, RD24(A_INDLP(1))); pc_ += 2; break;
        case 0xD7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_INDLPY(1))); else CMP16(a_, RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0xC3: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) CMP8(a_, rd(A_SR(1))); else CMP16(a_, RDW(A_SR(1))); pc_ += 2; break;
        case 0xD3: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) CMP8(a_, rd(A_SRIY(1))); else CMP16(a_, RDW(A_SRIY(1))); pc_ += 2; break; }
        /* ---- LDA ---- */
        case 0xA9: left = 2 + (MF ? 0 : 1); if (MF) WR_A8(OP8(1)); else WR_A16(OP16(1)); pc_ += MF ? 2 : 3; break;
        case 0xA5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_DP(1))); else WR_A16(RDW(A_DP(1))); pc_ += 2; break;
        case 0xB5: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_DPX(1))); else WR_A16(RDW(A_DPX(1))); pc_ += 2; break;
        case 0xAD: left = 4 + (MF ? 0 : 1); if (MF) WR_A8(rd(A_ABS(1))); else WR_A16(RDW(A_ABS(1))); pc_ += 3; break;
        case 0xBD: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            if (MF) WR_A8(rd(A_ABSX(1))); else WR_A16(RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0xB9: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, y_);
            if (MF) WR_A8(rd(A_ABSY(1))); else WR_A16(RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0xB2: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_INDP(1))); else WR_A16(RDW(A_INDP(1))); pc_ += 2; break;
        case 0xA1: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_INDPX(1))); else WR_A16(RDW(A_INDPX(1))); pc_ += 2; break;
        case 0xB1: { const unsigned w_ = RDW(A_DP(1)); left = 5 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(rd(A_INDPY(1))); else WR_A16(RDW(A_INDPY(1))); pc_ += 2; break; }
        case 0xA7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_INDLP(1))); else WR_A16(RD24(A_INDLP(1))); pc_ += 2; break;
        case 0xB7: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_INDLPY(1))); else WR_A16(RD24(A_INDLPY(1))); pc_ += 2; break;
        case 0xA3: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) WR_A8(rd(A_SR(1))); else WR_A16(RDW(A_SR(1))); pc_ += 2; break;
        case 0xB3: { const unsigned w_ = RDW(A_SR(1)); left = 7 + (MF ? 0 : 1) + DPPEN + CROSS(w_, y_);
            if (MF) WR_A8(rd(A_SRIY(1))); else WR_A16(RDW(A_SRIY(1))); pc_ += 2; break; }
        /* ---- STA ---- */
        case 0x85: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_DP(1), (uint8_t)a_); else WRW(A_DP(1), a_); pc_ += 2; break;
        case 0x95: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_DPX(1), (uint8_t)a_); else WRW(A_DPX(1), a_); pc_ += 2; break;
        case 0x8D: left = 4 + (MF ? 0 : 1); if (MF) wr(A_ABS(1), (uint8_t)a_); else WRW(A_ABS(1), a_); pc_ += 3; break;
        case 0x9D: left = 5 + (MF ? 0 : 1); if (MF) wr(A_ABSX(1), (uint8_t)a_); else WRW(A_ABSX(1), a_); pc_ += 3; break;
        case 0x99: left = 5 + (MF ? 0 : 1); if (MF) wr(A_ABSY(1), (uint8_t)a_); else WRW(A_ABSY(1), a_); pc_ += 3; break;
        case 0x92: left = 5 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_INDP(1), (uint8_t)a_); else WRW(A_INDP(1), a_); pc_ += 2; break;
        case 0x81: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_INDPX(1), (uint8_t)a_); else WRW(A_INDPX(1), a_); pc_ += 2; break;
        case 0x91: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_INDPY(1), (uint8_t)a_); else WRW(A_INDPY(1), a_); pc_ += 2; break;
        case 0x87: left = 6 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_INDLP(1), (uint8_t)a_); else WR24(A_INDLP(1), a_); pc_ += 2; break;
        case 0x97: left = 7 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_INDLPY(1), (uint8_t)a_); else WR24(A_INDLPY(1), a_); pc_ += 2; break;
        case 0x83: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_SR(1), (uint8_t)a_); else WRW(A_SR(1), a_); pc_ += 2; break;
        case 0x93: left = 7 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_SRIY(1), (uint8_t)a_); else WRW(A_SRIY(1), a_); pc_ += 2; break;
        /* ---- LDX/LDY/STX/STY/CPX/CPY ---- */
        case 0xA2: left = 2 + (XF ? 0 : 1); WR_X(XF ? OP8(1) : OP16(1)); pc_ += XF ? 2 : 3; break;
        case 0xA6: left = 4 + (XF ? 0 : 1) + DPPEN; WR_X(XF ? rd(A_DP(1)) : RDW(A_DP(1))); pc_ += 2; break;
        case 0xB6: left = 4 + (XF ? 0 : 1) + DPPEN; WR_X(XF ? rd(A_DPY(1)) : RDW(A_DPY(1))); pc_ += 2; break;
        case 0xAE: left = 4 + (XF ? 0 : 1); WR_X(XF ? rd(A_ABS(1)) : RDW(A_ABS(1))); pc_ += 3; break;
        case 0xBE: { const unsigned b_ = OP16(1); left = 4 + CROSS(b_, y_);
            WR_X(XF ? rd(A_ABSY(1)) : RDW(A_ABSY(1))); pc_ += 3; break; }
        case 0xA0: left = 2 + (XF ? 0 : 1); WR_Y(XF ? OP8(1) : OP16(1)); pc_ += XF ? 2 : 3; break;
        case 0xA4: left = 4 + (XF ? 0 : 1) + DPPEN; WR_Y(XF ? rd(A_DP(1)) : RDW(A_DP(1))); pc_ += 2; break;
        case 0xB4: left = 4 + (XF ? 0 : 1) + DPPEN; WR_Y(XF ? rd(A_DPX(1)) : RDW(A_DPX(1))); pc_ += 2; break;
        case 0xAC: left = 4 + (XF ? 0 : 1); WR_Y(XF ? rd(A_ABS(1)) : RDW(A_ABS(1))); pc_ += 3; break;
        case 0xBC: { const unsigned b_ = OP16(1); left = 4 + CROSS(b_, x_);
            WR_Y(XF ? rd(A_ABSX(1)) : RDW(A_ABSX(1))); pc_ += 3; break; }
        case 0x86: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) wr(A_DP(1), (uint8_t)x_); else WRW(A_DP(1), x_); pc_ += 2; break;
        case 0x96: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) wr(A_DPY(1), (uint8_t)x_); else WRW(A_DPY(1), x_); pc_ += 2; break;
        case 0x8E: left = 4 + (XF ? 0 : 1); if (XF) wr(A_ABS(1), (uint8_t)x_); else WRW(A_ABS(1), x_); pc_ += 3; break;
        case 0x84: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) wr(A_DP(1), (uint8_t)y_); else WRW(A_DP(1), y_); pc_ += 2; break;
        case 0x94: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) wr(A_DPX(1), (uint8_t)y_); else WRW(A_DPX(1), y_); pc_ += 2; break;
        case 0x8C: left = 4 + (XF ? 0 : 1); if (XF) wr(A_ABS(1), (uint8_t)y_); else WRW(A_ABS(1), y_); pc_ += 3; break;
        case 0xE0: left = 2 + (XF ? 0 : 1); if (XF) CMP8(x_, OP8(1)); else CMP16(x_, OP16(1)); pc_ += XF ? 2 : 3; break;
        case 0xE4: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) CMP8(x_, rd(A_DP(1))); else CMP16(x_, RDW(A_DP(1))); pc_ += 2; break;
        case 0xEC: left = 4 + (XF ? 0 : 1); if (XF) CMP8(x_, rd(A_ABS(1))); else CMP16(x_, RDW(A_ABS(1))); pc_ += 3; break;
        case 0xC0: left = 2 + (XF ? 0 : 1); if (XF) CMP8(y_, OP8(1)); else CMP16(y_, OP16(1)); pc_ += XF ? 2 : 3; break;
        case 0xC4: left = 4 + (XF ? 0 : 1) + DPPEN; if (XF) CMP8(y_, rd(A_DP(1))); else CMP16(y_, RDW(A_DP(1))); pc_ += 2; break;
        case 0xCC: left = 4 + (XF ? 0 : 1); if (XF) CMP8(y_, rd(A_ABS(1))); else CMP16(y_, RDW(A_ABS(1))); pc_ += 3; break;
        /* ---- STZ ---- */
        case 0x64: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_DP(1), 0); else WRW(A_DP(1), 0); pc_ += 2; break;
        case 0x74: left = 4 + (MF ? 0 : 1) + DPPEN; if (MF) wr(A_DPX(1), 0); else WRW(A_DPX(1), 0); pc_ += 2; break;
        case 0x9C: left = 4 + (MF ? 0 : 1); if (MF) wr(A_ABS(1), 0); else WRW(A_ABS(1), 0); pc_ += 3; break;
        case 0x9E: left = 5 + (MF ? 0 : 1); if (MF) wr(A_ABSX(1), 0); else WRW(A_ABSX(1), 0); pc_ += 3; break;
        /* ---- BIT ---- */
        case 0x89: { const unsigned m_ = MF ? OP8(1) : OP16(1); left = 2 + (MF ? 0 : 1);
            p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((a_ & m_) ? 0 : SNES_P_Z)); pc_ += MF ? 2 : 3; break; }
        case 0x24: left = 4 + (MF ? 0 : 1) + DPPEN; BITW(MF ? rd(A_DP(1)) : RDW(A_DP(1)), !MF); pc_ += 2; break;
        case 0x34: left = 4 + (MF ? 0 : 1) + DPPEN; BITW(MF ? rd(A_DPX(1)) : RDW(A_DPX(1)), !MF); pc_ += 2; break;
        case 0x2C: left = 4 + (MF ? 0 : 1); BITW(MF ? rd(A_ABS(1)) : RDW(A_ABS(1)), !MF); pc_ += 3; break;
        case 0x3C: { const unsigned b_ = OP16(1); left = 4 + (MF ? 0 : 1) + CROSS(b_, x_);
            BITW(MF ? rd(A_ABSX(1)) : RDW(A_ABSX(1)), !MF); pc_ += 3; break; }
        /* ---- shifts and RMW, in memory ---- */
        case 0x06: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ASL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ASL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x16: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ASL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ASL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x0E: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ASL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ASL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x1E: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ASL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ASL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x46: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = LSR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = LSR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x56: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = LSR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = LSR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x4E: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = LSR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = LSR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x5E: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = LSR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = LSR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x26: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ROL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x36: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ROL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x2E: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ROL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x3E: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ROL8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROL16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x66: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ROR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x76: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = ROR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0x6E: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ROR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0x7E: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = ROR8(rd(e_)); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = ROR16(RDW(e_)); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0xE6: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) + 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) + 1); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0xF6: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) + 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) + 1); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0xEE: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) + 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) + 1); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0xFE: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) + 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) + 1); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0xC6: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) - 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) - 1); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0xD6: { const uint32_t e_ = A_DPX(1); left = 6 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) - 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) - 1); WRW(e_, r_); set_zn16(r_); } pc_ += 2; break; }
        case 0xCE: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) - 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) - 1); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        case 0xDE: { const uint32_t e_ = A_ABSX(1); left = 7 + (MF ? 0 : 1);
            if (MF) { const uint8_t r_ = (uint8_t)(rd(e_) - 1); wr(e_, r_); set_zn8(r_); }
            else { const uint16_t r_ = (uint16_t)(RDW(e_) - 1); WRW(e_, r_); set_zn16(r_); } pc_ += 3; break; }
        /* TSB/TRB */
        case 0x04: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t m_ = rd(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                wr(e_, (uint8_t)(m_ | (uint8_t)a_)); }
            else { const uint16_t m_ = RDW(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                WRW(e_, (uint16_t)(m_ | a_)); } pc_ += 2; break; }
        case 0x0C: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t m_ = rd(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                wr(e_, (uint8_t)(m_ | (uint8_t)a_)); }
            else { const uint16_t m_ = RDW(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                WRW(e_, (uint16_t)(m_ | a_)); } pc_ += 3; break; }
        case 0x14: { const uint32_t e_ = A_DP(1); left = 5 + (MF ? 0 : 1) + DPPEN;
            if (MF) { const uint8_t m_ = rd(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                wr(e_, (uint8_t)(m_ & (uint8_t)~a_)); }
            else { const uint16_t m_ = RDW(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                WRW(e_, (uint16_t)(m_ & (uint16_t)~a_)); } pc_ += 2; break; }
        case 0x1C: { const uint32_t e_ = A_ABS(1); left = 6 + (MF ? 0 : 1);
            if (MF) { const uint8_t m_ = rd(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                wr(e_, (uint8_t)(m_ & (uint8_t)~a_)); }
            else { const uint16_t m_ = RDW(e_); p_ = (uint8_t)((p_ & ~(SNES_P_Z)) | ((m_ & a_) ? 0 : SNES_P_Z));
                WRW(e_, (uint16_t)(m_ & (uint16_t)~a_)); } pc_ += 3; break; }
        /* ---- the accumulator's own shifts, INC/DEC A ---- */
        case 0x0A: left = 2 + (MF ? 0 : 1); if (MF) { const uint8_t r_ = ASL8(a_); a_ = (a_ & 0xFF00u) | r_; set_zn8(r_); }
            else { a_ = ASL16(a_); set_zn16(a_); } break;
        case 0x4A: left = 2 + (MF ? 0 : 1); if (MF) { const uint8_t r_ = LSR8(a_); a_ = (a_ & 0xFF00u) | r_; set_zn8(r_); }
            else { a_ = LSR16(a_); set_zn16(a_); } break;
        case 0x2A: left = 2 + (MF ? 0 : 1); if (MF) { const uint8_t r_ = ROL8(a_); a_ = (a_ & 0xFF00u) | r_; set_zn8(r_); }
            else { a_ = ROL16(a_); set_zn16(a_); } break;
        case 0x6A: left = 2 + (MF ? 0 : 1); if (MF) { const uint8_t r_ = ROR8(a_); a_ = (a_ & 0xFF00u) | r_; set_zn8(r_); }
            else { a_ = ROR16(a_); set_zn16(a_); } break;
        case 0x1A: left = 2 + (MF ? 0 : 1); if (MF) WR_A8((uint8_t)(a_ + 1)); else WR_A16((uint16_t)(a_ + 1)); break;
        case 0x3A: left = 2 + (MF ? 0 : 1); if (MF) WR_A8((uint8_t)(a_ - 1)); else WR_A16((uint16_t)(a_ - 1)); break;
        case 0xE8: left = 2; if (XF) { x_ = (uint16_t)((x_ + 1) & 0xFF); set_zn8((uint8_t)x_); }
            else { x_++; set_zn16(x_); } break;
        case 0xCA: left = 2; if (XF) { x_ = (uint16_t)((x_ - 1) & 0xFF); set_zn8((uint8_t)x_); }
            else { x_--; set_zn16(x_); } break;
        case 0xC8: left = 2; if (XF) { y_ = (uint16_t)((y_ + 1) & 0xFF); set_zn8((uint8_t)y_); }
            else { y_++; set_zn16(y_); } break;
        case 0x88: left = 2; if (XF) { y_ = (uint16_t)((y_ - 1) & 0xFF); set_zn8((uint8_t)y_); }
            else { y_--; set_zn16(y_); } break;
        /* ---- transfers, XBA ---- */
        case 0xAA: left = 2; WR_X(MF ? (a_ & 0xFF) : a_); break;
        case 0xA8: left = 2; WR_Y(MF ? (a_ & 0xFF) : a_); break;
        case 0x8A: left = 2; if (MF) WR_A8(x_); else { a_ = x_; set_zn16(a_); } break;
        case 0x98: left = 2; if (MF) WR_A8(y_); else { a_ = y_; set_zn16(a_); } break;
        case 0xBA: left = 2; WR_X(s_); break;
        case 0x9A: left = 2; s_ = XF ? (uint16_t)((s_ & 0xFF00u) | (x_ & 0xFF)) : x_;
            if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); break;
        case 0x5B: left = 2; d_ = a_; set_zn16(d_); break;                       /* TCD */
        case 0x7B: left = 2; a_ = d_; set_zn16(a_); break;                       /* TDC */
        case 0x1B: left = 2; s_ = a_; if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); break; /* TCS */
        case 0x3B: left = 2; a_ = s_; set_zn16(a_); break;                       /* TSC */
        case 0x9B: left = 2; WR_Y(x_); break; /* TXY */
        case 0xBB: left = 2; WR_X(y_); break; /* TYX */
        case 0xEB: left = 3; a_ = (uint16_t)((a_ >> 8) | (a_ << 8)); set_zn8((uint8_t)a_); break; /* XBA */
        /* ---- flags ---- */
        case 0x18: left = 2; p_ &= (uint8_t)~SNES_P_C; break;
        case 0x38: left = 2; p_ |= SNES_P_C; break;
        case 0x58: left = 2; p_ &= (uint8_t)~SNES_P_I; break;
        case 0x78: left = 2; p_ |= SNES_P_I; break;
        case 0xB8: left = 2; p_ &= (uint8_t)~SNES_P_V; break;
        case 0xD8: left = 2; p_ &= (uint8_t)~SNES_P_D; break;
        case 0xF8: left = 2; p_ |= SNES_P_D; break;
        case 0xC2: { const uint8_t m_ = OP8(1); left = 3; pc_ += 2; /* REP */
            p_ &= (uint8_t)~m_; if (e_) p_ |= SNES_P_M | SNES_P_X; break; }
        case 0xE2: { const uint8_t m_ = OP8(1); left = 3; pc_ += 2; /* SEP */
            p_ |= m_; if (e_) p_ |= SNES_P_M | SNES_P_X;
            if (m_ & SNES_P_X) { x_ &= 0xFF; y_ &= 0xFF; } break; }
        case 0xFB: { const unsigned c_ = p_ & SNES_P_C; left = 2; /* XCE */
            p_ = (uint8_t)((p_ & ~SNES_P_C) | (e_ ? SNES_P_C : 0));
            e_ = c_ ? 1 : 0;
            if (e_) { p_ |= SNES_P_M | SNES_P_X; x_ &= 0xFF; y_ &= 0xFF;
                s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); }
            break; }
        case 0xEA: break; /* NOP */
        case 0x42: pc_ += 2; break; /* WDM: the reserved byte, a two-byte NOP */
        /* ---- stack ---- */
        case 0x48: left = 3 + (MF ? 0 : 1); if (MF) PUSH((uint8_t)a_); else { PUSH((uint8_t)(a_ >> 8)); PUSH((uint8_t)a_); } break;
        case 0xDA: left = 3 + (XF ? 0 : 1); if (XF) PUSH((uint8_t)x_); else PUSH16(x_); break;
        case 0x5A: left = 3 + (XF ? 0 : 1); if (XF) PUSH((uint8_t)y_); else PUSH16(y_); break;
        case 0x08: left = 3; PUSH(p_); break; /* PHP: the register as it is — in the
            emulation mode its M and X bits stand in for the pushed 1s */
        case 0x8B: left = 3; PUSH(dbr_); break;  /* PHB */
        case 0x0B: left = 4; PUSH((uint8_t)(d_ >> 8)); PUSH((uint8_t)d_); break; /* PHD */
        case 0x4B: left = 3; PUSH(k_); break;    /* PHK */
        case 0x68: left = 4 + (MF ? 0 : 1); if (MF) WR_A8(POP());
            else { const unsigned w_ = POP16(); a_ = (uint16_t)w_; set_zn16(a_); } break; /* PLA */
        case 0xFA: left = 4 + (XF ? 0 : 1); if (XF) WR_X(POP()); else { x_ = (uint16_t)POP16(); set_zn16(x_); } break; /* PLX */
        case 0x7A: left = 4 + (XF ? 0 : 1); if (XF) WR_Y(POP()); else { y_ = (uint16_t)POP16(); set_zn16(y_); } break; /* PLY */
        case 0x28: { const uint8_t v_ = POP(); left = 4; p_ = e_ ? (uint8_t)(v_ | 0x30u) : v_; /* PLP */
            if (XF) { x_ &= 0xFF; y_ &= 0xFF; } break; }
        case 0xAB: left = 4; dbr_ = POP(); set_zn8((uint8_t)dbr_); break; /* PLB */
        case 0x2B: left = 5; d_ = (uint16_t)POP16(); set_zn16(d_); break; /* PLD */
        case 0xF4: left = 5; PUSH16(OP16(1)); pc_ += 3; break; /* PEA */
        case 0xD4: left = 6 + DPPEN; { const uint32_t q_ = A_DP(1); PUSH16(rd(q_) | rd(WRAP16(q_)) << 8); } pc_ += 2; break; /* PEI */
        case 0x62: left = 6; { const unsigned t_ = OP16(1); PUSH16((uint16_t)(pc_ + 3 + (int16_t)t_)); } pc_ += 3; break; /* PER */
        /* ---- jumps, calls, returns ---- */
        case 0x4C: pc_ = (uint16_t)OP16(1); left = 3; pc_set = 1; break;
        case 0x6C: { const uint16_t ptr_ = (uint16_t)OP16(1); /* the pointer in the program bank, the 6502's page-wrap bug kept */
            pc_ = (uint16_t)(rd((uint32_t)k_ << 16 | ptr_) |
                             rd((uint32_t)k_ << 16 | (uint16_t)((ptr_ & 0xFF00u) | ((ptr_ + 1) & 0xFFu))) << 8);
            left = 5; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x7C: { const uint16_t ptr_ = (uint16_t)(OP16(1) + (XF ? (x_ & 0xFF) : x_));
            pc_ = (uint16_t)(rd((uint32_t)k_ << 16 | ptr_) |
                             rd((uint32_t)k_ << 16 | (uint16_t)(ptr_ + 1)) << 8);
            left = 6; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0xDC: { const uint16_t ptr_ = (uint16_t)OP16(1);
            const unsigned lo_ = rd((uint32_t)k_ << 16 | ptr_),
                           hi_ = rd((uint32_t)k_ << 16 | (uint16_t)(ptr_ + 1));
            k_ = rd((uint32_t)k_ << 16 | (uint16_t)(ptr_ + 2));
            pc_ = (uint16_t)(lo_ | hi_ << 8); /* JMP [abs], the pointer in the program bank */
            left = 6; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x5C: { const uint32_t dest_ = OP24(1); k_ = (uint8_t)(dest_ >> 16); pc_ = (uint16_t)dest_; /* JML */
            left = 4; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x20: { const unsigned lo_ = OP8(1); PUSH16(pc_ + 2);
            pc_ = (uint16_t)(lo_ | OP8(2) << 8); left = 6; pc_set = 1; break; }
        case 0xFC: { const uint16_t ptr_ = (uint16_t)(OP16(1) + (XF ? (x_ & 0xFF) : x_));
            const unsigned lo_ = rd((uint32_t)dbr_ << 16 | ptr_);
            PUSH16(pc_ + 3);
            pc_ = (uint16_t)(lo_ | rd((uint32_t)dbr_ << 16 | (uint16_t)(ptr_ + 1)) << 8);
            left = 8; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x22: { const uint32_t dest_ = OP24(1); PUSH(k_); PUSH16(pc_ + 3); /* JSL */
            k_ = (uint8_t)(dest_ >> 16); pc_ = (uint16_t)dest_; left = 8; pc_set = 1; break; }
        case 0x60: { const unsigned w_ = POP16(); pc_ = (uint16_t)(w_ + 1); left = 6; pc_set = 1;
            SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; } /* RTS */
        case 0x6B: { const unsigned w_ = POP16(); k_ = POP(); pc_ = (uint16_t)(w_ + 1);
            left = 6; pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; } /* RTL */
        case 0x40: { const uint8_t v_ = POP(); left = e_ ? 6 : 7; p_ = e_ ? (uint8_t)(v_ | 0x30u) : v_; /* RTI */
            if (XF) { x_ &= 0xFF; y_ &= 0xFF; }
            const unsigned w_ = POP16(); pc_ = (uint16_t)w_;
            if (!e_) k_ = POP();
            pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x00: { pc_ += 2; left = e_ ? 7 : 8; SOFTWARE(e_ ? 0xFFE6 : 0xFFF6); /* BRK */
            pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x02: { pc_ += 2; left = e_ ? 7 : 8; SOFTWARE(e_ ? 0xFFE4 : 0xFFF4); /* COP */
            pc_set = 1; SNES_DYN_HOOK((uint32_t)k_ << 16 | pc_); break; }
        case 0x80: { const int8_t off_ = (int8_t)OP8(1); /* BRA */
            if (e_) { const uint16_t t_ = (uint16_t)(pc_ + 2 + off_);
                left = 3 + (((pc_ + 2) & 0xFF00u) != (t_ & 0xFF00u) ? 1 : 0); }
            else left = 3;
            pc_ = (uint16_t)(pc_ + 2 + off_); pc_set = 1; break; }
        #define BRANCH(take) do { const int8_t off_ = (int8_t)OP8(1); \
            if (take) { const uint16_t t_ = (uint16_t)(pc_ + 2 + off_); \
                left = e_ ? 3 + (((pc_ + 2) & 0xFF00u) != (t_ & 0xFF00u) ? 1 : 0) : 3; \
                pc_ = t_; } \
            else { left = 2; pc_ = (uint16_t)(pc_ + 2); } \
            pc_set = 1; } while (0)
        case 0x10: BRANCH(!(p_ & SNES_P_N)); break;
        case 0x30: BRANCH(p_ & SNES_P_N); break;
        case 0x50: BRANCH(!(p_ & SNES_P_V)); break;
        case 0x70: BRANCH(p_ & SNES_P_V); break;
        case 0x90: BRANCH(!(p_ & SNES_P_C)); break;
        case 0xB0: BRANCH(p_ & SNES_P_C); break;
        case 0xD0: BRANCH(!(p_ & SNES_P_Z)); break;
        case 0xF0: BRANCH(p_ & SNES_P_Z); break;
        #undef BRANCH
        case 0x82: { const unsigned t_ = OP16(1); pc_ = (uint16_t)(pc_ + 3 + (int16_t)t_);
            left = 3; pc_set = 1; break; } /* BRL */
        /* ---- the block moves: X to Y, A bytes plus one ---- */
        case 0x54: case 0x44: { /* MVN, MVP */
            const uint8_t db_ = OP8(1), sb_ = OP8(2);
            const uint16_t cnt_ = a_; const uint32_t bytes_ = (uint32_t)cnt_ + 1;
            const int back = OP8(0) == 0x44;
            for (uint32_t i_ = 0; i_ < bytes_; i_++) {
                const uint32_t s_at = (uint32_t)sb_ << 16 |
                    (XF ? (uint8_t)(back ? x_ - i_ : x_ + i_) : (uint16_t)(back ? x_ - i_ : x_ + i_));
                const uint32_t d_at = (uint32_t)db_ << 16 |
                    (XF ? (uint8_t)(back ? y_ - i_ : y_ + i_) : (uint16_t)(back ? y_ - i_ : y_ + i_));
                wr(d_at, rd(s_at));
            }
            const uint16_t step_ = (uint16_t)(bytes_ & 0xFFFF);
            if (XF) { x_ = back ? (uint8_t)(x_ - step_) : (uint8_t)(x_ + step_);
                      y_ = back ? (uint8_t)(y_ - step_) : (uint8_t)(y_ + step_); }
            else { x_ = back ? (uint16_t)(x_ - step_) : (uint16_t)(x_ + step_);
                   y_ = back ? (uint16_t)(y_ - step_) : (uint16_t)(y_ + step_); }
            a_ = 0xFFFF; /* the accumulator's done mark */
            dbr_ = db_;
            left = (uint32_t)(7u * bytes_);
            pc_ += 3; break; }
        /* ---- sleeps ---- */
        case 0xCB: /* WAI: wakes on the next interrupt; nothing pending, the
            rest of the slice is spent here (the frame's NMI lands at its
            end, the boundary check takes it next slice) */
            pc_ += 1; pc_set = 1; left = 3; machine.cpu.waiting = 1;
            break;
        case 0xDB: /* STP: stopped until the next reset */
            pc_ += 1; pc_set = 1; left = 3; machine.cpu.halted = 1;
            break;
        default: break;
        }
        if (!pc_set && pc_ == pc0) pc_ = (uint16_t)(pc0 + 1);
        budget -= left;
    }
    machine.cpu.pc = pc_;
    machine.cpu.a = a_;
    machine.cpu.x = x_;
    machine.cpu.y = y_;
    machine.cpu.s = s_;
    machine.cpu.d = d_;
    machine.cpu.k = k_;
    machine.cpu.dbr = dbr_;
    machine.cpu.p = p_;
    machine.cpu.e = e_;
    machine.cpu.cycles = until - (uint32_t)budget;
}

#ifdef SNES_NATIVE
/* Recompiled code (snes_gen.c) in front of the interpreter.  The loop is
 * the interpreter's own — the interrupt lines, then one step — except
 * that a step is a call into the recompiled entry at (K:PC, context)
 * when there is one: it runs until the slice is spent (precisely: after
 * the instruction that spends it; otherwise at its next check), or until
 * a transfer it cannot follow.  The context check makes a changed flag or
 * bank fall back instead of running the wrong bytes.  Anything else —
 * what the budget left out — is interpreted one instruction at a time.
 * snes_native = 0 is the reference (--interp). */
#include "snes_gen.h"

int snes_native = 1;

/* BRA/JMP to themselves in a ROM window, a game waiting for its
 * interrupt: the rest of the slice in laps at once (m0_idle.h), left with
 * pc on the loop as the interpreter would */
static int rom_at(const snes_cpu_t *c, uint32_t at, uint8_t *out) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) return 0;
    if (off < 0x2000 || (off >= 0x2000 && off < 0x8000)) return 0;
    if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0) {
        if (!c->hirom && !c->exhirom) return 0;
    }
    uint32_t fo;
    if (c->exhirom)
        fo = bank >= 0xC0 ? (uint32_t)(bank - 0xC0) * 0x10000u + off
                          : 0x400000u + (uint32_t)(bank & 0x3F) * 0x10000u + off;
    else if (c->hirom)
        fo = (uint32_t)(bank & 0x3F) * 0x10000u + off;
    else
        fo = (uint32_t)(bank & 0x3F) * 0x8000u + (off & 0x7FFFu);
    if (fo >= c->rom_bytes) return 0;
    *out = c->rom[fo];
    return 1;
}

void snes_run(snes_cpu_t *cpu, uint32_t until) {
    if (!snes_native) { snes_interp(cpu, until); return; }
    int32_t budget = (int32_t)(until - cpu->cycles);
    while (budget > 0) {
        if (machine.cpu.halted) { /* STP: nothing but a reset wakes it */
            budget = 0;
            break;
        }
        if (machine.cpu.waiting) {
            if (!machine.cpu.nmi_pending && !machine.cpu.irq_line) { budget = 0; break; }
            machine.cpu.waiting = 0;
        }
        if (machine.cpu.nmi_pending) {
            machine.cpu.nmi_pending = 0;
            snes_take_interrupt(cpu, cpu->e ? 0xFFEA : 0xFFFA);
            budget -= cpu->e ? 7 : 8;
            continue;
        }
        if (machine.cpu.irq_line && !(cpu->p & SNES_P_I)) {
            snes_take_interrupt(cpu, cpu->e ? 0xFFEE : 0xFFFE);
            budget -= cpu->e ? 7 : 8;
            continue;
        }
        uint8_t op = 0xFF;
        const uint32_t at = (uint32_t)cpu->k << 16 | cpu->pc;
        if (rom_at(cpu, at, &op)) {
            uint8_t o1 = 0, o2 = 0;
            rom_at(cpu, at + 1, &o1);
            rom_at(cpu, at + 2, &o2);
            if ((op == 0x80 && o1 == 0xFEu) ||
                (op == 0x4C && (uint16_t)(o1 | o2 << 8) == cpu->pc)) {
                cpu->mdr = op == 0x80 ? o1 : o2;
                budget = SNES_SPIN3(budget);
                break;
            }
        }
        const int entry = snes_gen_lookup(at, snes_ctx_key(cpu));
        if (entry >= 0) {
            budget = snes_gen_enter(entry, budget);
            continue;
        }
        /* one instruction: a budget of one cycle is spent by any of them */
        cpu->cycles = until - (uint32_t)budget;
        snes_interp(cpu, cpu->cycles + 1u);
        budget = (int32_t)(until - cpu->cycles);
    }
    cpu->cycles = until - (uint32_t)budget;
}
#endif
