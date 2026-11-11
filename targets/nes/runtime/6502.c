/* 6502 interpreter, after the repo's Z80 core's model: cycle counts per
 * the documented table (page-cross penalties where they apply), no decimal
 * mode (the 2A03 has none), NMI on the host's edge and IRQ level-sampled.
 * Illegal opcodes run as NOPs of their true length — no game in the
 * pipeline's scope relies on them. */
#if __has_include("nes_config.h") /* a generated project's: NES_NATIVE */
#ifndef M0_TL
#define M0_TL   /* deepeel builds with -DM0_TL=__thread: one machine per worker thread */
#endif
#include "nes_config.h"
#endif
#include "6502.h"
#include "nes_native.h"
#ifdef NES_TRACE
#include "nes_trace.h"
M0_TL NesTrace nes_trace;
#endif
#ifdef NES_COVERAGE
#include <stdio.h>
#include <stdlib.h>
#endif

/* The interpreter's hot state — PC, the stack pointer and the bus's two
 * hot windows — lives in nes6502_run's locals, in registers: a read is a
 * compare, a mask and a load (nes_rd's pointer chase through the machine
 * cost a third of the interpreter's cycles).  The opcode table and the
 * helpers below are macros over those locals; they exist only inside the
 * run loop's reach, which nes6502_reset steps around with nes_rd. */
#ifndef NES_TRACE
#define rd(cpu, at) (__extension__({ (void)(cpu); uint16_t a_ = (at); \
    a_ < 0x2000u ? ram_[a_ & 0x7FFu] \
    : a_ >= 0x8000u ? prg_[(a_ & 0x3FFFu) + (a_ >= 0xC000u ? fixed_base_ : cpu->bank_base)] \
    : nes_bus_read(a_); }))
#define wr(cpu, at, v) do { (void)(cpu); uint16_t a_ = (at); \
    if (a_ < 0x2000u) ram_[a_ & 0x7FFu] = (v); else nes_bus_write(a_, (v)); } while (0)
#else
/* the traced variants: every data read and write goes through the hook
 * (the fetch and the stack stay out), and a stop ends the slice at the
 * next instruction boundary with only the cycles actually consumed */
#define NES_T_RD(cpu, a_, v_) do { if (nes_trace.rd) nes_trace.rd(nes_trace.user, (a_), (v_)); } while (0)
#define NES_T_WR(cpu, a_, v) do { if (nes_trace.wr) nes_trace.wr(nes_trace.user, (a_), (v)); } while (0)
#define rd(cpu, at) (__extension__({ (void)(cpu); uint16_t a_ = (at); \
    uint8_t v_ = a_ < 0x2000u ? ram_[a_ & 0x7FFu] \
    : a_ >= 0x8000u ? prg_[(a_ & 0x3FFFu) + (a_ >= 0xC000u ? fixed_base_ : cpu->bank_base)] \
    : nes_bus_read(a_); \
    NES_T_RD(cpu, a_, v_); v_; }))
#define wr(cpu, at, v) do { (void)(cpu); uint16_t a_ = (at); NES_T_WR(cpu, a_, (v)); \
    if (a_ < 0x2000u) ram_[a_ & 0x7FFu] = (v); else nes_bus_write(a_, (v)); } while (0)
#endif
#define push(cpu, v) do { (void)(cpu); ram_[0x100u | s_] = (v); s_--; } while (0)
#define pop(cpu) ram_[0x100u | ++s_]
#define set_zn(cpu, v) nes_zn(cpu, v)
#define adc(cpu, m) nes_adc(cpu, m)
#define sbc(cpu, m) nes_sbc(cpu, m)
#define cmp(cpu, r, m) nes_cmp(cpu, r, m)

#define A cpu->a
#define X cpu->x
#define Y cpu->y
#define P cpu->p

/* addressing modes, computed only when the opcode needs them (a 16-bit
 * pointer read inside the zero page wraps at $FF -> $00) */
#define A_ZP() rd(cpu, (uint16_t)(pc_ + 1))
#define A_ZPX() (uint16_t)(uint8_t)(A_ZP() + X)
#define A_ZPY() (uint16_t)(uint8_t)(A_ZP() + Y)
#define A_ABS() (uint16_t)(rd(cpu, (uint16_t)(pc_ + 1)) | (rd(cpu, (uint16_t)(pc_ + 2)) << 8))
#define A_IZX() (__extension__({ unsigned b_ = (unsigned)(uint8_t)(A_ZP() + X); \
    (uint16_t)(rd(cpu, (uint16_t)b_) | (rd(cpu, (uint16_t)((b_ + 1) & 0xFFu)) << 8)); }))
