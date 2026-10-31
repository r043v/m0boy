/* HuC6280 interpreter, after the NES 6502 core's model and the mednafen
 * core's facts: the opcode table, the cycle counts (their LastCycle
 * included — one more than the 6502's table on most memory instructions),
 * and the reset state.  The illegal opcodes are one-byte two-cycle NOPs —
 * no game in the pipeline's scope relies on them.  The T flag makes
 * ADC/AND/ORA/EOR read and write the zero page at X; it clears itself
 * after every instruction but SET, RTI and PLP.  Decimal mode follows the
 * mednafen core, quirks included (the Z flag from the raw nibble sum). */
#if __has_include("pce_config.h") /* a generated project's: PCE_NATIVE */
#include "pce_config.h"
#endif
#include "huc6280.h"

/* the debug tracer's per-instruction hook (the harness builds with
 * -DPCE_TRACE and provides pce_trace_step); the dyn hook marks the
 * addresses a dynamic transfer (RTS, RTI, a jump through a pointer, an
 * interrupt) landed on — the recompiler's entries the static walk cannot
 * see */
#ifdef PCE_TRACE
extern void pce_trace_step(uint16_t pc);
extern void pce_trace_dyn(uint16_t pc);
#define PCE_TRACE_HOOK(pc) pce_trace_step(pc)
#define PCE_DYN_HOOK(pc) pce_trace_dyn(pc)
#else
#define PCE_TRACE_HOOK(pc) ((void)0)
#define PCE_DYN_HOOK(pc) ((void)0)
#endif

/* the hot state — PC, the stack pointer, the registers and the bus's
 * window tables — lives in huc6280_run's locals: a read is a load through
 * a cached page pointer, a compare and a mask.  The macros' locals carry
 * the rd_/wr_ prefix so an argument never shadows a machine register. */
static uint8_t rd_out(huc6280_t *cpu, uint16_t at) {
    const uint8_t *b = cpu->rbase[at >> 13];
    return b ? b[at & 0x1FFFu] : pce_bus_read(at);
}

#define rd(cpu, at) (__extension__({ uint16_t rd_at_ = (at); \
    const uint8_t *rd_b_ = rbase_[rd_at_ >> 13]; \
    rd_b_ ? rd_b_[rd_at_ & 0x1FFFu] : pce_bus_read(rd_at_); }))
#define wr(cpu, at, v) do { uint16_t wr_at_ = (at); uint8_t *wr_b_ = wbase_[wr_at_ >> 13]; \
    if (wr_b_) wr_b_[wr_at_ & 0x1FFFu] = (v); else pce_bus_write(wr_at_, (v)); } while (0)
#define push(cpu, v) do { wr(cpu, (uint16_t)(0x2100u | s_), (v)); s_--; } while (0)
#define pop(cpu) (__extension__({ s_++; rd(cpu, (uint16_t)(0x2100u | s_)); }))

/* addressing modes: the zero page is MPR1's window at $2000; (zp) wraps
 * inside the zero page, there is no page-crossing cost anywhere */
#define ZP_AT(o) ((uint16_t)(0x2000u | (uint8_t)(o)))
#define A_ZP() ZP_AT(rd(cpu, (uint16_t)(pc_ + 1)))
#define A_ZPI(i) ZP_AT((uint8_t)(rd(cpu, (uint16_t)(pc_ + 1)) + (i)))
#define A_ABS() (__extension__({ const unsigned lo_ = rd(cpu, (uint16_t)(pc_ + 1)); \
    const unsigned hi_ = rd(cpu, (uint16_t)(pc_ + 2)); \
    (uint16_t)(lo_ | hi_ << 8); }))
#define A_IND() (__extension__({ const unsigned o_ = rd(cpu, (uint16_t)(pc_ + 1)); \
    const unsigned lo_ = rd(cpu, ZP_AT(o_)); \
    const unsigned hi_ = rd(cpu, ZP_AT((uint8_t)(o_ + 1))); \
    (uint16_t)(lo_ | hi_ << 8); }))
#define A_IX() (__extension__({ const unsigned o_ = (uint8_t)(rd(cpu, (uint16_t)(pc_ + 1)) + x_); \
    const unsigned lo_ = rd(cpu, ZP_AT(o_)); \
    const unsigned hi_ = rd(cpu, ZP_AT((uint8_t)(o_ + 1))); \
    (uint16_t)(lo_ | hi_ << 8); }))
#define A_IY() (__extension__({ const unsigned o_ = rd(cpu, (uint16_t)(pc_ + 1)); \
    const unsigned lo_ = rd(cpu, ZP_AT(o_)); \
    const unsigned hi_ = rd(cpu, ZP_AT((uint8_t)(o_ + 1))); \
    (uint16_t)((uint16_t)(lo_ | hi_ << 8) + y_); }))

#define set_zn(v) do { const uint8_t zn_v_ = (v); \
    p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_N)) | (zn_v_ ? (zn_v_ & 0x80u ? PCE_P_N : 0) : PCE_P_Z)); } while (0)

/* flag-preserving add/sub/compare (the locals carry a prefix so an
 * argument never shadows them) */
#define adc(am) (__extension__({ const uint8_t adc_m_ = (am); \
    const unsigned adc_l_ = a_ + adc_m_ + (p_ & 1); \
    p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_C | PCE_P_N | PCE_P_V)) \
        | (((((a_ ^ adc_m_) & 0x80u) ^ 0x80u) & ((a_ ^ adc_l_) & 0x80u)) >> 1) \
        | ((adc_l_ >> 8) & 1)); \
    (uint8_t)adc_l_; }))
#define sbc(am) (__extension__({ const uint8_t sbc_m_ = (am); \
    const unsigned sbc_l_ = a_ - sbc_m_ - ((p_ & 1) ^ 1); \
    p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_C | PCE_P_N | PCE_P_V)) \
        | ((((a_ ^ sbc_l_) & (a_ ^ sbc_m_) & 0x80u)) >> 1) \
        | ((((sbc_l_ >> 8) & 1) ^ 1))); \
    (uint8_t)sbc_l_; }))
#define cmp(r, m) do { const unsigned t_ = (r) - (m); set_zn((uint8_t)t_); \
    p_ = (uint8_t)((p_ & ~PCE_P_C) | (((t_ >> 8) & 1) ^ 1)); } while (0)

#define ASL(m) (__extension__({ unsigned r_ = (m); p_ = (uint8_t)((p_ & ~PCE_P_C) | (r_ >> 7)); \
    (uint8_t)((r_ << 1) & 0xFFu); }))
