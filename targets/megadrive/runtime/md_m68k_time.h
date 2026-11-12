/* 68000 instruction times, from the MC68000 user's manual (section 8):
 * md_m68k_cycles() is everything an opcode costs that the opcode alone
 * decides, effective-address times included.  What depends on data adds to
 * it where it is known: a branch taken or not (md_m68k_branch_*), a DBcc
 * outcome, Scc true, a shift by a register count, MOVEM's register count,
 * MULU/MULS bits and DIVU/DIVS steps (md_m68k_mul/div_cycles).
 *
 * Pure C, no state: the interpreter (md_full_cpu.c) charges these at run
 * time and the 68000 -> C emitter (recompiler/src/system/megadrive) folds
 * the same values into generated code, so both count the same cycles. */
#ifndef M0BOY_MD_M68K_TIME_H
#define M0BOY_MD_M68K_TIME_H
#include <stdint.h>

/* Effective-address calculation and operand fetch (table 8-1); `lng` for
 * a long operand.  Modes 7.0-7.4: abs.w, abs.l, d16(PC), d8(PC,Xn), #imm. */
static inline unsigned md_m68k_ea(unsigned mode, unsigned reg, int lng) {
    static const uint8_t t[12] = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
    const unsigned i = mode < 7u ? mode : 7u + reg;
    if (i > 11u) return 0;
    return t[i] + (lng && i >= 2u ? 4u : 0u);
}
/* The destination half of MOVE: no predecrement penalty. */
static inline unsigned md_m68k_move_dst(unsigned mode, unsigned reg, int lng) {
    if (mode <= 1u) return 0;
    return (mode == 4u ? 4u : md_m68k_ea(mode, reg, 0)) + (lng ? 4u : 0u);
}
/* Control addressing (JMP/JSR/LEA/PEA/MOVEM): index 0 (An) .. 6 d8(PC,Xn). */
static inline int md_m68k_control(unsigned mode, unsigned reg) {
    if (mode == 2u) return 0;
    if (mode == 5u) return 1;
    if (mode == 6u) return 2;
    if (mode == 7u && reg <= 3u) return 3 + (int)reg;
    return -1;
}
static inline int md_m68k_is_imm(unsigned mode, unsigned reg) { return mode == 7u && reg == 4u; }