#define A_IZY() (__extension__({ unsigned b_ = (unsigned)A_ZP() & 0xFFu; \
    (uint16_t)(rd(cpu, (uint16_t)b_) | (rd(cpu, (uint16_t)((b_ + 1) & 0xFFu)) << 8)); }))
/* the page-cross penalty: the high byte of the effective address changed */
#define CROSS(a, b) ((((uint16_t)(a) ^ (uint16_t)(b)) & 0xFF00u) ? 1u : 0u)
#define ASL(m) ((P = (P & ~NES_P_C) | ((m) >> 7), (uint8_t)((m) << 1)))
#define LSR(m) ((P = (P & ~NES_P_C) | ((m) & 1), (uint8_t)((m) >> 1)))
#define ROL(m) nes_rol(cpu, m)
#define ROR(m) nes_ror(cpu, m)
/* branches: operand first, then the page-crossing cost */
#define BRANCH(cpu, take) do { \
    const int8_t off = (int8_t)rd(cpu, (uint16_t)(pc_ + 1)); \
    if (take) { \
        const uint16_t from = (uint16_t)(pc_ + 2); \
        const uint16_t to = (uint16_t)(from + off); \
        left = 3u + (((from ^ to) & 0xFF00u) ? 1u : 0u); \
        pc_ = to; \
    } else { \
        pc_ = (uint16_t)(pc_ + 2); \
        left = 2u; \
    } \
} while (0)
/* NMI, IRQ and BRK share the sequence */
#ifdef NES_TRACE
#define INTERRUPT(cpu, vector, brk) do { \
    if (nes_trace.interrupt) nes_trace.interrupt(nes_trace.user, (vector)); \
    push(cpu, (uint8_t)(pc_ >> 8)); \
    push(cpu, (uint8_t)pc_); \
    push(cpu, (uint8_t)(P | NES_P_U | ((brk) ? NES_P_B : 0))); \
    P |= NES_P_I; \
    pc_ = (uint16_t)(rd(cpu, (vector)) | (rd(cpu, (uint16_t)((vector) + 1)) << 8)); \
} while (0)
#else
#define INTERRUPT(cpu, vector, brk) do { \
    push(cpu, (uint8_t)(pc_ >> 8)); \
    push(cpu, (uint8_t)pc_); \
    push(cpu, (uint8_t)(P | NES_P_U | ((brk) ? NES_P_B : 0))); \
    P |= NES_P_I; \
    pc_ = (uint16_t)(rd(cpu, (vector)) | (rd(cpu, (uint16_t)((vector) + 1)) << 8)); \
} while (0)
#endif

void nes6502_reset(nes6502_t *cpu) {
    cpu->stall_pending = 0;
    cpu->a = cpu->x = cpu->y = 0;
    cpu->s = 0xFD;
    cpu->p = NES_P_I | NES_P_U;
    cpu->pc = (uint16_t)(nes_rd(cpu, 0xFFFC) | (nes_rd(cpu, 0xFFFD) << 8));
    cpu->nmi_edge = 0;
    cpu->irq_line = 0;
    cpu->cycles = 7;
}

void nes6502_stall(nes6502_t *cpu, uint32_t cycles) { cpu->stall_pending += cycles; }

/* the page-cross penalty: the high byte of the effective address changed */
#define CROSS(a, b) ((((uint16_t)(a) ^ (uint16_t)(b)) & 0xFF00u) ? 1u : 0u)
#define ASL(m) ((P = (P & ~NES_P_C) | ((m) >> 7), (uint8_t)((m) << 1)))
#define LSR(m) ((P = (P & ~NES_P_C) | ((m) & 1), (uint8_t)((m) >> 1)))
#define ROL(m) nes_rol(cpu, m)
#define ROR(m) nes_ror(cpu, m)

#ifdef NES_NATIVE
#define NES_INTERP_FN static void nes6502_interp
#else
#define NES_INTERP_FN void nes6502_run
#endif
#ifdef NES_COVERAGE
/* the harness's --coverage: executions per PRG address in its bank
 * context (the switchable bank mapped at the fetch, the fixed window at
 * $C000-$FFFF, slot bank_count), the addresses a dynamic transfer (RTS,
 * RTI, JMP (ind)) landed on — the recompiler's entries the static walk
 * cannot see — and the distinct addresses played, the explorer's novelty
 * (m0_explore.h).  The tables are allocated to the cartridge's banks
 * (nes_coverage_init, the host calls it before nes_init). */