#define LSR(m) (__extension__({ unsigned r_ = (m); p_ = (uint8_t)((p_ & ~PCE_P_C) | (r_ & 1)); \
    (uint8_t)(r_ >> 1); }))
#define ROL(m) (__extension__({ const unsigned c_ = p_ & PCE_P_C; unsigned r_ = (m); \
    p_ = (uint8_t)((p_ & ~PCE_P_C) | (r_ >> 7)); (uint8_t)(((r_ << 1) | c_) & 0xFFu); }))
#define ROR(m) (__extension__({ const unsigned c_ = p_ & PCE_P_C; unsigned r_ = (m); \
    p_ = (uint8_t)((p_ & ~PCE_P_C) | (r_ & 1)); (uint8_t)((r_ >> 1) | (c_ << 7)); }))

/* branches: operand first, then the page-crossing cost — none on the
 * HuC6280 */
#define BRANCH(take) do { \
    const int8_t off = (int8_t)rd(cpu, (uint16_t)(pc_ + 1)); \
    if (take) { pc_ = (uint16_t)(pc_ + 2 + off); left = 4; } \
    else { pc_ = (uint16_t)(pc_ + 2); left = 2; } \
    pc_set = 1; \
} while (0)

/* the interrupt sequence: 8 cycles, B pushed clear, T and D cleared */
#define INTERRUPT(vector) do { \
    push(cpu, (uint8_t)(pc_ >> 8)); \
    push(cpu, (uint8_t)pc_); \
    push(cpu, (uint8_t)(p_ & ~PCE_P_B)); \
    p_ |= PCE_P_I; \
    p_ &= (uint8_t)~(PCE_P_T | PCE_P_D); \
    { const unsigned lo_ = rd(cpu, (vector)); const unsigned hi_ = rd(cpu, (uint16_t)((vector) + 1)); \
      pc_ = (uint16_t)(lo_ | hi_ << 8); } \
} while (0)

/* ADC/SBC: binary, decimal (+1 cycle) and T mode compose; the T mode's
 * result lands in the zero page at X and leaves the flags of the result */
#define ADC_CORE(m) do { if (p_ & PCE_P_D) { left++; \
        unsigned t_ = (a_ & 0x0F) + ((m) & 0x0F) + (p_ & 1); \
        if (t_ >= 0x0A) t_ += 0x06; \
        t_ += (a_ & 0xF0) + ((m) & 0xF0); \
        p_ &= (uint8_t)~(PCE_P_Z | PCE_P_N | PCE_P_C); \
        if (t_ & 0xFF00u) p_ |= PCE_P_C; \
        a_ = (uint8_t)t_; set_zn(a_); \
    } else { a_ = adc(m); set_zn(a_); } } while (0)
#define SBC_CORE(m) do { if (p_ & PCE_P_D) { left++; \
        uint8_t lo_ = (uint8_t)((a_ & 0xF) - ((m) & 0xF) - ((p_ & 1) ^ 1)); \
        uint8_t hi_ = (uint8_t)((a_ >> 4) - ((m) >> 4) - ((lo_ >> 4) & 1)); \
        uint8_t res_ = (uint8_t)((hi_ << 4) | (lo_ & 0xF)); \
        p_ &= (uint8_t)~(PCE_P_Z | PCE_P_N | PCE_P_C); \
        if (lo_ & 0x10u) res_ -= 0x06; \
        if (hi_ & 0x10u) res_ -= 0x60; \
        a_ = res_; p_ |= (uint8_t)(((hi_ >> 4) & 1) ^ 1); set_zn(a_); \
    } else { a_ = sbc(m); set_zn(a_); } } while (0)
/* the operand is read first, whatever the mode */
#define LOGIC(opname, get) do { const uint8_t m_ = (get); \
    if (p_ & PCE_P_T) { const uint8_t aback_ = a_; \
        a_ = rd(cpu, ZP_AT(x_)); left += 2; a_ opname##= m_; set_zn(a_); \
        wr(cpu, ZP_AT(x_), a_); a_ = aback_; \
    } else { a_ opname##= m_; set_zn(a_); } } while (0)
#define ARITH(core, get) do { const uint8_t m_ = (get); \
    if (p_ & PCE_P_T) { const uint8_t aback_ = a_; \
        a_ = rd(cpu, ZP_AT(x_)); left += 2; core(m_); \
        wr(cpu, ZP_AT(x_), a_); a_ = aback_; \
    } else { core(m_); } } while (0)

/* the block moves: TII $73, TDD $C3, TIN $D3, TIA $E3, TAI $F3 — source,
 * destination and length, 16 bits each; 14 cycles of fixed part and 6 a
 * byte, plus the registers' pushes and pops.  Seven bytes: the opcode
 * plus the three 16-bit operands. */
#define BLOCK_MOVE(loop) do { \
    const unsigned sl_ = A_ABS(); pc_ += 2; \
    const unsigned dl_ = A_ABS(); pc_ += 2; \
    const unsigned ln_ = A_ABS(); pc_ += 3; \
    push(cpu, y_); push(cpu, a_); push(cpu, x_); \
    uint16_t src = (uint16_t)sl_, dst = (uint16_t)dl_; uint16_t len = (uint16_t)ln_; \
    unsigned alt = 0; uint32_t n_ = 0; \
    do { loop; alt ^= 1; len--; n_++; } while (len); \
    x_ = pop(cpu); a_ = pop(cpu); y_ = pop(cpu); \
    left = 17u + 6u * n_; \
} while (0)

void huc6280_map(huc6280_t *cpu) {
    const uint32_t pages = (cpu->rom_bytes + 0x1FFFu) / 0x2000u;
    uint64_t key = 0;
    for (unsigned i = 0; i < 8; i++) key |= (uint64_t)cpu->mpr[i] << (8 * i);
    cpu->mpr_key = key;
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t v = cpu->mpr[i];
        if (v < pages) {
            cpu->rbase[i] = cpu->rom + (uint32_t)v * 0x2000u;
            cpu->wbase[i] = 0;
        } else if (v >= 0xF8 && v <= 0xFB) { /* the WRAM mirrors on four pages */
            cpu->rbase[i] = cpu->wram;
            cpu->wbase[i] = cpu->wram;
        } else {
            cpu->rbase[i] = 0; /* $FF: the hardware page, else open bus */
            cpu->wbase[i] = 0;
        }
    }
}