static inline unsigned md_m68k_cycles(uint16_t op) {
    const unsigned top = op >> 12, mode = (op >> 3) & 7u, reg = op & 7u;
    const unsigned size = (op >> 6) & 3u, lng = size == 2u;
    switch (top) {
    case 1: case 2: case 3: {
        const int l = top == 2u;
        return 4u + md_m68k_ea(mode, reg, l) + md_m68k_move_dst((op >> 6) & 7u, (op >> 9) & 7u, l);
    }
    case 7: return 4;
    case 6: {
        const unsigned cc = (op >> 8) & 15u;
        if (cc == 0u) return 10;
        if (cc == 1u) return 18;
        return (op & 255u) ? 8u : 12u;          /* not taken; taken: md_m68k_branch_taken */
    }
    case 5:
        if (size == 3u) {
            if (mode == 1u) return 12;            /* DBcc with cc true */
            return mode == 0u ? 4u : 8u + md_m68k_ea(mode, reg, 0);   /* Scc false (Dn) */
        }
        if (mode == 1u) return 8;                 /* ADDQ/SUBQ to An */
        if (mode == 0u) return lng ? 8u : 4u;
        return (lng ? 12u : 8u) + md_m68k_ea(mode, reg, lng);
    case 0: {
        if ((op & 0x0100u) || (op & 0x0f00u) == 0x0800u) {
            if ((op & 0x0138u) == 0x0108u) return lng ? 24u : 16u;     /* MOVEP */
            const unsigned kind = (op >> 6) & 3u, dynamic = !!(op & 0x0100u);
            if (mode == 0u)
                return (kind == 0u ? 6u : kind == 2u ? 8u : 6u) + (dynamic ? 0u : 4u);   /* +2 when the bit is >= 16: charged at run time */
            return (kind == 0u ? 4u : 8u) + (dynamic ? 0u : 4u) + md_m68k_ea(mode, reg, 0);
        }
        if ((op & 0x00ffu) == 0x003cu || (op & 0x00ffu) == 0x007cu) return 20;   /* to CCR/SR */
        const unsigned kind = (op >> 9) & 7u;
        if (kind == 6u)                              /* CMPI */
            return mode == 0u ? (lng ? 14u : 8u) : (lng ? 12u : 8u) + md_m68k_ea(mode, reg, lng);
        if (mode == 0u) return lng ? (kind == 1u ? 14u : 16u) : 8u;
        return (lng ? 20u : 12u) + md_m68k_ea(mode, reg, lng);
    }
    case 4: {
        switch (op) {
        case 0x4e70: return 132;                     /* RESET */
        case 0x4e71: return 4;
        case 0x4e72: return 4;                       /* STOP */
        case 0x4e73: return 20;                      /* RTE */
        case 0x4e75: return 16;
        case 0x4e76: return 4;                       /* TRAPV not taken */
        case 0x4e77: return 20;                      /* RTR */
        default: break;
        }
        if ((op & 0xfff0u) == 0x4e40u) return 34;    /* TRAP */
        if ((op & 0xfff8u) == 0x4e50u) return 16;    /* LINK */
        if ((op & 0xfff8u) == 0x4e58u) return 12;    /* UNLK */
        if ((op & 0xfff0u) == 0x4e60u) return 4;     /* MOVE USP */
        const int ctl = md_m68k_control(mode, reg);
        if ((op & 0xffc0u) == 0x4e80u) { static const uint8_t t[7] = {16, 18, 22, 18, 20, 18, 22}; return ctl < 0 ? 4u : t[ctl]; }
        if ((op & 0xffc0u) == 0x4ec0u) { static const uint8_t t[7] = {8, 10, 14, 10, 12, 10, 14}; return ctl < 0 ? 4u : t[ctl]; }
        if ((op & 0xf1c0u) == 0x41c0u) { static const uint8_t t[7] = {4, 8, 12, 8, 12, 8, 12}; return ctl < 0 ? 4u : t[ctl]; }
        if ((op & 0xf1c0u) == 0x4180u) return 10u + md_m68k_ea(mode, reg, 0);    /* CHK */
        if ((op & 0xfff8u) == 0x4840u) return 4;     /* SWAP */
        if ((op & 0xffc0u) == 0x4840u) { static const uint8_t t[7] = {12, 16, 20, 16, 20, 16, 20}; return ctl < 0 ? 4u : t[ctl]; }
        if ((op & 0xffb8u) == 0x4880u) return 4;     /* EXT */
        if ((op & 0xfb80u) == 0x4880u) {             /* MOVEM: + 4 or 8 per register */
            static const uint8_t t[7] = {0, 4, 6, 4, 8, 4, 6};
            if (mode == 3u || mode == 4u) return (op & 0x0400u) ? 12u : 8u;
            return ((op & 0x0400u) ? 12u : 8u) + (ctl < 0 ? 0u : t[ctl]);
        }
        if ((op & 0xffc0u) == 0x40c0u) return mode == 0u ? 6u : 8u + md_m68k_ea(mode, reg, 0);   /* from SR */
        if ((op & 0xffc0u) == 0x44c0u || (op & 0xffc0u) == 0x46c0u) return 12u + md_m68k_ea(mode, reg, 0);
        if ((op & 0xffc0u) == 0x4800u) return mode == 0u ? 6u : 8u + md_m68k_ea(mode, reg, 0);   /* NBCD */
        if ((op & 0xffc0u) == 0x4ac0u) return mode == 0u ? 4u : 10u + md_m68k_ea(mode, reg, 0);  /* TAS */
        if ((op & 0xff00u) == 0x4a00u) return 4u + md_m68k_ea(mode, reg, lng);                 /* TST */
        /* NEGX, CLR, NEG, NOT */
        if (mode == 0u) return lng ? 6u : 4u;
        return (lng ? 12u : 8u) + md_m68k_ea(mode, reg, lng);
    }
    case 8: case 9: case 11: case 12: case 13: {
        const unsigned opmode = (op >> 6) & 7u;
        const int l = opmode == 2u || opmode == 6u || opmode == 7u;
        if ((top == 9u || top == 11u || top == 13u) && (opmode == 3u || opmode == 7u)) {
            if (top == 11u) return 6u + md_m68k_ea(mode, reg, opmode == 7u);    /* CMPA */
            if (opmode == 3u) return 8u + md_m68k_ea(mode, reg, 0);             /* ADDA.W/SUBA.W */
            return (mode <= 1u || md_m68k_is_imm(mode, reg) ? 8u : 6u) + md_m68k_ea(mode, reg, 1);
        }
        if ((top == 8u || top == 12u) && (opmode == 3u || opmode == 7u))
            return (top == 12u ? 38u : 0u) + md_m68k_ea(mode, reg, 0);          /* MUL / DIV: + data */
        if ((top == 8u || top == 12u) && opmode == 4u && mode <= 1u) return mode ? 18u : 6u;  /* ABCD/SBCD */
        if (top == 12u && (op & 0x0130u) == 0x0100u && (opmode == 5u || opmode == 6u)) return 6;  /* EXG */
        if ((top == 9u || top == 13u) && opmode >= 4u && mode <= 1u)            /* ADDX/SUBX */
            return mode ? (l ? 30u : 18u) : (l ? 8u : 4u);
        if (top == 11u && opmode >= 4u && mode == 1u) return l ? 20u : 12u;    /* CMPM */
        if (top == 11u && opmode >= 4u)                                          /* EOR */
            return mode == 0u ? (l ? 8u : 4u) : (l ? 12u : 8u) + md_m68k_ea(mode, reg, l);
        if (top == 11u) return (l ? 6u : 4u) + md_m68k_ea(mode, reg, l);        /* CMP */
        if (opmode <= 2u)                                                        /* <ea>,Dn */
            return (l ? (mode <= 1u || md_m68k_is_imm(mode, reg) ? 8u : 6u) : 4u) + md_m68k_ea(mode, reg, l);
        return (l ? 12u : 8u) + md_m68k_ea(mode, reg, l);                       /* Dn,<ea> */
    }
    case 14:
        if (size == 3u) return 8u + md_m68k_ea(mode, reg, 0);                   /* memory, one bit */
        if (op & 0x0020u) return lng ? 8u : 6u;                                 /* + 2 per count */
        return (lng ? 8u : 6u) + 2u * ((((op >> 9) & 7u) - 1u) & 7u) + 2u;
    default:
        return 34;                                   /* line A/F exceptions */
    }
}