M0_TL uint32_t *nes_coverage_hits;
M0_TL uint8_t *nes_coverage_dynamic;
M0_TL uint32_t nes_coverage_locations;
void nes_coverage_init(unsigned banks) {
    nes_coverage_hits = calloc((size_t)(banks + 1) * 0x8000u, sizeof *nes_coverage_hits);
    nes_coverage_dynamic = calloc((size_t)(banks + 1) * 0x8000u, 1);
    if (!nes_coverage_hits || !nes_coverage_dynamic) { fprintf(stderr, "no memory for coverage\n"); exit(2); }
}
#define COVER(pc) do { if ((pc) >= 0x8000u) { \
    const unsigned slot_ = !cpu->banked ? 0u : (pc) >= 0xC000u ? cpu->bank_count : cpu->bank; \
    uint32_t *hit_ = &nes_coverage_hits[slot_ * 0x8000u + ((pc) - 0x8000u)]; \
    if (!(*hit_)++) nes_coverage_locations++; } } while (0)
#define COVER_DYNAMIC(pc) do { if ((pc) >= 0x8000u) { \
    const unsigned slot_ = !cpu->banked ? 0u : (pc) >= 0xC000u ? cpu->bank_count : cpu->bank; \
    nes_coverage_dynamic[slot_ * 0x8000u + ((pc) - 0x8000u)] = 1; } } while (0)
#else
#define COVER(pc) do { } while (0)
#define COVER_DYNAMIC(pc) do { } while (0)
#endif

