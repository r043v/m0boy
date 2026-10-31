/* The S-SMP: a complete SPC700 interpreter in front of the 64 KiB A-RAM,
 * the three timers and the comm ports.  Cycle counts follow the SPC700
 * manual's matrix; the master clock is the 65816's at 1024000/3579545,
 * driven from the frame loop through snes_apu_run.  The timers tick at
 * 8 kHz (T0/T1) and 64 kHz (T2), their readable outputs ($FD-$FF)
 * increment at source/(target+1) while the run bit of $F1 stands.
 *
 * The boot's IPL ROM is simulated by a state machine over the CPU's
 * $2140-$2143 writes — no ROM bytes: the $AA/$BB handshake starts an
 * upload whose bytes land in A-RAM and echo back on port 0, and a port-0
 * zero after fresh $2142/3 starts the game at that address (a block
 * whose first byte is $00 would jump instead).  Games that reset the
 * S-SMP in play are not modelled.
 *
 * Not modelled: the exact DIV overflow result (approximated to $FF/$FF),
 * the TEST register ($F0), $F1's bit 4, and the SPC700's own cycle-level
 * bus interleaving with the DMA. */
#include "snes.h"
#define PSW_N 0x80
#define PSW_V 0x40
#define PSW_P 0x20
#define PSW_B 0x10
#define PSW_H 0x08
#define PSW_I 0x04
#define PSW_Z 0x02
#define PSW_C 0x01

/* the DSP's face (snes_dsp.c) */
uint8_t snes_dsp_read(uint8_t at);
void snes_dsp_write(uint8_t at, uint8_t v);
void snes_dsp_sample(void);

static void smp_push(uint8_t v) {
    machine.smp.ram[0x100u | machine.smp.sp] = v;
    machine.smp.sp--;
}
static uint8_t smp_pop(void) {
    return machine.smp.ram[0x100u | ++machine.smp.sp];
}

static uint8_t smp_read(uint16_t at) {
    switch (at & 0xFFF0u) {
    case 0x00F0:
        switch (at & 0xF) {
        case 1: return machine.smp.control;
        case 2: return machine.smp.dsp_addr;
        case 3: return snes_dsp_read(machine.smp.dsp_addr);
        case 4: case 5: case 6: case 7: return machine.smp.ports_in[at & 3];
        case 0xD: case 0xE: case 0xF: return machine.smp.timer_out[(at & 0xF) - 0xD];
        case 0: return 0; /* TEST */
        default: break;   /* $F8-$FC: plain RAM */
        }
        break;
    default:
        if (at >= 0xFFC0) return 0; /* the IPL ROM's region, simulated */
        break;
    }
    return machine.smp.ram[at];
}

static void smp_write(uint16_t at, uint8_t v) {
    switch (at & 0xFFF0u) {
    case 0x00F0:
        switch (at & 0xF) {
        case 1: machine.smp.control = v; return;
        case 2: machine.smp.dsp_addr = v; return;
        case 3: snes_dsp_write(machine.smp.dsp_addr, v); return;
        case 4: case 5: case 6: case 7:
            machine.smp.ports_out[at & 3] = v;
            return;
        case 0xD: case 0xE: case 0xF:
            machine.smp.timer_out[(at & 0xF) - 0xD] = 0;
            return;
        default: break; /* $F0, $F8-$FC: plain RAM */
        }
        break;
    default:
        if (at >= 0xFFC0) return; /* the ROM's region ignores writes */
        break;
    }
    machine.smp.ram[at] = v;
}

/* the flag and arithmetic helpers run on the interpreter's locals */
static void nz(uint8_t v, uint8_t *psw) {
    *psw &= (uint8_t)~(PSW_N | PSW_Z);
    if (!v) *psw |= PSW_Z;
    if (v & 0x80) *psw |= PSW_N;
}
static void nz16(uint16_t v, uint8_t *psw) {
    *psw &= (uint8_t)~(PSW_N | PSW_Z);
    if (!v) *psw |= PSW_Z;
    if (v & 0x8000) *psw |= PSW_N;
}
static uint8_t adc(uint8_t a, uint8_t m, uint8_t *psw) {
    const unsigned r = a + m + (*psw & PSW_C);
    *psw &= (uint8_t)~(PSW_C | PSW_V | PSW_H);
    if (((a ^ r) & (m ^ r) & 0x80)) *psw |= PSW_V;
    if ((a ^ m ^ r) & 0x10) *psw |= PSW_H;
    if (r > 0xFF) *psw |= PSW_C;
    nz((uint8_t)r, psw);
    return (uint8_t)r;
}
static uint8_t sbc(uint8_t a, uint8_t m, uint8_t *psw) {
    const unsigned r = a - m - !(*psw & PSW_C);
    *psw &= (uint8_t)~(PSW_C | PSW_V | PSW_H);
    if (((a ^ r) & (a ^ m) & 0x80)) *psw |= PSW_V;
    if (!((a ^ m ^ r) & 0x10)) *psw |= PSW_H;
    if (!(r & 0x100)) *psw |= PSW_C;
    nz((uint8_t)r, psw);
    return (uint8_t)r;
}
static uint8_t cmpv(uint8_t reg, uint8_t m, uint8_t *psw) {
    *psw &= (uint8_t)~PSW_C;
    if (reg >= m) *psw |= PSW_C;
    nz((uint8_t)(reg - m), psw);
    return (uint8_t)(reg - m); /* for N/Z only, never stored */
}