void huc6280_power(huc6280_t *cpu) {
    cpu->a = cpu->x = cpu->y = 0;
    cpu->s = 0;
    cpu->p = 0;
    cpu->pc = 0;
    cpu->cycles = 0;
    cpu->irq_low = 0;
    for (unsigned i = 0; i < 8; i++) cpu->mpr[i] = 0;
    cpu->io_latch = 0xFF;
}

void huc6280_reset(huc6280_t *cpu) {
    cpu->speed = 0;
    cpu->irq_mask = 7;
    cpu->mpr[7] = 0; /* the vectors live in the cartridge's page 0 */
    huc6280_map(cpu);
    cpu->pc = (uint16_t)(rd_out(cpu, 0xFFFE) | rd_out(cpu, 0xFFFF) << 8);
    cpu->p = PCE_P_I;
}

#ifdef PCE_NATIVE
#define PCE_INTERP_FN static void huc6280_interp
#else
#define PCE_INTERP_FN void huc6280_run
#endif
PCE_INTERP_FN(huc6280_t *cpu, uint32_t until) {
    /* signed: an instruction runs past `until`, the budget goes negative
     * and the overshoot stays in cpu->cycles */
    int32_t budget = (int32_t)(until - cpu->cycles);
    uint16_t pc_ = cpu->pc;
    uint8_t s_ = cpu->s, a_ = cpu->a, x_ = cpu->x, y_ = cpu->y, p_ = cpu->p;
    const uint8_t *rbase_[8];
    uint8_t *wbase_[8];
    uint32_t left;
    int keep_t;
    int pc_set; /* the case put an address in pc_ by itself (a branch, a
        jump, a return): the step below must not move it — a branch or a
        jump to its own address leaves pc_ where it stands, on purpose */
    for (unsigned i = 0; i < 8; i++) { rbase_[i] = cpu->rbase[i]; wbase_[i] = cpu->wbase[i]; }
    while (budget > 0) {
        keep_t = 0;
        pc_set = 0;
        const uint16_t pc0 = pc_; /* the cases move pc_; the one-byte ones
            leave it on the opcode and the step below advances it */
        if (cpu->irq_low & cpu->irq_mask && !(p_ & PCE_P_I)) {
            unsigned vector = 0xFFF6; /* IRQ2 */
            if (cpu->irq_low & PCE_IQ_TIMER) vector = 0xFFFA;
            else if (cpu->irq_low & PCE_IQ_IRQ1) vector = 0xFFF8;
            INTERRUPT(vector);
            budget -= 8;
            PCE_DYN_HOOK(pc_);
            PCE_TRACE_HOOK(pc_);
            continue;
        }
        PCE_TRACE_HOOK(pc_);
        {
            const uint8_t op = rd(cpu, pc_); /* pc_ stays on the opcode;
                the cases advance it by the instruction's length */
            left = 2;
            switch (op) {
        /* loads */
        case 0xA9: a_ = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(a_); pc_ += 2; break;
        case 0xA5: a_ = rd(cpu, A_ZP()); set_zn(a_); pc_ += 2; left = 4; break;
        case 0xB5: a_ = rd(cpu, A_ZPI(x_)); set_zn(a_); pc_ += 2; left = 4; break;
        case 0xAD: a_ = rd(cpu, A_ABS()); set_zn(a_); pc_ += 3; left = 5; break;
        case 0xBD: a_ = rd(cpu, (uint16_t)(A_ABS() + x_)); set_zn(a_); pc_ += 3; left = 5; break;
        case 0xB9: a_ = rd(cpu, (uint16_t)(A_ABS() + y_)); set_zn(a_); pc_ += 3; left = 5; break;
        case 0xB2: a_ = rd(cpu, A_IND()); set_zn(a_); pc_ += 2; left = 7; break;
        case 0xA1: a_ = rd(cpu, A_IX()); set_zn(a_); pc_ += 2; left = 7; break;
        case 0xB1: a_ = rd(cpu, A_IY()); set_zn(a_); pc_ += 2; left = 7; break;
        case 0xA2: x_ = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(x_); pc_ += 2; break;
        case 0xA6: x_ = rd(cpu, A_ZP()); set_zn(x_); pc_ += 2; left = 4; break;
        case 0xB6: x_ = rd(cpu, A_ZPI(y_)); set_zn(x_); pc_ += 2; left = 4; break;
        case 0xAE: x_ = rd(cpu, A_ABS()); set_zn(x_); pc_ += 3; left = 5; break;
        case 0xBE: x_ = rd(cpu, (uint16_t)(A_ABS() + y_)); set_zn(x_); pc_ += 3; left = 5; break;
        case 0xA0: y_ = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(y_); pc_ += 2; break;
        case 0xA4: y_ = rd(cpu, A_ZP()); set_zn(y_); pc_ += 2; left = 4; break;
        case 0xB4: y_ = rd(cpu, A_ZPI(x_)); set_zn(y_); pc_ += 2; left = 4; break;
        case 0xAC: y_ = rd(cpu, A_ABS()); set_zn(y_); pc_ += 3; left = 5; break;
        case 0xBC: y_ = rd(cpu, (uint16_t)(A_ABS() + x_)); set_zn(y_); pc_ += 3; left = 5; break;
        /* stores */
        case 0x85: wr(cpu, A_ZP(), a_); pc_ += 2; left = 4; break;
        case 0x95: wr(cpu, A_ZPI(x_), a_); pc_ += 2; left = 4; break;
        case 0x8D: wr(cpu, A_ABS(), a_); pc_ += 3; left = 5; break;
        case 0x9D: wr(cpu, (uint16_t)(A_ABS() + x_), a_); pc_ += 3; left = 5; break;
        case 0x99: wr(cpu, (uint16_t)(A_ABS() + y_), a_); pc_ += 3; left = 5; break;
        case 0x92: wr(cpu, A_IND(), a_); pc_ += 2; left = 7; break;
        case 0x81: wr(cpu, A_IX(), a_); pc_ += 2; left = 7; break;
        case 0x91: wr(cpu, A_IY(), a_); pc_ += 2; left = 7; break;
        case 0x86: wr(cpu, A_ZP(), x_); pc_ += 2; left = 4; break;
        case 0x96: wr(cpu, A_ZPI(y_), x_); pc_ += 2; left = 4; break;
        case 0x8E: wr(cpu, A_ABS(), x_); pc_ += 3; left = 5; break;
        case 0x84: wr(cpu, A_ZP(), y_); pc_ += 2; left = 4; break;
        case 0x94: wr(cpu, A_ZPI(x_), y_); pc_ += 2; left = 4; break;
        case 0x8C: wr(cpu, A_ABS(), y_); pc_ += 3; left = 5; break;
        /* STZ */
        case 0x64: wr(cpu, A_ZP(), 0); pc_ += 2; left = 4; break;
        case 0x74: wr(cpu, A_ZPI(x_), 0); pc_ += 2; left = 4; break;
        case 0x9C: wr(cpu, A_ABS(), 0); pc_ += 3; left = 5; break;
        case 0x9E: wr(cpu, (uint16_t)(A_ABS() + x_), 0); pc_ += 3; left = 5; break;
        /* logic: AND, ORA, EOR, with the T mode */
        case 0x29: left = 2; LOGIC(&, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0x25: left = 4; LOGIC(&, rd(cpu, A_ZP())); pc_ += 2; break;
        case 0x35: left = 4; LOGIC(&, rd(cpu, A_ZPI(x_))); pc_ += 2; break;
        case 0x2D: left = 5; LOGIC(&, rd(cpu, A_ABS())); pc_ += 3; break;
        case 0x3D: left = 5; LOGIC(&, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; break;
        case 0x39: left = 5; LOGIC(&, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; break;
        case 0x32: left = 7; LOGIC(&, rd(cpu, A_IND())); pc_ += 2; break;
        case 0x21: left = 7; LOGIC(&, rd(cpu, A_IX())); pc_ += 2; break;
        case 0x31: left = 7; LOGIC(&, rd(cpu, A_IY())); pc_ += 2; break;
        case 0x09: left = 2; LOGIC(|, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0x05: left = 4; LOGIC(|, rd(cpu, A_ZP())); pc_ += 2; break;
        case 0x15: left = 4; LOGIC(|, rd(cpu, A_ZPI(x_))); pc_ += 2; break;
        case 0x0D: left = 5; LOGIC(|, rd(cpu, A_ABS())); pc_ += 3; break;
        case 0x1D: left = 5; LOGIC(|, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; break;
        case 0x19: left = 5; LOGIC(|, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; break;
        case 0x12: left = 7; LOGIC(|, rd(cpu, A_IND())); pc_ += 2; break;
        case 0x01: left = 7; LOGIC(|, rd(cpu, A_IX())); pc_ += 2; break;
        case 0x11: left = 7; LOGIC(|, rd(cpu, A_IY())); pc_ += 2; break;
        case 0x49: left = 2; LOGIC(^, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0x45: left = 4; LOGIC(^, rd(cpu, A_ZP())); pc_ += 2; break;
        case 0x55: left = 4; LOGIC(^, rd(cpu, A_ZPI(x_))); pc_ += 2; break;
        case 0x4D: left = 5; LOGIC(^, rd(cpu, A_ABS())); pc_ += 3; break;
        case 0x5D: left = 5; LOGIC(^, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; break;
        case 0x59: left = 5; LOGIC(^, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; break;
        case 0x52: left = 7; LOGIC(^, rd(cpu, A_IND())); pc_ += 2; break;
        case 0x41: left = 7; LOGIC(^, rd(cpu, A_IX())); pc_ += 2; break;
        case 0x51: left = 7; LOGIC(^, rd(cpu, A_IY())); pc_ += 2; break;
        /* ADC and SBC: binary, decimal and T mode compose */
        case 0x69: left = 2; ARITH(ADC_CORE, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0x65: left = 4; ARITH(ADC_CORE, rd(cpu, A_ZP())); pc_ += 2; break;
        case 0x75: left = 4; ARITH(ADC_CORE, rd(cpu, A_ZPI(x_))); pc_ += 2; break;
        case 0x6D: left = 5; ARITH(ADC_CORE, rd(cpu, A_ABS())); pc_ += 3; break;
        case 0x7D: left = 5; ARITH(ADC_CORE, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; break;
        case 0x79: left = 5; ARITH(ADC_CORE, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; break;
        case 0x72: left = 7; ARITH(ADC_CORE, rd(cpu, A_IND())); pc_ += 2; break;
        case 0x61: left = 7; ARITH(ADC_CORE, rd(cpu, A_IX())); pc_ += 2; break;
        case 0x71: left = 7; ARITH(ADC_CORE, rd(cpu, A_IY())); pc_ += 2; break;
        case 0xE9: left = 2; ARITH(SBC_CORE, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0xE5: left = 4; ARITH(SBC_CORE, rd(cpu, A_ZP())); pc_ += 2; break;
        case 0xF5: left = 4; ARITH(SBC_CORE, rd(cpu, A_ZPI(x_))); pc_ += 2; break;
        case 0xED: left = 5; ARITH(SBC_CORE, rd(cpu, A_ABS())); pc_ += 3; break;
        case 0xFD: left = 5; ARITH(SBC_CORE, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; break;
        case 0xF9: left = 5; ARITH(SBC_CORE, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; break;
        case 0xF2: left = 7; ARITH(SBC_CORE, rd(cpu, A_IND())); pc_ += 2; break;
        case 0xE1: left = 7; ARITH(SBC_CORE, rd(cpu, A_IX())); pc_ += 2; break;
        case 0xF1: left = 7; ARITH(SBC_CORE, rd(cpu, A_IY())); pc_ += 2; break;
        /* compares */
        case 0xC9: cmp(a_, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0xC5: cmp(a_, rd(cpu, A_ZP())); pc_ += 2; left = 4; break;
        case 0xD5: cmp(a_, rd(cpu, A_ZPI(x_))); pc_ += 2; left = 4; break;
        case 0xCD: cmp(a_, rd(cpu, A_ABS())); pc_ += 3; left = 5; break;
        case 0xDD: cmp(a_, rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; left = 5; break;
        case 0xD9: cmp(a_, rd(cpu, (uint16_t)(A_ABS() + y_))); pc_ += 3; left = 5; break;
        case 0xD2: cmp(a_, rd(cpu, A_IND())); pc_ += 2; left = 7; break;
        case 0xC1: cmp(a_, rd(cpu, A_IX())); pc_ += 2; left = 7; break;
        case 0xD1: cmp(a_, rd(cpu, A_IY())); pc_ += 2; left = 7; break;
        case 0xE0: cmp(x_, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0xE4: cmp(x_, rd(cpu, A_ZP())); pc_ += 2; left = 4; break;
        case 0xEC: cmp(x_, rd(cpu, A_ABS())); pc_ += 3; left = 5; break;
        case 0xC0: cmp(y_, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0xC4: cmp(y_, rd(cpu, A_ZP())); pc_ += 2; left = 4; break;
        case 0xCC: cmp(y_, rd(cpu, A_ABS())); pc_ += 3; left = 5; break;
        /* BIT: the Z from A & m, the N and V from m itself */
        #define BIT_OF(m_) do { p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) \
            | (a_ & (m_) ? 0 : PCE_P_Z) | ((m_) & 0xC0u)); } while (0)
        case 0x89: BIT_OF(rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; break;
        case 0x24: BIT_OF(rd(cpu, A_ZP())); pc_ += 2; left = 4; break;
        case 0x34: BIT_OF(rd(cpu, A_ZPI(x_))); pc_ += 2; left = 4; break;
        case 0x2C: BIT_OF(rd(cpu, A_ABS())); pc_ += 3; left = 5; break;
        case 0x3C: BIT_OF(rd(cpu, (uint16_t)(A_ABS() + x_))); pc_ += 3; left = 5; break;
        #undef BIT_OF
        /* shifts, RMW */
        case 0x0A: a_ = ASL(a_); set_zn(a_); break;
        case 0x06: { const uint16_t at_ = A_ZP(); const uint8_t r_ = ASL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x16: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = ASL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x0E: { const uint16_t at_ = A_ABS(); const uint8_t r_ = ASL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x1E: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = ASL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x4A: a_ = LSR(a_); set_zn(a_); break;
        case 0x46: { const uint16_t at_ = A_ZP(); const uint8_t r_ = LSR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x56: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = LSR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x4E: { const uint16_t at_ = A_ABS(); const uint8_t r_ = LSR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x5E: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = LSR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x2A: a_ = ROL(a_); set_zn(a_); break;
        case 0x26: { const uint16_t at_ = A_ZP(); const uint8_t r_ = ROL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x36: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = ROL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x2E: { const uint16_t at_ = A_ABS(); const uint8_t r_ = ROL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x3E: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = ROL(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x6A: a_ = ROR(a_); set_zn(a_); break;
        case 0x66: { const uint16_t at_ = A_ZP(); const uint8_t r_ = ROR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x76: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = ROR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0x6E: { const uint16_t at_ = A_ABS(); const uint8_t r_ = ROR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0x7E: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = ROR(rd(cpu, at_));
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        /* inc/dec */
        case 0x1A: a_ = (uint8_t)(a_ + 1); set_zn(a_); break;
        case 0x3A: a_ = (uint8_t)(a_ - 1); set_zn(a_); break;
        case 0xE6: { const uint16_t at_ = A_ZP(); const uint8_t r_ = (uint8_t)(rd(cpu, at_) + 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0xF6: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = (uint8_t)(rd(cpu, at_) + 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0xEE: { const uint16_t at_ = A_ABS(); const uint8_t r_ = (uint8_t)(rd(cpu, at_) + 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0xFE: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = (uint8_t)(rd(cpu, at_) + 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0xC6: { const uint16_t at_ = A_ZP(); const uint8_t r_ = (uint8_t)(rd(cpu, at_) - 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0xD6: { const uint16_t at_ = A_ZPI(x_); const uint8_t r_ = (uint8_t)(rd(cpu, at_) - 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 2; left = 6; break; }
        case 0xCE: { const uint16_t at_ = A_ABS(); const uint8_t r_ = (uint8_t)(rd(cpu, at_) - 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0xDE: { const uint16_t at_ = (uint16_t)(A_ABS() + x_); const uint8_t r_ = (uint8_t)(rd(cpu, at_) - 1);
            wr(cpu, at_, r_); set_zn(r_); pc_ += 3; left = 7; break; }
        case 0xE8: x_++; set_zn(x_); break;
        case 0xCA: x_--; set_zn(x_); break;
        case 0xC8: y_++; set_zn(y_); break;
        case 0x88: y_--; set_zn(y_); break;
        /* RMB/SMB: reset and set memory bits, zero page */
        #define RMB_SMB(setbit) do { const uint16_t at_ = A_ZP(); uint8_t r_ = rd(cpu, at_); \
            if (setbit) r_ |= (uint8_t)(1u << ((op >> 4) & 7)); \
            else r_ &= (uint8_t)~(1u << ((op >> 4) & 7)); \
            wr(cpu, at_, r_); pc_ += 2; left = 7; } while (0)
        case 0x07: RMB_SMB(0); break;
        case 0x17: RMB_SMB(0); break;
        case 0x27: RMB_SMB(0); break;
        case 0x37: RMB_SMB(0); break;
        case 0x47: RMB_SMB(0); break;
        case 0x57: RMB_SMB(0); break;
        case 0x67: RMB_SMB(0); break;
        case 0x77: RMB_SMB(0); break;
        case 0x87: RMB_SMB(1); break;
        case 0x97: RMB_SMB(1); break;
        case 0xA7: RMB_SMB(1); break;
        case 0xB7: RMB_SMB(1); break;
        case 0xC7: RMB_SMB(1); break;
        case 0xD7: RMB_SMB(1); break;
        case 0xE7: RMB_SMB(1); break;
        case 0xF7: RMB_SMB(1); break;
        #undef RMB_SMB
        /* TSB/TRB: the Z from the pre-op test, the N and V from the
         * operand itself (the mednafen core's reading) */
        case 0x04: { const uint16_t at_ = A_ZP(); const uint8_t m_ = rd(cpu, at_);
            p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ | a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u));
            wr(cpu, at_, (uint8_t)(m_ | a_)); pc_ += 2; left = 6; break; }
        case 0x0C: { const uint16_t at_ = A_ABS(); const uint8_t m_ = rd(cpu, at_);
            p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ | a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u));
            wr(cpu, at_, (uint8_t)(m_ | a_)); pc_ += 3; left = 7; break; }
        case 0x14: { const uint16_t at_ = A_ZP(); const uint8_t m_ = rd(cpu, at_);
            p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ & (uint8_t)~a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u));
            wr(cpu, at_, (uint8_t)(m_ & (uint8_t)~a_)); pc_ += 2; left = 6; break; }
        case 0x1C: { const uint16_t at_ = A_ABS(); const uint8_t m_ = rd(cpu, at_);
            p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ & (uint8_t)~a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u));
            wr(cpu, at_, (uint8_t)(m_ & (uint8_t)~a_)); pc_ += 3; left = 7; break; }
        /* TST #m,ea: BIT's flags with an immediate mask.  The zp/abs
         * operand follows the immediate byte (83 imm zp), so A_ZP()'s
         * pc_+1 fetch would read the immediate as the address */
        #define TST_OF(m_, t_) do { p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) \
            | ((t_ & (m_)) ? 0 : PCE_P_Z) | ((m_) & 0xC0u)); } while (0)
        case 0x83: { const uint8_t imm_ = rd(cpu, (uint16_t)(pc_ + 1));
            const uint8_t m_ = rd(cpu, ZP_AT(rd(cpu, (uint16_t)(pc_ + 2))));
            TST_OF(imm_, m_); pc_ += 3; left = 7; break; }
        case 0xA3: { const uint8_t imm_ = rd(cpu, (uint16_t)(pc_ + 1));
            const uint8_t m_ = rd(cpu, ZP_AT((uint8_t)(rd(cpu, (uint16_t)(pc_ + 2)) + x_)));
            TST_OF(imm_, m_); pc_ += 3; left = 7; break; }
        case 0x93: { const uint8_t imm_ = rd(cpu, (uint16_t)(pc_ + 1));
            const uint16_t ea_ = (uint16_t)(rd(cpu, (uint16_t)(pc_ + 2)) | rd(cpu, (uint16_t)(pc_ + 3)) << 8);
            const uint8_t m_ = rd(cpu, ea_);
            TST_OF(imm_, m_); pc_ += 4; left = 8; break; }
        case 0xB3: { const uint8_t imm_ = rd(cpu, (uint16_t)(pc_ + 1));
            const uint16_t ea_ = (uint16_t)((rd(cpu, (uint16_t)(pc_ + 2)) | rd(cpu, (uint16_t)(pc_ + 3)) << 8) + x_);
            const uint8_t m_ = rd(cpu, ea_);
            TST_OF(imm_, m_); pc_ += 4; left = 8; break; }
        #undef TST_OF
        /* branches */
        case 0x80: BRANCH(1); break; /* BRA */
        case 0x10: BRANCH(!(p_ & PCE_P_N)); break;
        case 0x30: BRANCH(p_ & PCE_P_N); break;
        case 0x50: BRANCH(!(p_ & PCE_P_V)); break;
        case 0x70: BRANCH(p_ & PCE_P_V); break;
        case 0x90: BRANCH(!(p_ & PCE_P_C)); break;
        case 0xB0: BRANCH(p_ & PCE_P_C); break;
        case 0xD0: BRANCH(!(p_ & PCE_P_Z)); break;
        case 0xF0: BRANCH(p_ & PCE_P_Z); break;
        /* BBR/BBS: zero page, then relative */
        #define BBRBBS(setbit) do { const uint16_t at_ = ZP_AT(rd(cpu, (uint16_t)(pc_ + 1))); \
            const uint8_t m_ = rd(cpu, at_); \
            const int take_ = setbit ? (m_ >> ((op >> 4) & 7)) & 1 : !((m_ >> ((op >> 4) & 7)) & 1); \
            const int8_t off_ = (int8_t)rd(cpu, (uint16_t)(pc_ + 2)); \
            if (take_) { pc_ = (uint16_t)(pc_ + 3 + off_); left = 8; } \
            else { pc_ = (uint16_t)(pc_ + 3); left = 6; } \
            pc_set = 1; } while (0)
        case 0x0F: BBRBBS(0); break;
        case 0x1F: BBRBBS(0); break;
        case 0x2F: BBRBBS(0); break;
        case 0x3F: BBRBBS(0); break;
        case 0x4F: BBRBBS(0); break;
        case 0x5F: BBRBBS(0); break;
        case 0x6F: BBRBBS(0); break;
        case 0x7F: BBRBBS(0); break;
        case 0x8F: BBRBBS(1); break;
        case 0x9F: BBRBBS(1); break;
        case 0xAF: BBRBBS(1); break;
        case 0xBF: BBRBBS(1); break;
        case 0xCF: BBRBBS(1); break;
        case 0xDF: BBRBBS(1); break;
        case 0xEF: BBRBBS(1); break;
        case 0xFF: BBRBBS(1); break;
        #undef BBRBBS
        /* jumps, calls, returns */
        case 0x4C: pc_ = A_ABS(); left = 4; pc_set = 1; break;
        case 0x6C: { /* the 6502's page-wrap bug, faithfully */
            const uint16_t ptr = A_ABS();
            const unsigned lo_ = rd(cpu, ptr);
            const unsigned hi_ = rd(cpu, (uint16_t)((ptr & 0xFF00u) | ((ptr + 1) & 0xFFu)));
            pc_ = (uint16_t)(lo_ | hi_ << 8); left = 7; pc_set = 1; PCE_DYN_HOOK(pc_); break; }
        case 0x7C: { const uint16_t ptr = (uint16_t)(A_ABS() + x_);
            const unsigned lo_ = rd(cpu, ptr);
            const unsigned hi_ = rd(cpu, (uint16_t)(ptr + 1));
            pc_ = (uint16_t)(lo_ | hi_ << 8); left = 7; pc_set = 1; PCE_DYN_HOOK(pc_); break; }
        case 0x20: { const unsigned lo_ = rd(cpu, (uint16_t)(pc_ + 1)); pc_ += 2;
            const uint16_t ret = pc_; /* the high operand byte's address, as the 6502 */
            push(cpu, (uint8_t)(ret >> 8)); push(cpu, (uint8_t)ret);
            const unsigned hi_ = rd(cpu, pc_);
            pc_ = (uint16_t)(hi_ << 8 | lo_); left = 7; pc_set = 1; break; }
        case 0x60: { const uint8_t lo8 = pop(cpu), hi8 = pop(cpu);
            pc_ = (uint16_t)(lo8 | hi8 << 8); pc_++; left = 7; pc_set = 1; PCE_DYN_HOOK(pc_); break; }
        case 0x40: p_ = pop(cpu); keep_t = 1;
            { const uint8_t lo8 = pop(cpu), hi8 = pop(cpu); pc_ = (uint16_t)(lo8 | hi8 << 8); }
            left = 7; pc_set = 1; PCE_DYN_HOOK(pc_); break;
        case 0x00: pc_ += 2; /* past the signature byte, as the 6502 */
            push(cpu, (uint8_t)(pc_ >> 8)); push(cpu, (uint8_t)pc_);
            push(cpu, (uint8_t)(p_ | PCE_P_B));
            p_ |= PCE_P_I; p_ &= (uint8_t)~PCE_P_D;
            { const unsigned lo_ = rd(cpu, 0xFFF6); const unsigned hi_ = rd(cpu, 0xFFF7);
              pc_ = (uint16_t)(lo_ | hi_ << 8); }
            left = 8; pc_set = 1; PCE_DYN_HOOK(pc_); break;
        /* BSR: a relative call; the return address is the displacement
         * byte's own address (RTS's +1 lands past the instruction) */
        case 0x44: { const int8_t off = (int8_t)rd(cpu, (uint16_t)(pc_ + 1));
            const uint16_t ret = (uint16_t)(pc_ + 1);
            push(cpu, (uint8_t)(ret >> 8)); push(cpu, (uint8_t)ret);
            pc_ = (uint16_t)(pc_ + 2 + off); left = 8; pc_set = 1; break; }
        /* stack */
        case 0x48: push(cpu, a_); left = 3; break;
        case 0xDA: push(cpu, x_); left = 3; break;
        case 0x5A: push(cpu, y_); left = 3; break;
        case 0x08: p_ &= (uint8_t)~PCE_P_T; push(cpu, (uint8_t)(p_ | PCE_P_B)); left = 3; break;
        case 0x68: a_ = pop(cpu); set_zn(a_); left = 4; break;
        case 0xFA: x_ = pop(cpu); set_zn(x_); left = 4; break;
        case 0x7A: y_ = pop(cpu); set_zn(y_); left = 4; break;
        case 0x28: p_ = pop(cpu); keep_t = 1; left = 4; break;
        /* flags */
        case 0x18: p_ &= (uint8_t)~PCE_P_C; break;
        case 0x38: p_ |= PCE_P_C; break;
        case 0x58: p_ &= (uint8_t)~PCE_P_I; break;
        case 0x78: p_ |= PCE_P_I; break;
        case 0xB8: p_ &= (uint8_t)~PCE_P_V; break;
        case 0xD8: p_ &= (uint8_t)~PCE_P_D; break;
        case 0xF8: p_ |= PCE_P_D; break;
        case 0xF4: p_ |= PCE_P_T; keep_t = 1; break; /* SET */
        /* transfers, clears, swaps, speed */
        case 0xAA: x_ = a_; set_zn(x_); break;
        case 0xA8: y_ = a_; set_zn(y_); break;
        case 0x8A: a_ = x_; set_zn(a_); break;
        case 0x98: a_ = y_; set_zn(a_); break;
        case 0xBA: x_ = s_; set_zn(x_); break;
        case 0x9A: s_ = x_; break;
        case 0x62: a_ = 0; break; /* CLA */
        case 0x82: x_ = 0; break; /* CLX */
        case 0xC2: y_ = 0; break; /* CLY */
        case 0x02: { const uint8_t t_ = x_; x_ = y_; y_ = t_; left = 3; break; } /* SXY */
        case 0x22: { const uint8_t t_ = x_; x_ = a_; a_ = t_; left = 3; break; } /* SAX */
        case 0x42: { const uint8_t t_ = y_; y_ = a_; a_ = t_; left = 3; break; } /* SAY */
        case 0x54: cpu->speed = 0; left = 3; break; /* CSL */
        case 0xD4: cpu->speed = 1; left = 3; break; /* CSH */
        case 0xEA: break; /* NOP */
        /* the MPR pages */
        case 0x53: { const uint8_t m_ = rd(cpu, (uint16_t)(pc_ + 1)); pc_ += 2; left = 5;
            for (unsigned i = 0; i < 8; i++) if (m_ & (1u << i)) cpu->mpr[i] = a_;
            huc6280_map(cpu);
            for (unsigned i = 0; i < 8; i++) { rbase_[i] = cpu->rbase[i]; wbase_[i] = cpu->wbase[i]; }
            break; }
        case 0x43: { const uint8_t m_ = rd(cpu, (uint16_t)(pc_ + 1)); pc_ += 2; left = 4;
            for (unsigned i = 0; i < 8; i++) if (m_ & (1u << i)) a_ = cpu->mpr[i];
            break; }
        /* the VDC's ports, straight to the registers */
        case 0x03: pce_bus_write(0x0000, a_); left = 4; break; /* ST0 */
        case 0x13: pce_bus_write(0x0002, a_); left = 4; break; /* ST1 */
        case 0x23: pce_bus_write(0x0003, a_); left = 4; break; /* ST2 */
        /* the block moves: source, destination, length */
        case 0x73: BLOCK_MOVE(const uint8_t v_ = rd(cpu, src); wr(cpu, dst, v_); src++; dst++;); break;   /* TII */
        case 0xC3: BLOCK_MOVE(const uint8_t v_ = rd(cpu, src); wr(cpu, dst, v_); src--; dst--;); break;   /* TDD */
        case 0xD3: BLOCK_MOVE(const uint8_t v_ = rd(cpu, src); wr(cpu, dst, v_); src++;); break;          /* TIN */
        case 0xE3: BLOCK_MOVE(const uint8_t v_ = rd(cpu, src); wr(cpu, (uint16_t)(dst + alt), v_); src++;); break; /* TIA */
        case 0xF3: BLOCK_MOVE(const uint8_t v_ = rd(cpu, (uint16_t)(src + alt)); wr(cpu, dst, v_); dst++;); break; /* TAI */
        default: /* illegal or NOP: one byte, two cycles */
            break;
        }
        }
        if (!pc_set && pc_ == pc0) pc_ = (uint16_t)(pc0 + 1); /* a one-byte instruction */
        if (!keep_t) p_ &= (uint8_t)~PCE_P_T;
        budget -= left;
    }
    cpu->pc = pc_;
    cpu->s = s_;
    cpu->a = a_;
    cpu->x = x_;
    cpu->y = y_;
    cpu->p = p_;
    cpu->cycles = until - (uint32_t)budget;
}

#ifdef PCE_NATIVE
/* Recompiled code (pce_gen.c) in front of the interpreter.  The loop is the
 * interpreter's own — the interrupt line, then one step — except that a
 * step is a call into the recompiled entry at (pc, MPR context) when there
 * is one: it runs until the slice is spent (precisely: after the
 * instruction that spends it, the same boundary as the interpreter;
 * otherwise at its next check), or until a transfer it cannot follow.  The
 * context check makes a re-mapped window fall back instead of running the
 * wrong bytes.  Anything else — code in WRAM, contexts the walk could not
 * prove, what the budget left out — is interpreted one instruction at a
 * time.  pce_native = 0 is the reference (--interp). */
#include "pce_gen.h"

int pce_native = 1;

/* the interpreter's push (its wr through the window tables) at the
 * dispatcher's reach */
static void disp_push(huc6280_t *cpu, uint8_t v) {
    const uint16_t at = (uint16_t)(0x2100u | cpu->s);
    uint8_t *b = cpu->wbase[at >> 13];
    if (b) b[at & 0x1FFFu] = v;
    else pce_bus_write(at, v);
    cpu->s--;
}

void huc6280_run(huc6280_t *cpu, uint32_t until) {
    if (!pce_native) { huc6280_interp(cpu, until); return; }
    int32_t budget = (int32_t)(until - cpu->cycles);
    while (budget > 0) {
        if (cpu->irq_low & cpu->irq_mask && !(cpu->p & PCE_P_I)) {
            unsigned vector = 0xFFF6; /* IRQ2 */
            if (cpu->irq_low & PCE_IQ_TIMER) vector = 0xFFFA;
            else if (cpu->irq_low & PCE_IQ_IRQ1) vector = 0xFFF8;
            disp_push(cpu, (uint8_t)(cpu->pc >> 8));
            disp_push(cpu, (uint8_t)cpu->pc);
            disp_push(cpu, (uint8_t)(cpu->p & ~PCE_P_B));
            cpu->p |= PCE_P_I;
            cpu->p &= (uint8_t)~(PCE_P_T | PCE_P_D);
            cpu->pc = (uint16_t)(rd_out(cpu, (uint16_t)vector) | rd_out(cpu, (uint16_t)(vector + 1)) << 8);
            budget -= 8;
            continue;
        }
        /* the idle poll loop: LDA zp/zp,X/abs followed by (B)EQ backwards
         * to the LDA — the game waiting for its vblank flag, most of its
         * time in a menu or an attract mode.  The lap runs here: one
         * windowed read and the exact cycles the regions spend (the zp
         * fetch is 4, the taken branch 4), IRQs sampled every lap.  The
         * MPR windows are live (huc6280_map keeps them in step), so the
         * poll reads whatever the handler left mapped; a handler that
         * rewrites the loop's own bytes is the one case this misses (the
         * JMP-self spin below shares the assumption). */
        {
            const uint8_t *b = cpu->rbase[cpu->pc >> 13];
            if (b) {
                const unsigned at = cpu->pc & 0x1FFFu;
                const uint8_t op = b[at];
                unsigned size = 0; /* instruction bytes up to the branch */
                if (op == 0xA5u || op == 0xB5u) size = 2;       /* LDA zp / zp,X */
                else if (op == 0xADu) size = 3;                 /* LDA abs */
                if (size) {
                    const uint8_t bop = b[(at + size) & 0x1FFFu];
                    if (bop == 0xF0u || bop == 0xD0u) {         /* BEQ / BNE */
                        const uint16_t br_at = (uint16_t)(cpu->pc + size);
                        const uint16_t target = (uint16_t)(br_at + 2 + (int8_t)b[(at + size + 1) & 0x1FFFu]);
                        if (target == cpu->pc) {
                            const unsigned poll_bne = (bop == 0xD0u);
                            uint16_t addr;         /* the byte's full bus address */
                            const uint8_t *w;      /* its window, NULL = the bus */
                            if (op == 0xADu) {
                                addr = (uint16_t)(b[(at + 1) & 0x1FFFu] | b[(at + 2) & 0x1FFFu] << 8);
                                w = cpu->rbase[addr >> 13];
                            } else {
                                addr = (uint16_t)(0x2000u | b[(at + 1) & 0x1FFFu]);
                                w = cpu->rbase[1];
                            }
                            for (;;) {
                                if (cpu->irq_low & cpu->irq_mask && !(cpu->p & PCE_P_I)) break;
                                uint8_t a;
                                if (op == 0xB5u) {
                                    const uint16_t e = (uint16_t)(0x2000u | (uint8_t)(addr + cpu->x));
                                    a = w ? w[e & 0x1FFFu] : pce_bus_read(e);
                                } else {
                                    a = w ? w[addr & 0x1FFFu] : pce_bus_read(addr);
                                }
                                cpu->cycles += 4;
                                budget -= 4;
                                /* the budget may expire on the read: the
                                 * interpreter's loop-top check would stop
                                 * here, pc left on the branch — spending
                                 * the branch anyway adds 4 cycles the
                                 * reference never spends */
                                if (budget <= 0) {
                                    cpu->pc = br_at;
                                    break;
                                }
                                cpu->p &= (uint8_t)~(PCE_P_T | PCE_P_Z | PCE_P_N);
                                if (a & 0x80u) cpu->p |= PCE_P_N; else cpu->p |= PCE_P_Z;
                                if (poll_bne ? a == 0 : a != 0) {
                                    cpu->cycles += 2; /* the branch not taken */
                                    budget -= 2;
                                    cpu->pc = (uint16_t)(br_at + 2); /* and it falls through */
                                    break;
                                }
                                cpu->cycles += 4; /* the branch taken back */
                                budget -= 4;
                                if (budget <= 0) break;
                            }
                            continue;
                        }
                    }
                }
            }
        }
        const int entry = pce_gen_lookup(cpu->pc, cpu->mpr_key);
        if (entry >= 0) {
            budget = pce_gen_enter(entry, budget);
            continue;
        }
        /* BRA/JMP to themselves in a ROM window, a game waiting for its
         * interrupt: the rest of the slice in laps of 4 cycles, as the
         * recompiled loop spends it (emitter.cpp).  Checked on the
         * fallback path only: a compiled dispatch pays no read here. */
        {
            const uint8_t *b = cpu->rbase[cpu->pc >> 13];
            if (b && b >= cpu->rom && b < cpu->rom + cpu->rom_bytes) {
                const unsigned at = cpu->pc & 0x1FFFu;
                const uint8_t op = b[at];
                const unsigned o1 = b[(at + 1u) & 0x1FFFu], o2 = b[(at + 2u) & 0x1FFFu];
                if ((op == 0x80 && o1 == 0xFEu) ||
                    (op == 0x4C && (uint16_t)(o1 | o2 << 8) == cpu->pc)) {
                    budget = PCE_SPIN4(budget);
                    break;
                }
            }
        }
        /* no entry here: the interpreter runs the rest of the slice — its
         * own loop, its own IRQ sampling — and the dispatcher comes back
         * at the next slice boundary (compiled code re-entered there) */
        cpu->cycles = until - (uint32_t)budget;
        huc6280_interp(cpu, until);
        budget = (int32_t)(until - cpu->cycles); /* the overshoot the
            interpreter left in cpu->cycles: the loop's exit assignment
            must not flatten it back to `until` */
        break;
    }
    cpu->cycles = until - (uint32_t)budget;
}
#endif