/* Bcc taken instead of not: .b 10 rather than 8, .w 10 rather than 12. */
static inline int md_m68k_branch_taken(uint16_t op) { return (op & 255u) ? 2 : -2; }
/* DBcc with cc false: -2 when it loops (10), +2 when the count expires (14). */
enum { MD_DBCC_LOOP = -2, MD_DBCC_EXPIRED = 2, MD_SCC_TRUE_DN = 2 };
static inline unsigned md_m68k_mulu_cycles(uint16_t src) {
    unsigned n = 0;
    for (; src; src &= (uint16_t)(src - 1u)) ++n;
    return 2u * n;
}
static inline unsigned md_m68k_muls_cycles(uint16_t src) {
    unsigned n = 0, x = (unsigned)src << 1;
    for (unsigned i = 0; i < 16u; ++i, x >>= 1) n += ((x ^ (x >> 1)) & 1u);
    return 2u * n;
}
/* DIVU/DIVS steps (Jorge Cwik's derivation of the microcode); overflow
 * included, divide by zero excluded (a trap). */
static inline unsigned md_m68k_divu_cycles(uint32_t dividend, uint16_t divisor) {
    if ((dividend >> 16) >= divisor) return 10;
    unsigned mcycles = 38;
    const uint32_t hdivisor = (uint32_t)divisor << 16;
    for (int i = 0; i < 15; ++i) {
        const uint32_t temp = dividend;
        dividend <<= 1;
        if (temp & 0x80000000u) dividend -= hdivisor;
        else {
            mcycles += 2;
            if (dividend >= hdivisor) { dividend -= hdivisor; --mcycles; }
        }
    }
    return mcycles * 2u;
}
static inline unsigned md_m68k_divs_cycles(int32_t dividend, int16_t divisor) {
    unsigned mcycles = 6;
    if (dividend < 0) ++mcycles;
    const uint32_t ad = dividend < 0 ? 0u - (uint32_t)dividend : (uint32_t)dividend;
    const uint32_t ads = divisor < 0 ? (uint32_t)(-(int32_t)divisor) : (uint32_t)divisor;
    if ((ad >> 16) >= ads) return (mcycles + 2u) * 2u;
    uint32_t aquot = ad / ads;
    mcycles += 55;
    if (divisor >= 0) { if (dividend >= 0) --mcycles; else ++mcycles; }
    for (int i = 0; i < 15; ++i) { if (!(aquot & 0x8000u)) ++mcycles; aquot <<= 1; }
    return mcycles * 2u;
}