/* one instruction, its cycles returned; the hot state lives in locals
 * and commits at the end, as the 65816 interpreter's does */
static unsigned smp_step(void) {
    uint16_t pc = machine.smp.pc;
    uint8_t a_ = machine.smp.a, x_ = machine.smp.x, y_ = machine.smp.y;
    uint8_t psw_ = machine.smp.psw, sp_ = machine.smp.sp;
#define RD(at) smp_read((uint16_t)(at))
#define WR(at, v) smp_write((uint16_t)(at), (v))
#define P8() RD(pc++)
#define P16() ({ const uint8_t plo_ = RD(pc++); (uint16_t)(plo_ | RD(pc++) << 8); })
#define DPB ((uint16_t)(psw_ & PSW_P ? 0x100u : 0x0000u) + (uint8_t)(d))
    const uint8_t op = RD(pc++);
    unsigned cyc = 2;
    const unsigned lo = op & 0x1F;
    uint8_t d; /* the direct-page byte of an operand, where DPB needs it */

    /* ---- the ALU matrix: OR 0x, AND 2x, EOR 4x, CMP 6x, ADC 8x, SBC Ax,
     * the same twelve operand forms each; the three memory forms write
     * back and flag from the memory result ---- */
    if (op < 0xC0 && ((lo >= 4 && lo <= 9) || (lo >= 20 && lo <= 25))) {
        uint8_t m = 0;
        switch (lo) {
        case 4: d = P8(); m = RD(DPB); cyc = 3; break;                 /* A,dp */
        case 5: m = RD(P16()); cyc = 4; break;                         /* A,abs */
        case 6: m = RD(x_); cyc = 3; break;                            /* A,(X) */
        case 7: d = P8(); { const uint16_t p = DPB;
                           m = RD(RD(p) | RD((uint16_t)(p + 1)) << 8); } cyc = 6; break;
        case 8: m = P8(); cyc = 2; break;                              /* A,#imm */
        case 20: d = P8(); m = RD(DPB + x_); cyc = 4; break;           /* A,dp+X */
        case 21: m = RD((uint16_t)(P16() + x_)); cyc = 5; break;       /* A,abs+X */
        case 22: m = RD((uint16_t)(P16() + y_)); cyc = 5; break;       /* A,abs+Y */
        case 23: d = P8(); { const uint16_t p = DPB;
                             const uint16_t q = RD(p) | RD((uint16_t)(p + 1)) << 8;
                             m = RD((uint16_t)(q + y_)); } cyc = 6; break; /* A,(dp)+Y */
        }
        if (lo == 9) { /* dp,dp */
            d = P8();
            const uint16_t dst = DPB;
            d = P8();
            const uint8_t src = RD(DPB);
            switch (op & 0xE0) {
            case 0x00: WR(dst, src | RD(dst)); break;
            case 0x20: WR(dst, src & RD(dst)); break;
            case 0x40: WR(dst, src ^ RD(dst)); break;
            case 0x60: cmpv(RD(dst), src, &psw_); break;
            case 0x80: WR(dst, adc(RD(dst), src, &psw_)); break;
            case 0xA0: WR(dst, sbc(RD(dst), src, &psw_)); break;
            }
            nz(RD(dst), &psw_);
            cyc = 6;
        } else if (lo == 24) { /* dp,#imm */
            d = P8();
            const uint16_t dst = DPB;
            const uint8_t m2 = P8();
            switch (op & 0xE0) {
            case 0x00: WR(dst, m2 | RD(dst)); break;
            case 0x20: WR(dst, m2 & RD(dst)); break;
            case 0x40: WR(dst, m2 ^ RD(dst)); break;
            case 0x60: cmpv(RD(dst), m2, &psw_); break;
            case 0x80: WR(dst, adc(RD(dst), m2, &psw_)); break;
            case 0xA0: WR(dst, sbc(RD(dst), m2, &psw_)); break;
            }
            nz(RD(dst), &psw_);
            cyc = 5;
        } else if (lo == 25) { /* (X),(Y) */
            const uint8_t src = RD(y_);
            switch (op & 0xE0) {
            case 0x00: WR(x_, src | RD(x_)); break;
            case 0x20: WR(x_, src & RD(x_)); break;
            case 0x40: WR(x_, src ^ RD(x_)); break;
            case 0x60: cmpv(RD(x_), src, &psw_); break;
            case 0x80: WR(x_, adc(RD(x_), src, &psw_)); break;
            case 0xA0: WR(x_, sbc(RD(x_), src, &psw_)); break;
            }
            nz(RD(x_), &psw_);
            cyc = 5;
        } else { /* the A forms */
            switch (op & 0xE0) {
            case 0x00: a_ |= m; break;
            case 0x20: a_ &= m; break;
            case 0x40: a_ ^= m; break;
            case 0x60: (void)cmpv(a_, m, &psw_); break;
            case 0x80: a_ = adc(a_, m, &psw_); break;
            case 0xA0: a_ = sbc(a_, m, &psw_); break;
            }
            nz(a_, &psw_);
        }
        goto commit;
    }

    switch (op) {
    /* ---- the loads ---- */
    case 0xE8: a_ = P8(); break;
    case 0xE4: d = P8(); a_ = RD(DPB); cyc = 3; break;
    case 0xF4: d = P8(); a_ = RD(DPB + x_); cyc = 4; break;
    case 0xE5: a_ = RD(P16()); cyc = 4; break;
    case 0xF5: a_ = RD((uint16_t)(P16() + x_)); cyc = 5; break;
    case 0xF6: a_ = RD((uint16_t)(P16() + y_)); cyc = 5; break;
    case 0xE7: d = P8(); { const uint16_t p = DPB;
                           a_ = RD(RD(p) | RD((uint16_t)(p + 1)) << 8); } cyc = 6; break;
    case 0xF7: d = P8(); { const uint16_t p = DPB;
                           const uint16_t q = RD(p) | RD((uint16_t)(p + 1)) << 8;
                           a_ = RD((uint16_t)(q + y_)); } cyc = 6; break;
    case 0xE6: a_ = RD(x_); cyc = 3; break;
    case 0xBF: a_ = RD(x_); x_++; cyc = 4; break;
    case 0xCD: x_ = P8(); break;
    case 0xF8: d = P8(); x_ = RD(DPB); cyc = 3; break;
    case 0xE9: x_ = RD(P16()); cyc = 4; break;
    case 0x8D: y_ = P8(); break;
    case 0xEB: d = P8(); y_ = RD(DPB); cyc = 3; break;
    case 0xFB: d = P8(); y_ = RD(DPB + x_); cyc = 4; break;
    case 0xEC: y_ = RD(P16()); cyc = 4; break;
    case 0xBA: { const uint16_t p = DPB;
                 a_ = RD(p); y_ = RD((uint16_t)(p + 1)); } cyc = 5; nz16((uint16_t)(y_ << 8 | a_), &psw_); break;
    case 0x5D: x_ = a_; break;
    case 0x7D: a_ = x_; nz(a_, &psw_); break;
    case 0xDD: a_ = y_; nz(a_, &psw_); break;
    case 0xFD: y_ = a_; nz(y_, &psw_); break;
    case 0x9D: x_ = sp_; nz(x_, &psw_); break;
    case 0xBD: sp_ = x_; break;

    /* ---- the stores ---- */
    case 0xC4: d = P8(); WR(DPB, a_); cyc = 4; break;
    case 0xD4: d = P8(); WR(DPB + x_, a_); cyc = 5; break;
    case 0xC5: WR(P16(), a_); cyc = 5; break;
    case 0xD5: WR((uint16_t)(P16() + x_), a_); cyc = 6; break;
    case 0xD6: WR((uint16_t)(P16() + y_), a_); cyc = 6; break;
    case 0xC6: WR(x_, a_); cyc = 4; break;
    case 0xAF: WR(x_, a_); x_++; cyc = 4; break;
    case 0xC7: d = P8(); { const uint16_t p = DPB;
                           WR(RD(p) | RD((uint16_t)(p + 1)) << 8, a_); } cyc = 7; break;
    case 0xD7: d = P8(); { const uint16_t p = DPB;
                           const uint16_t q = RD(p) | RD((uint16_t)(p + 1)) << 8;
                           WR((uint16_t)(q + y_), a_); } cyc = 7; break;
    case 0x8F: d = P8(); WR(DPB, P8()); cyc = 5; break;
    case 0xFA: d = P8(); { const uint16_t dst = DPB;
                           d = P8(); WR(dst, RD(DPB)); } cyc = 5; break;
    case 0xCB: d = P8(); WR(DPB, y_); cyc = 4; break;
    case 0xCC: WR(P16(), y_); cyc = 4; break;
    case 0xD8: d = P8(); WR(DPB, x_); cyc = 4; break;
    case 0xC9: WR(P16(), x_); cyc = 4; break;
    case 0xD9: d = P8(); WR(DPB + y_, x_); cyc = 5; break;
    case 0xDB: d = P8(); WR(DPB + x_, y_); cyc = 5; break;
    case 0xDA: { d = P8(); const uint16_t p = DPB; WR(p, a_); WR((uint16_t)(p + 1), y_); } cyc = 5; break;

    /* ---- the shifts and the inc/dec ---- */
    case 0x1C: psw_ &= (uint8_t)~PSW_C; if (a_ & 0x80) psw_ |= PSW_C; a_ <<= 1; nz(a_, &psw_); cyc = 2; break;
    case 0x3C: { const uint8_t c = psw_ & PSW_C; psw_ &= (uint8_t)~PSW_C;
                 if (a_ & 0x80) psw_ |= PSW_C;
                 a_ = (uint8_t)((a_ << 1) | (c ? 1 : 0)); nz(a_, &psw_); cyc = 2; break; }
    case 0x5C: psw_ &= (uint8_t)~PSW_C; if (a_ & 1) psw_ |= PSW_C; a_ >>= 1; nz(a_, &psw_); cyc = 2; break;
    case 0x7C: { const uint8_t c = psw_ & PSW_C; psw_ &= (uint8_t)~PSW_C;
                 if (a_ & 1) psw_ |= PSW_C;
                 a_ = (uint8_t)((a_ >> 1) | (c ? 0x80 : 0)); nz(a_, &psw_); cyc = 2; break; }
    case 0x0B: case 0x1B: case 0x0C: /* ASL dp / dp+X / abs */
    case 0x2B: case 0x3B: case 0x2C: /* ROL */
    case 0x4B: case 0x5B: case 0x4C: /* LSR */
    case 0x6B: case 0x7B: case 0x6C: { /* ROR */
        uint16_t at;
        if ((op & 0x0F) == 0x0C) { at = P16(); cyc = 5; }      /* abs */
        else if (op & 0x10) { d = P8(); at = DPB + x_; cyc = 5; } /* dp+X */
        else { d = P8(); at = DPB; cyc = 4; }                   /* dp */
        const uint8_t m = RD(at);
        uint8_t r_ = m;
        const uint8_t c = psw_ & PSW_C;
        psw_ &= (uint8_t)~PSW_C;
        switch ((op >> 5) & 3) {
        case 0: if (m & 0x80) psw_ |= PSW_C; r_ = (uint8_t)(m << 1); break;  /* ASL */
        case 1: if (m & 0x80) psw_ |= PSW_C;
                r_ = (uint8_t)((m << 1) | (c ? 1 : 0)); break;               /* ROL */
        case 2: if (m & 1) psw_ |= PSW_C; r_ = (uint8_t)(m >> 1); break;     /* LSR */
        case 3: if (m & 1) psw_ |= PSW_C;
                r_ = (uint8_t)((m >> 1) | (c ? 0x80 : 0)); break;            /* ROR */
        }
        WR(at, r_);
        nz(r_, &psw_);
        break;
    }
    case 0xAB: d = P8(); { const uint16_t at = DPB; const uint8_t r_ = (uint8_t)(RD(at) + 1); WR(at, r_); nz(r_, &psw_); } cyc = 4; break;
    case 0xBB: d = P8(); { const uint16_t at = DPB + x_; const uint8_t r_ = (uint8_t)(RD(at) + 1); WR(at, r_); nz(r_, &psw_); } cyc = 5; break;
    case 0xAC: { const uint16_t at = P16(); const uint8_t r_ = (uint8_t)(RD(at) + 1); WR(at, r_); nz(r_, &psw_); } cyc = 5; break;
    case 0x8B: d = P8(); { const uint16_t at = DPB; const uint8_t r_ = (uint8_t)(RD(at) - 1); WR(at, r_); nz(r_, &psw_); } cyc = 4; break;
    case 0x9B: d = P8(); { const uint16_t at = DPB + x_; const uint8_t r_ = (uint8_t)(RD(at) - 1); WR(at, r_); nz(r_, &psw_); } cyc = 5; break;
    case 0x8C: { const uint16_t at = P16(); const uint8_t r_ = (uint8_t)(RD(at) - 1); WR(at, r_); nz(r_, &psw_); } cyc = 5; break;
    case 0xBC: a_++; nz(a_, &psw_); break;
    case 0x9C: a_--; nz(a_, &psw_); break;
    case 0x3D: x_++; nz(x_, &psw_); break;
    case 0x1D: x_--; nz(x_, &psw_); break;
    case 0xFC: y_++; nz(y_, &psw_); break;
    case 0xDC: y_--; nz(y_, &psw_); break;
    case 0x3A: case 0x1A: { /* INCW/DECW dp */
        d = P8();
        const uint16_t p = DPB;
        uint16_t w = (uint16_t)(RD(p) | RD((uint16_t)(p + 1)) << 8);
        w = op == 0x3A ? (uint16_t)(w + 1) : (uint16_t)(w - 1);
        WR(p, (uint8_t)w); WR((uint16_t)(p + 1), (uint8_t)(w >> 8));
        cyc = 5;
        break;
    }

    /* ---- the 16-bit ALU and the X/Y compares ---- */
    case 0x7A: { d = P8(); const uint16_t p = DPB;
                 const unsigned ya = (unsigned)y_ << 8 | a_;
                 const unsigned m = RD(p) | RD((uint16_t)(p + 1)) << 8;
                 const unsigned r = ya + m;
                 psw_ &= (uint8_t)~(PSW_C | PSW_V | PSW_H);
                 if (((ya ^ r) & (m ^ r) & 0x8000)) psw_ |= PSW_V;
                 if ((ya ^ m ^ r) & 0x1000) psw_ |= PSW_H;
                 if (r > 0xFFFF) psw_ |= PSW_C;
                 y_ = (uint8_t)(r >> 8); a_ = (uint8_t)r;
                 nz16((uint16_t)r, &psw_); cyc = 5; break; }
    case 0x9A: { d = P8(); const uint16_t p = DPB;
                 const unsigned ya = (unsigned)y_ << 8 | a_;
                 const unsigned m = RD(p) | RD((uint16_t)(p + 1)) << 8;
                 const unsigned r = ya - m;
                 psw_ &= (uint8_t)~(PSW_C | PSW_V | PSW_H);
                 if (((ya ^ r) & (ya ^ m) & 0x8000)) psw_ |= PSW_V;
                 if (!((ya ^ m ^ r) & 0x1000)) psw_ |= PSW_H;
                 if (!(r & 0x10000)) psw_ |= PSW_C;
                 y_ = (uint8_t)(r >> 8); a_ = (uint8_t)r;
                 nz16((uint16_t)r, &psw_); cyc = 5; break; }
    case 0x5A: { d = P8(); const uint16_t p = DPB;
                 const unsigned ya = (unsigned)y_ << 8 | a_;
                 const unsigned m = RD(p) | RD((uint16_t)(p + 1)) << 8;
                 const unsigned dff = ya - m;
                 psw_ &= (uint8_t)~(PSW_C | PSW_N | PSW_Z);
                 if (ya >= m) psw_ |= PSW_C;
                 if (!dff) psw_ |= PSW_Z;
                 if (dff & 0x8000) psw_ |= PSW_N;
                 cyc = 4; break; }
    case 0xC8: { const uint8_t m = P8(); (void)cmpv(x_, m, &psw_); break; }
    case 0xA8: { const uint8_t m = P8(); (void)cmpv(y_, m, &psw_); break; }
    case 0x3E: { d = P8(); (void)cmpv(x_, RD(DPB), &psw_); cyc = 3; break; }
    case 0x1E: { const uint8_t m = RD((uint16_t)(P16() + x_)); (void)cmpv(x_, m, &psw_); cyc = 4; break; }
    case 0x7E: { d = P8(); (void)cmpv(y_, RD(DPB), &psw_); cyc = 3; break; }
    case 0x5E: { const uint8_t m = RD((uint16_t)(P16() + y_)); (void)cmpv(y_, m, &psw_); cyc = 4; break; }

    /* ---- the special arithmetic ---- */
    case 0xCF: { const unsigned r = y_ * a_;
                 y_ = (uint8_t)(r >> 8); a_ = (uint8_t)r;
                 nz(y_, &psw_); cyc = 9; break; }                     /* MUL */
    case 0x9E: { /* DIV */
        const unsigned ya = (unsigned)y_ << 8 | a_;
        psw_ &= (uint8_t)~(PSW_V | PSW_H);
        if (!x_ || y_ >= x_) {
            psw_ |= PSW_V;
            if (!x_) psw_ |= PSW_H;
            a_ = 0xFF; y_ = 0xFF;
        } else {
            a_ = (uint8_t)(ya / x_);
            y_ = (uint8_t)(ya % x_);
        }
        nz(a_, &psw_);
        cyc = 12;
        break;
    }
    case 0x9F: a_ = (uint8_t)((a_ >> 4) | (a_ << 4)); nz(a_, &psw_); cyc = 5; break; /* XCN */
    case 0xDF: { /* DAA */
        unsigned r = a_;
        const uint8_t h = psw_ & PSW_H, c = psw_ & PSW_C;
        if ((a_ & 0xF) > 9 || h) r += 0x06;
        if ((a_ & 0xF0) > 0x80 || (a_ & 0x0F) > 9 || c) r += 0x60;
        psw_ &= (uint8_t)~(PSW_C | PSW_H);
        if (r > 0xFF) psw_ |= PSW_C;
        if ((a_ ^ r) & 0x10) psw_ |= PSW_H;
        a_ = (uint8_t)r;
        nz(a_, &psw_);
        cyc = 3;
        break;
    }
    case 0xBE: { /* DAS */
        unsigned r = a_;
        const uint8_t h = psw_ & PSW_H, c = psw_ & PSW_C;
        if ((a_ & 0xF) > 9 || !h) r -= 0x06;
        if ((a_ & 0xF0) > 0x80 || (a_ & 0x0F) > 9 || !c) r -= 0x60;
        psw_ &= (uint8_t)~(PSW_C | PSW_H);
        if (!(r & 0x100)) psw_ |= PSW_C;
        if (!((a_ ^ r) & 0x10)) psw_ |= PSW_H;
        a_ = (uint8_t)r;
        nz(a_, &psw_);
        cyc = 3;
        break;
    }

    /* ---- the flags ---- */
    case 0x60: psw_ &= (uint8_t)~PSW_C; break; /* CLRC */
    case 0x80: psw_ |= PSW_C; break;           /* SETC */
    case 0xED: psw_ ^= PSW_C; cyc = 3; break;  /* NOTC */
    case 0xE0: psw_ &= (uint8_t)~(PSW_V | PSW_H); break; /* CLRV */
    case 0x20: psw_ &= (uint8_t)~PSW_P; break; /* CLRP */
    case 0x40: psw_ |= PSW_P; break;           /* SETP */
    case 0xA0: psw_ |= PSW_I; break;           /* EI */
    case 0xC0: psw_ &= (uint8_t)~PSW_I; break; /* DI */

    /* ---- the stack ---- */
    case 0x2D: smp_push(a_); cyc = 4; break;
    case 0x4D: smp_push(x_); cyc = 4; break;
    case 0x6D: smp_push(y_); cyc = 4; break;
    case 0x0D: smp_push(psw_ | PSW_B); cyc = 4; break;
    case 0xAE: a_ = smp_pop(); nz(a_, &psw_); cyc = 4; break;
    case 0xCE: x_ = smp_pop(); nz(x_, &psw_); cyc = 4; break;
    case 0xEE: y_ = smp_pop(); nz(y_, &psw_); cyc = 4; break;
    case 0x8E: psw_ = smp_pop() | 0x30; cyc = 4; break;

    /* ---- the jumps and the calls ---- */
    case 0x5F: pc = P16(); cyc = 3; break;
    case 0x1F: { const uint16_t b = P16();
                 pc = (uint16_t)(RD((uint16_t)(b + x_)) | RD((uint16_t)(b + x_ + 1)) << 8);
                 cyc = 6; break; }
    case 0x3F: { const uint16_t dd = P16();
                 smp_push((uint8_t)(pc >> 8)); smp_push((uint8_t)pc);
                 pc = dd; cyc = 8; break; }                            /* CALL */
    case 0x4F: { const uint16_t dd = 0xFF00u | P8();
                 smp_push((uint8_t)(pc >> 8)); smp_push((uint8_t)pc);
                 pc = dd; cyc = 6; break; }                            /* PCALL */
    case 0x11: case 0x31: case 0x51: case 0x71:
    case 0x91: case 0xB1: case 0xD1: case 0xF1: {
        const uint16_t v = (uint16_t)(0xFFDEu - 2u * ((op >> 4) - 1));
        const uint8_t vlo = RD(v);
        smp_push((uint8_t)(pc >> 8)); smp_push((uint8_t)pc);
        pc = (uint16_t)(vlo | RD((uint16_t)(v + 1)) << 8);
        cyc = 8;
        break;
    }                                                                    /* TCALL */
    case 0x6F: { const uint8_t wlo = smp_pop();
                 pc = (uint16_t)(wlo | smp_pop() << 8); cyc = 5; break; } /* RET */
    case 0x7F: { psw_ = (uint8_t)((smp_pop() | 0x30) & ~PSW_B);
                 const uint8_t wlo = smp_pop();
                 pc = (uint16_t)(wlo | smp_pop() << 8); cyc = 6; break; } /* RETI */
    case 0x0F: { /* BRK */
        smp_push((uint8_t)(pc >> 8)); smp_push((uint8_t)pc);
        smp_push(psw_ | PSW_B);
        psw_ |= PSW_I;
        pc = (uint16_t)(RD(0xFFFEu) | RD(0xFFFFu) << 8);
        cyc = 8;
        break;
    }

    /* ---- the branches ---- */
    case 0x10: { const int8_t off = (int8_t)P8(); if (!(psw_ & PSW_N)) pc = (uint16_t)(pc + off); cyc = !(psw_ & PSW_N) ? 4 : 2; break; }
    case 0x30: { const int8_t off = (int8_t)P8(); if (psw_ & PSW_N) pc = (uint16_t)(pc + off); cyc = (psw_ & PSW_N) ? 4 : 2; break; }
    case 0x50: { const int8_t off = (int8_t)P8(); if (!(psw_ & PSW_V)) pc = (uint16_t)(pc + off); cyc = !(psw_ & PSW_V) ? 4 : 2; break; }
    case 0x70: { const int8_t off = (int8_t)P8(); if (psw_ & PSW_V) pc = (uint16_t)(pc + off); cyc = (psw_ & PSW_V) ? 4 : 2; break; }
    case 0x90: { const int8_t off = (int8_t)P8(); if (!(psw_ & PSW_C)) pc = (uint16_t)(pc + off); cyc = !(psw_ & PSW_C) ? 4 : 2; break; }
    case 0xB0: { const int8_t off = (int8_t)P8(); if (psw_ & PSW_C) pc = (uint16_t)(pc + off); cyc = (psw_ & PSW_C) ? 4 : 2; break; }
    case 0xD0: { const int8_t off = (int8_t)P8(); if (!(psw_ & PSW_Z)) pc = (uint16_t)(pc + off); cyc = !(psw_ & PSW_Z) ? 4 : 2; break; }
    case 0xF0: { const int8_t off = (int8_t)P8(); if (psw_ & PSW_Z) pc = (uint16_t)(pc + off); cyc = (psw_ & PSW_Z) ? 4 : 2; break; }
    case 0x2F: { const int8_t off = (int8_t)P8(); pc = (uint16_t)(pc + off); cyc = 4; break; } /* BRA */
    case 0xFE: { y_--; nz(y_, &psw_); const int8_t off = (int8_t)P8();
                 if (y_) pc = (uint16_t)(pc + off); cyc = y_ ? 6 : 4; break; } /* DBNZ Y */
    case 0x6E: { d = P8(); const uint16_t p = DPB;
                 const uint8_t v = (uint8_t)(RD(p) - 1);
                 WR(p, v); nz(v, &psw_);
                 const int8_t off = (int8_t)P8();
                 if (v) pc = (uint16_t)(pc + off);
                 cyc = v ? 7 : 5; break; }                            /* DBNZ dp */
    case 0x2E: { d = P8(); const uint8_t m = RD(DPB);
                 const int8_t off = (int8_t)P8();
                 if (a_ != m) pc = (uint16_t)(pc + off);
                 cyc = a_ != m ? 7 : 5; break; }                      /* CBNE dp */
    case 0xDE: { d = P8(); const uint8_t m = RD(DPB + x_);
                 const int8_t off = (int8_t)P8();
                 if (a_ != m) pc = (uint16_t)(pc + off);
                 cyc = a_ != m ? 8 : 6; break; }                      /* CBNE dp+X */
    case 0x03: case 0x23: case 0x43: case 0x63:
    case 0x83: case 0xA3: case 0xC3: case 0xE3:
    case 0x13: case 0x33: case 0x53: case 0x73:
    case 0x93: case 0xB3: case 0xD3: case 0xF3: { /* BBS/BBC dp.b */
        d = P8();
        const unsigned bit = (op >> 5) & 7;
        const int set = (RD(DPB) >> bit) & 1;
        const int take = op & 0x10 ? !set : set;
        const int8_t off = (int8_t)P8();
        if (take) pc = (uint16_t)(pc + off);
        cyc = take ? 7 : 5;
        break;
    }

    /* ---- the bit operations ---- */
    case 0x02: case 0x22: case 0x42: case 0x62:
    case 0x82: case 0xA2: case 0xC2: case 0xE2: {
        d = P8();
        WR(DPB, RD(DPB) | (1u << ((op >> 5) & 7)));
        cyc = 4;
        break;
    }                                                                    /* SET1 */
    case 0x12: case 0x32: case 0x52: case 0x72:
    case 0x92: case 0xB2: case 0xD2: case 0xF2: {
        d = P8();
        WR(DPB, RD(DPB) & (uint8_t)~(1u << ((op >> 5) & 7)));
        cyc = 4;
        break;
    }                                                                    /* CLR1 */
    case 0xEA: { /* NOT1 abs.b */
        const uint8_t al = P8();
        const uint8_t ah = P8();
        const uint16_t at = (uint16_t)(al | (ah & 0x1F) << 8);
        WR(at, RD(at) ^ (1u << (ah >> 5)));
        cyc = 5;
        break;
    }
    case 0x0A: case 0x2A: case 0x4A: case 0x6A:
    case 0x8A: case 0xAA: case 0xCA: { /* the carry/bit group */
        const uint8_t al = P8();
        const uint8_t ah = P8();
        const uint16_t at = (uint16_t)(al | (ah & 0x1F) << 8);
        const unsigned bit = ah >> 5;
        const unsigned b = (RD(at) >> bit) & 1;
        switch (op) {
        case 0x0A: if (b) psw_ |= PSW_C; cyc = 5; break;               /* OR1 */
        case 0x2A: if (!b) psw_ |= PSW_C; cyc = 5; break;              /* OR1 / */
        case 0x4A: if (!b) psw_ &= (uint8_t)~PSW_C; cyc = 4; break;    /* AND1 */
        case 0x6A: if (b) psw_ &= (uint8_t)~PSW_C; cyc = 4; break;     /* AND1 / */
        case 0x8A: psw_ ^= (uint8_t)(b ? PSW_C : 0); cyc = 5; break;   /* EOR1 */
        case 0xAA: psw_ = (psw_ & (uint8_t)~PSW_C) | (b ? PSW_C : 0); cyc = 4; break;
        case 0xCA: WR(at, b ? RD(at) | (1u << bit) : RD(at) & (uint8_t)~(1u << bit));
                   cyc = 6; break;
        }
        break;
    }

    /* ---- the stops and the nop ---- */
    case 0xEF: machine.smp.halted = 1; cyc = 3; break;                 /* SLEEP */
    case 0xFF: machine.smp.halted = 2; cyc = 3; break;                 /* STOP */
    case 0x00: break;                                                  /* NOP */
    default: break; /* the bytes the walk assigned nothing behave as NOP 2 */
    }

#undef P16
#undef P8
#undef DPB
#undef RD
#undef WR
commit:
    machine.smp.pc = pc;
    machine.smp.a = a_;
    machine.smp.x = x_;
    machine.smp.y = y_;
    machine.smp.sp = sp_;
    machine.smp.psw = psw_;
    return cyc;
}