NES_INTERP_FN(nes6502_t *cpu, uint32_t until) {
    /* signed: an instruction runs past `until`, the budget goes negative
     * and the overshoot stays in cpu->cycles, as `cycles += left` did */
    int32_t budget = (int32_t)(until - cpu->cycles);
    uint16_t pc_ = cpu->pc;
    uint8_t s_ = cpu->s;
    uint8_t *ram_ = cpu->ram;
    const uint8_t *prg_ = cpu->prg;
    /* the fixed window never moves; the bank at $8000 does, mid-slice
     * (a banked game's write to $8000-$FFFF): read cpu->bank_base live */
    const uint32_t fixed_base_ = cpu->fixed_base;
    uint32_t left;
    while (budget > 0) {
        if (cpu->stall_pending) { /* the slice ends where the DMA bit */
            budget -= (int32_t)cpu->stall_pending;
            cpu->stall_pending = 0;
            if (budget <= 0) break;
        }
#ifdef NES_TRACE
        if (nes_trace.stop) { cpu->cycles = until - budget; goto done; }
#endif
        if (cpu->nmi_edge) {
            cpu->nmi_edge = 0;
            INTERRUPT(cpu, 0xFFFA, 0);
            COVER_DYNAMIC(pc_);
            budget -= 7;
            continue;
        }
        if (cpu->irq_line && !(P & NES_P_I)) {
            INTERRUPT(cpu, 0xFFFE, 0);
            COVER_DYNAMIC(pc_);
            budget -= 7;
            continue;
        }
        {
            COVER(pc_);
#ifdef NES_TRACE
            const uint8_t op = (__extension__({ uint16_t a_ = (uint16_t)pc_; \
                a_ < 0x2000u ? ram_[a_ & 0x7FFu] \
                : prg_[(a_ & 0x3FFFu) + (a_ >= 0xC000u ? fixed_base_ : cpu->bank_base)]; }));
            if (nes_trace.step) {
                /* the loop keeps pc, s and cycles in locals: sync them so a
                 * hook (and any snapshot it takes) sees the exact state */
                cpu->pc = pc_;
                cpu->s = s_;
                cpu->cycles = until - budget;
                nes_trace.step(nes_trace.user, pc_, op);
            }
            if (nes_trace.stop) { cpu->cycles = until - budget; goto done; }
#else
            const uint8_t op = rd(cpu, pc_);
#endif
            left = 2;
            switch (op) {
        /* loads */
        case 0xA9: A = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, A); pc_ += 2; left = 2; break;
        case 0xA5: A = rd(cpu, A_ZP()); set_zn(cpu, A); pc_ += 2; left = 3; break;
        case 0xB5: A = rd(cpu, A_ZPX()); set_zn(cpu, A); pc_ += 2; left = 4; break;
        case 0xAD: A = rd(cpu, A_ABS()); set_zn(cpu, A); pc_ += 3; left = 4; break;
        case 0xBD: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A = rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xB9: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A = rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xA1: A = rd(cpu, A_IZX()); set_zn(cpu, A); pc_ += 2; left = 6; break;
        case 0xB1: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A = rd(cpu, t); set_zn(cpu, A); pc_ += 2; left = 5 + CROSS(b, t); break; }
        case 0xA2: X = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, X); pc_ += 2; left = 2; break;
        case 0xA6: X = rd(cpu, A_ZP()); set_zn(cpu, X); pc_ += 2; left = 3; break;
        case 0xB6: X = rd(cpu, A_ZPY()); set_zn(cpu, X); pc_ += 2; left = 4; break;
        case 0xAE: X = rd(cpu, A_ABS()); set_zn(cpu, X); pc_ += 3; left = 4; break;
        case 0xBE: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            X = rd(cpu, t); set_zn(cpu, X); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xA0: Y = rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, Y); pc_ += 2; left = 2; break;
        case 0xA4: Y = rd(cpu, A_ZP()); set_zn(cpu, Y); pc_ += 2; left = 3; break;
        case 0xB4: Y = rd(cpu, A_ZPX()); set_zn(cpu, Y); pc_ += 2; left = 4; break;
        case 0xAC: Y = rd(cpu, A_ABS()); set_zn(cpu, Y); pc_ += 3; left = 4; break;
        case 0xBC: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            Y = rd(cpu, t); set_zn(cpu, Y); pc_ += 3; left = 4 + CROSS(b, t); break; }
        /* stores */
        case 0x85: wr(cpu, A_ZP(), A); pc_ += 2; left = 3; break;
        case 0x95: wr(cpu, A_ZPX(), A); pc_ += 2; left = 4; break;
        case 0x8D: wr(cpu, A_ABS(), A); pc_ += 3; left = 4; break;
        case 0x9D: { const uint16_t b = A_ABS(); wr(cpu, (uint16_t)(b + X), A); pc_ += 3; left = 5; break; }
        case 0x99: { const uint16_t b = A_ABS(); wr(cpu, (uint16_t)(b + Y), A); pc_ += 3; left = 5; break; }
        case 0x81: wr(cpu, A_IZX(), A); pc_ += 2; left = 6; break;
        case 0x91: { const uint16_t b = A_IZY(); wr(cpu, (uint16_t)(b + Y), A); pc_ += 2; left = 6; break; }
        case 0x86: wr(cpu, A_ZP(), X); pc_ += 2; left = 3; break;
        case 0x96: wr(cpu, A_ZPY(), X); pc_ += 2; left = 4; break;
        case 0x8E: wr(cpu, A_ABS(), X); pc_ += 3; left = 4; break;
        case 0x84: wr(cpu, A_ZP(), Y); pc_ += 2; left = 3; break;
        case 0x94: wr(cpu, A_ZPX(), Y); pc_ += 2; left = 4; break;
        case 0x8C: wr(cpu, A_ABS(), Y); pc_ += 3; left = 4; break;
        /* arithmetic */
        case 0x69: A = adc(cpu, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; left = 2; break;
        case 0x65: A = adc(cpu, rd(cpu, A_ZP())); pc_ += 2; left = 3; break;
        case 0x75: A = adc(cpu, rd(cpu, A_ZPX())); pc_ += 2; left = 4; break;
        case 0x6D: A = adc(cpu, rd(cpu, A_ABS())); pc_ += 3; left = 4; break;
        case 0x7D: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A = adc(cpu, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x79: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A = adc(cpu, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x61: A = adc(cpu, rd(cpu, A_IZX())); pc_ += 2; left = 6; break;
        case 0x71: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A = adc(cpu, rd(cpu, t)); pc_ += 2; left = 5 + CROSS(b, t); break; }
        case 0xE9: A = sbc(cpu, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; left = 2; break;
        case 0xE5: A = sbc(cpu, rd(cpu, A_ZP())); pc_ += 2; left = 3; break;
        case 0xF5: A = sbc(cpu, rd(cpu, A_ZPX())); pc_ += 2; left = 4; break;
        case 0xED: A = sbc(cpu, rd(cpu, A_ABS())); pc_ += 3; left = 4; break;
        case 0xFD: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A = sbc(cpu, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xF9: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A = sbc(cpu, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xE1: A = sbc(cpu, rd(cpu, A_IZX())); pc_ += 2; left = 6; break;
        case 0xF1: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A = sbc(cpu, rd(cpu, t)); pc_ += 2; left = 5 + CROSS(b, t); break; }
        /* logic */
        case 0x09: A |= rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, A); pc_ += 2; left = 2; break;
        case 0x05: A |= rd(cpu, A_ZP()); set_zn(cpu, A); pc_ += 2; left = 3; break;
        case 0x15: A |= rd(cpu, A_ZPX()); set_zn(cpu, A); pc_ += 2; left = 4; break;
        case 0x0D: A |= rd(cpu, A_ABS()); set_zn(cpu, A); pc_ += 3; left = 4; break;
        case 0x1D: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A |= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x19: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A |= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x01: A |= rd(cpu, A_IZX()); set_zn(cpu, A); pc_ += 2; left = 6; break;
        case 0x11: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A |= rd(cpu, t); set_zn(cpu, A); pc_ += 2; left = 5 + CROSS(b, t); break; }
        case 0x29: A &= rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, A); pc_ += 2; left = 2; break;
        case 0x25: A &= rd(cpu, A_ZP()); set_zn(cpu, A); pc_ += 2; left = 3; break;
        case 0x35: A &= rd(cpu, A_ZPX()); set_zn(cpu, A); pc_ += 2; left = 4; break;
        case 0x2D: A &= rd(cpu, A_ABS()); set_zn(cpu, A); pc_ += 3; left = 4; break;
        case 0x3D: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A &= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x39: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A &= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x21: A &= rd(cpu, A_IZX()); set_zn(cpu, A); pc_ += 2; left = 6; break;
        case 0x31: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A &= rd(cpu, t); set_zn(cpu, A); pc_ += 2; left = 5 + CROSS(b, t); break; }
        case 0x49: A ^= rd(cpu, (uint16_t)(pc_ + 1)); set_zn(cpu, A); pc_ += 2; left = 2; break;
        case 0x45: A ^= rd(cpu, A_ZP()); set_zn(cpu, A); pc_ += 2; left = 3; break;
        case 0x55: A ^= rd(cpu, A_ZPX()); set_zn(cpu, A); pc_ += 2; left = 4; break;
        case 0x4D: A ^= rd(cpu, A_ABS()); set_zn(cpu, A); pc_ += 3; left = 4; break;
        case 0x5D: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            A ^= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x59: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            A ^= rd(cpu, t); set_zn(cpu, A); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0x41: A ^= rd(cpu, A_IZX()); set_zn(cpu, A); pc_ += 2; left = 6; break;
        case 0x51: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            A ^= rd(cpu, t); set_zn(cpu, A); pc_ += 2; left = 5 + CROSS(b, t); break; }
        /* compares */
        case 0xC9: cmp(cpu, A, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; left = 2; break;
        case 0xC5: cmp(cpu, A, rd(cpu, A_ZP())); pc_ += 2; left = 3; break;
        case 0xD5: cmp(cpu, A, rd(cpu, A_ZPX())); pc_ += 2; left = 4; break;
        case 0xCD: cmp(cpu, A, rd(cpu, A_ABS())); pc_ += 3; left = 4; break;
        case 0xDD: { const uint16_t b = A_ABS(), t = (uint16_t)(b + X);
            cmp(cpu, A, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xD9: { const uint16_t b = A_ABS(), t = (uint16_t)(b + Y);
            cmp(cpu, A, rd(cpu, t)); pc_ += 3; left = 4 + CROSS(b, t); break; }
        case 0xC1: cmp(cpu, A, rd(cpu, A_IZX())); pc_ += 2; left = 6; break;
        case 0xD1: { const uint16_t b = A_IZY(), t = (uint16_t)(b + Y);
            cmp(cpu, A, rd(cpu, t)); pc_ += 2; left = 5 + CROSS(b, t); break; }
        case 0xE0: cmp(cpu, X, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; left = 2; break;
        case 0xE4: cmp(cpu, X, rd(cpu, A_ZP())); pc_ += 2; left = 3; break;
        case 0xEC: cmp(cpu, X, rd(cpu, A_ABS())); pc_ += 3; left = 4; break;
        case 0xC0: cmp(cpu, Y, rd(cpu, (uint16_t)(pc_ + 1))); pc_ += 2; left = 2; break;
        case 0xC4: cmp(cpu, Y, rd(cpu, A_ZP())); pc_ += 2; left = 3; break;
        case 0xCC: cmp(cpu, Y, rd(cpu, A_ABS())); pc_ += 3; left = 4; break;
        case 0x24: { const uint8_t m = rd(cpu, A_ZP());
            P = (P & ~(NES_P_Z | NES_P_N | NES_P_V)) | (A & m ? 0 : NES_P_Z) | (m & 0xC0);
            pc_ += 2; left = 3; break; }
        case 0x2C: { const uint8_t m = rd(cpu, A_ABS());
            P = (P & ~(NES_P_Z | NES_P_N | NES_P_V)) | (A & m ? 0 : NES_P_Z) | (m & 0xC0);
            pc_ += 3; left = 4; break; }
        /* shifts */
        case 0x0A: A = ASL(A); set_zn(cpu, A); pc_ += 1; left = 2; break;
        case 0x06: { const uint16_t at = A_ZP(); const uint8_t r = ASL(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 5; break; }
        case 0x16: { const uint16_t at = A_ZPX(); const uint8_t r = ASL(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 6; break; }
        case 0x0E: { const uint16_t at = A_ABS(); const uint8_t r = ASL(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 6; break; }
        case 0x1E: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); const uint8_t r = ASL(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 7; break; }
        case 0x4A: A = LSR(A); set_zn(cpu, A); pc_ += 1; left = 2; break;
        case 0x46: { const uint16_t at = A_ZP(); const uint8_t r = LSR(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 5; break; }
        case 0x56: { const uint16_t at = A_ZPX(); const uint8_t r = LSR(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 6; break; }
        case 0x4E: { const uint16_t at = A_ABS(); const uint8_t r = LSR(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 6; break; }
        case 0x5E: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); const uint8_t r = LSR(rd(cpu, at));
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 7; break; }
        case 0x2A: A = ROL(A); pc_ += 1; left = 2; break;
        case 0x26: { const uint16_t at = A_ZP(); wr(cpu, at, ROL(rd(cpu, at))); pc_ += 2; left = 5; break; }
        case 0x36: { const uint16_t at = A_ZPX(); wr(cpu, at, ROL(rd(cpu, at))); pc_ += 2; left = 6; break; }
        case 0x2E: { const uint16_t at = A_ABS(); wr(cpu, at, ROL(rd(cpu, at))); pc_ += 3; left = 6; break; }
        case 0x3E: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); wr(cpu, at, ROL(rd(cpu, at))); pc_ += 3; left = 7; break; }
        case 0x6A: A = ROR(A); pc_ += 1; left = 2; break;
        case 0x66: { const uint16_t at = A_ZP(); wr(cpu, at, ROR(rd(cpu, at))); pc_ += 2; left = 5; break; }
        case 0x76: { const uint16_t at = A_ZPX(); wr(cpu, at, ROR(rd(cpu, at))); pc_ += 2; left = 6; break; }
        case 0x6E: { const uint16_t at = A_ABS(); wr(cpu, at, ROR(rd(cpu, at))); pc_ += 3; left = 6; break; }
        case 0x7E: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); wr(cpu, at, ROR(rd(cpu, at))); pc_ += 3; left = 7; break; }
        /* inc/dec */
        case 0xE6: { const uint16_t at = A_ZP(); const uint8_t r = (uint8_t)(rd(cpu, at) + 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 5; break; }
        case 0xF6: { const uint16_t at = A_ZPX(); const uint8_t r = (uint8_t)(rd(cpu, at) + 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 6; break; }
        case 0xEE: { const uint16_t at = A_ABS(); const uint8_t r = (uint8_t)(rd(cpu, at) + 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 6; break; }
        case 0xFE: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); const uint8_t r = (uint8_t)(rd(cpu, at) + 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 7; break; }
        case 0xC6: { const uint16_t at = A_ZP(); const uint8_t r = (uint8_t)(rd(cpu, at) - 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 5; break; }
        case 0xD6: { const uint16_t at = A_ZPX(); const uint8_t r = (uint8_t)(rd(cpu, at) - 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 2; left = 6; break; }
        case 0xCE: { const uint16_t at = A_ABS(); const uint8_t r = (uint8_t)(rd(cpu, at) - 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 6; break; }
        case 0xDE: { const uint16_t b = A_ABS(), at = (uint16_t)(b + X); const uint8_t r = (uint8_t)(rd(cpu, at) - 1);
            wr(cpu, at, r); set_zn(cpu, r); pc_ += 3; left = 7; break; }
        case 0xE8: X++; set_zn(cpu, X); pc_ += 1; left = 2; break;
        case 0xCA: X--; set_zn(cpu, X); pc_ += 1; left = 2; break;
        case 0xC8: Y++; set_zn(cpu, Y); pc_ += 1; left = 2; break;
        case 0x88: Y--; set_zn(cpu, Y); pc_ += 1; left = 2; break;
        /* branches */
        case 0x10: BRANCH(cpu, !(P & NES_P_N)); break;
        case 0x30: BRANCH(cpu, P & NES_P_N); break;
        case 0x50: BRANCH(cpu, !(P & NES_P_V)); break;
        case 0x70: BRANCH(cpu, P & NES_P_V); break;
        case 0x90: BRANCH(cpu, !(P & NES_P_C)); break;
        case 0xB0: BRANCH(cpu, P & NES_P_C); break;
        case 0xD0: BRANCH(cpu, !(P & NES_P_Z)); break;
        case 0xF0: BRANCH(cpu, P & NES_P_Z); break;
        /* jumps, calls, returns */
        case 0x4C: pc_ = A_ABS(); left = 3; break;
        case 0x6C: { /* the 6502's page-wrap bug, faithfully */
            const uint16_t ptr = A_ABS();
            const uint16_t tgt = (uint16_t)(rd(cpu, ptr) |
                (rd(cpu, (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF))) << 8));
            pc_ = tgt; COVER_DYNAMIC(pc_); left = 5; break; }
        case 0x20: { const uint16_t ret = (uint16_t)(pc_ + 2);
            push(cpu, (uint8_t)(ret >> 8));
            push(cpu, (uint8_t)ret);
            pc_ = A_ABS(); left = 6; break; }
        case 0x60: { const uint8_t lo8 = pop(cpu), hi8 = pop(cpu);
            pc_ = (uint16_t)((lo8 | (hi8 << 8)) + 1); COVER_DYNAMIC(pc_); left = 6; break; }
        case 0x40: { P = (uint8_t)((pop(cpu) & ~NES_P_B) | NES_P_U);
            { const uint8_t lo8 = pop(cpu), hi8 = pop(cpu);
              pc_ = (uint16_t)(lo8 | (hi8 << 8)); }
            COVER_DYNAMIC(pc_); left = 6; break; }
        case 0x00: pc_++; INTERRUPT(cpu, 0xFFFE, 1); left = 7; break;
        /* flags */
        case 0x18: P &= ~NES_P_C; pc_ += 1; left = 2; break;
        case 0x38: P |= NES_P_C; pc_ += 1; left = 2; break;
        case 0x58: P &= ~NES_P_I; pc_ += 1; left = 2; break;
        case 0x78: P |= NES_P_I; pc_ += 1; left = 2; break;
        case 0xB8: P &= ~NES_P_V; pc_ += 1; left = 2; break;
        case 0xD8: P &= ~NES_P_D; pc_ += 1; left = 2; break;
        case 0xF8: P |= NES_P_D; pc_ += 1; left = 2; break;
        /* transfers, stack */
        case 0xAA: X = A; set_zn(cpu, X); pc_ += 1; left = 2; break;
        case 0xA8: Y = A; set_zn(cpu, Y); pc_ += 1; left = 2; break;
        case 0x8A: A = X; set_zn(cpu, A); pc_ += 1; left = 2; break;
        case 0x98: A = Y; set_zn(cpu, A); pc_ += 1; left = 2; break;
        case 0xBA: X = s_; set_zn(cpu, X); pc_ += 1; left = 2; break;
        case 0x9A: s_ = X; pc_ += 1; left = 2; break;
        case 0x48: push(cpu, A); pc_ += 1; left = 3; break;
        case 0x08: push(cpu, (uint8_t)(P | NES_P_B | NES_P_U)); pc_ += 1; left = 3; break;
        case 0x68: A = pop(cpu); set_zn(cpu, A); pc_ += 1; left = 4; break;
        case 0x28: P = (uint8_t)((pop(cpu) & ~NES_P_B) | NES_P_U); pc_ += 1; left = 4; break;
        default:
            /* illegal or NOP: honest length and cycles keep the sweep aligned */
            if (op == 0x1A || op == 0x3A || op == 0x5A || op == 0x7A ||
                op == 0xDA || op == 0xFA) { pc_ += 1; left = 2; }
            else if (op == 0x80 || op == 0x82 || op == 0x89 || op == 0xC2 ||
                     op == 0xE2) { pc_ += 2; left = 2; }
            else if (op == 0x04 || op == 0x44 || op == 0x64) { pc_ += 2; left = 3; }
            else if (op == 0x14 || op == 0x34 || op == 0x54 || op == 0x74 ||
                     op == 0xD4 || op == 0xF4) { pc_ += 2; left = 4; }
            else if (op == 0x0C) { pc_ += 3; left = 4; }
            else if (op == 0x1C || op == 0x3C || op == 0x5C || op == 0x7C ||
                     op == 0xDC || op == 0xFC) { pc_ += 3; left = 4; }
            else { pc_ += 1; left = 2; }
            break;
        }
        }
        budget -= left;
    }
#ifdef NES_TRACE
done:
#endif
    cpu->pc = pc_;
    cpu->s = s_;
    cpu->cycles = until - budget;
}

#ifdef NES_NATIVE
/* Recompiled code (nes_gen.c) in front of the interpreter.  The loop is the
 * interpreter's own — DMA stall, NMI, IRQ, then one step — except that a
 * step is a call into the recompiled entry at pc when there is one: it runs
 * until the slice is spent (precisely: after the instruction that spends
 * it, the same boundary as the interpreter; otherwise at its next check),
 * or until a transfer it cannot follow.  Anything else — code in RAM, code
 * left out of the flash budget, undocumented opcodes — is interpreted one
 * instruction at a time.  nes_native = 0 is the reference (--interp). */
#include "nes_gen.h"

M0_TL int nes_native = 1;
#ifdef NES_GEN_STATS
uint32_t nes_gen_calls, nes_gen_steps; /* the harness prints them */
#define GEN_STAT(n) ((n)++)
#else
#define GEN_STAT(n) ((void)0)
#endif

void nes6502_run(nes6502_t *cpu, uint32_t until) {
    if (!nes_native) { nes6502_interp(cpu, until); return; }
    int32_t budget = (int32_t)(until - cpu->cycles);
    while (budget > 0) {
        if (cpu->stall_pending) {
            budget -= (int32_t)cpu->stall_pending;
            cpu->stall_pending = 0;
            if (budget <= 0) break;
        }
        if (cpu->nmi_edge || (cpu->irq_line && !(cpu->p & NES_P_I))) {
            const uint16_t vector = cpu->nmi_edge ? 0xFFFAu : 0xFFFEu;
            cpu->nmi_edge = 0;
            nes_push(cpu, (uint8_t)(cpu->pc >> 8));
            nes_push(cpu, (uint8_t)cpu->pc);
            nes_push(cpu, (uint8_t)(cpu->p | NES_P_U));
            cpu->p |= NES_P_I;
            cpu->pc = (uint16_t)(nes_rd(cpu, vector) | (nes_rd(cpu, (uint16_t)(vector + 1)) << 8));
            budget -= 7;
            continue;
        }
        /* JMP to itself in PRG, a game waiting for its NMI: the rest of
         * the slice in jumps of 3 cycles, as the recompiled loop spends it
         * (emitter.cpp), without entering it each line */
        if (cpu->pc >= 0x8000u && nes_rd(cpu, cpu->pc) == 0x4C &&
            (nes_rd(cpu, (uint16_t)(cpu->pc + 1u)) | nes_rd(cpu, (uint16_t)(cpu->pc + 2u)) << 8) == cpu->pc) {
            budget = NES_SPIN3(budget);
            break;
        }
        const int entry = nes_gen_lookup(cpu->pc);
        if (entry >= 0) {
            GEN_STAT(nes_gen_calls);
            budget = nes_gen_enter(entry, budget);
            continue;
        }
        /* one instruction: a budget of one cycle is spent by any of them */
        GEN_STAT(nes_gen_steps);
        cpu->cycles = until - (uint32_t)budget;
        nes6502_interp(cpu, cpu->cycles + 1u);
        budget = (int32_t)(until - cpu->cycles);
    }
    cpu->cycles = until - (uint32_t)budget;
}
#endif