/* An idle loop: TST, BTST #n or CMPI on an absolute work-RAM address, then
 * a short Bcc back to it -- a game waiting for its interrupt.  Nothing can
 * change that RAM before the slice ends (interrupts are only taken between
 * slices), so the spin is spent at once.  Returns the length in bytes of
 * the test (the Bcc follows it), 0 when `pc` does not start such a loop. */
static inline unsigned md_m68k_idle_loop(const uint8_t *rom, uint32_t size, uint32_t pc) {
    if ((pc & 1u) || pc + 10u > size) return 0;
    const uint16_t op = (uint16_t)(rom[pc] << 8 | rom[pc + 1u]);
    const unsigned mode = (op >> 3) & 7u, reg = op & 7u, sz = (op >> 6) & 3u;
    if (mode != 7u || reg > 1u) return 0;
    unsigned len, at;
    if ((op & 0xff00u) == 0x4a00u && sz != 3u) { len = 2; at = pc + 2u; }
    else if ((op & 0xfffeu) == 0x0838u) { len = 4; at = pc + 4u; }
    else if ((op & 0xff00u) == 0x0c00u && sz != 3u) { len = sz == 2u ? 6u : 4u; at = pc + len; }
    else return 0;
    if (at + 4u + 2u > size) return 0;
    uint32_t addr;
    if (reg == 0u) { addr = (uint32_t)(int32_t)(int16_t)(rom[at] << 8 | rom[at + 1u]); len += 2u; }
    else { addr = (uint32_t)rom[at] << 24 | (uint32_t)rom[at + 1u] << 16 | (uint32_t)rom[at + 2u] << 8 | rom[at + 3u]; len += 4u; }
    if ((addr & 0xffffffu) < 0xff0000u) return 0;
    const uint32_t b = pc + len;
    if (b + 2u > size) return 0;
    const uint16_t bop = (uint16_t)(rom[b] << 8 | rom[b + 1u]);
    if ((bop & 0xf000u) != 0x6000u || ((bop >> 8) & 15u) < 2u || !(bop & 255u)) return 0;
    if (b + 2u + (uint32_t)(int32_t)(int8_t)bop != pc) return 0;
    return len;
}

#endif