#undef PSW_N
#undef PSW_V
#undef PSW_P
#undef PSW_B
#undef PSW_H
#undef PSW_I
#undef PSW_Z
#undef PSW_C

/* the CPU-side comm port: the simulated IPL state machine (the real 64
 * bytes are Nintendo ROM, never shipped here) lives on these writes,
 * following the boot ROM's own protocol: at reset the S-SMP answers $AA
 * on port 0 and $BB on port 1; a $CC on port 0 (sent last) takes the
 * $2142/3 word as either the next block's address (high byte nonzero) or
 * the entry point (high byte zero, execution starts); during a block the
 * SNES sends each byte on port 1 with its counter on port 0, the counter
 * equal to the IPL's means store and echo, a counter signed-ahead means
 * the block ended and the ROM waits for the next $CC. */
void snes_smp_port_write(unsigned n, uint8_t v) {
    snes_smp_t *s = &machine.smp;
    s->ports_in[n] = v;
    if (n == 2) {
        s->ipl_addr = (uint16_t)((s->ipl_addr & 0xFF00u) | v);
        return;
    }
    if (n == 3) {
        s->ipl_addr = (uint16_t)((s->ipl_addr & 0x00FFu) | (uint16_t)v << 8);
        return;
    }
    if (n == 0) {
        if (s->ipl_state == 2) { /* inside a block: counter semantics */
            if (v == s->ipl_y) {
                if (s->ipl_fresh1) { /* the byte arrived first: commit it */
                    s->ipl_fresh1 = 0;
                    s->ram[(uint16_t)(s->ipl_addr + s->ipl_y)] = s->ports_in[1];
                    s->ports_out[0] = s->ipl_y;
                    if (++s->ipl_y == 0) /* the counter wrapped: next page */
                        s->ipl_addr = (uint16_t)(s->ipl_addr + 0x100u);
                } else {
                    s->ipl_pending = 1; /* the byte follows on port 1 */
                }
            } else if ((int8_t)(v - s->ipl_y) > 0) { /* the block's terminate */
                s->ports_out[0] = v;
                s->ports_out[1] = 0xBB;
                s->ipl_pending = 0;
                if (s->ports_in[1] == 0) { /* port 1 clear: jump to $2142/3 */
                    s->pc = s->ipl_addr;
                    s->halted = 0;
                    s->ipl_state = 3;
                } else { /* the next block starts at the $2142/3 word,
                          * its counter back at zero */
                    s->ipl_y = 0;
                }
            }
            return;
        }
        if (v == 0xCC && s->ipl_state != 3) { /* the transfer or go command */
            s->ports_out[0] = 0xCC;
            s->ports_out[1] = 0xBB;
            s->ipl_pending = 0;
            s->ipl_fresh1 = 0; /* the command's own port-1 byte is not data */
            if (s->ports_in[1] != 0) { /* port 1 nonzero: block upload at $2142/3 */
                s->ipl_state = 2;
                s->ipl_y = 0;
            } else { /* entry point: the uploaded program takes over */
                s->pc = s->ipl_addr;
                s->halted = 0;
                s->ipl_state = 3;
            }
        }
        return;
    }
    if (n == 1 && s->ipl_state == 2 && (s->ipl_pending || s->ports_in[0] == s->ipl_y)) {
        /* the byte arrives with its counter already on port 0 */
        s->ipl_pending = 0;
        s->ipl_fresh1 = 0;
        s->ram[(uint16_t)(s->ipl_addr + s->ipl_y)] = v;
        s->ports_out[0] = s->ipl_y;
        if (++s->ipl_y == 0)
            s->ipl_addr = (uint16_t)(s->ipl_addr + 0x100u);
    } else if (n == 1) {
        s->ipl_fresh1 = 1; /* the byte precedes its counter */
    }
}

/* the timers tick at 8 kHz (T0/T1) and 64 kHz (T2); the outputs count at
 * source/(target+1) while the run bits of $F1 stand */
static void smp_tick8k(void) {
    for (unsigned t = 0; t < 2; t++) {
        if (!(machine.smp.control & (1u << t))) continue;
        if (++machine.smp.timer_phase[t] > machine.smp.ram[0xFA + t]) {
            machine.smp.timer_phase[t] = 0;
            machine.smp.timer_out[t]++;
        }
    }
}
static void smp_tick64k(void) {
    if (!(machine.smp.control & 4)) return;
    if (++machine.smp.timer_phase[2] > machine.smp.ram[0xFC]) {
        machine.smp.timer_phase[2] = 0;
        machine.smp.timer_out[2]++;
    }
}

/* advance by `cpu_cycles` of the 65816's clock: the SPC700 runs unless
 * SLEEP/STOP holds it, the timers tick either way, and the DSP renders
 * one stereo sample per 32 SPC cycles (1024000/32000) */
void snes_apu_run(uint32_t cpu_cycles) {
    snes_smp_t *s = &machine.smp;
    s->acc += (uint64_t)cpu_cycles * 1024000ULL;
    uint64_t ticks = s->acc / 3579545ULL;
    s->acc %= 3579545ULL;
    while (ticks) {
        const uint64_t chunk = ticks < 16 ? ticks : 16;
        ticks -= chunk;
        s->tick64k = (uint8_t)(s->tick64k + chunk);
        while (s->tick64k >= 16) { s->tick64k -= 16; smp_tick64k(); }
        s->tick8k = (uint8_t)(s->tick8k + chunk);
        while (s->tick8k >= 128) { s->tick8k -= 128; smp_tick8k(); }
        s->dsp_acc += chunk;
        while (s->dsp_acc >= 32) { s->dsp_acc -= 32; snes_dsp_sample(); }
        if (s->halted) continue; /* SLEEP/STOP: time passes, nothing runs */
        int64_t left = (int64_t)chunk;
        while (left > 0)
            left -= smp_step(); /* an instruction overruns the slice */
    }
}

/* drain n stereo samples for the host */
unsigned snes_apu_read(int16_t *dst, unsigned n) {
    snes_dsp_t *d = &machine.dsp;
    unsigned got = 0;
    while (got < n && d->ring_r != d->ring_w) {
        *dst++ = d->ring[d->ring_r];
        d->ring_r = (d->ring_r + 1) & 8191;
        got++;
    }
    while (got < n) { *dst++ = 0; got++; } /* underrun: silence */
    return got;
}
