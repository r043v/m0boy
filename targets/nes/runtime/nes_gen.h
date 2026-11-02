/* What recompiled 6502 code (nes_gen.c, written by the recompiler's
 * src/system/nes/emitter.cpp) needs from the runtime, and what the
 * dispatcher (6502.c under NES_NATIVE) needs from it.
 *
 * A region is one C function over neighbouring instructions.  It loads the
 * CPU's registers into locals, runs with the flags split (n and z the last
 * result, c 0/1, v 0/0x40, the I and D bits apart), and stores them back
 * when it leaves: on the budget's end, on a transfer it cannot follow, or
 * by a tail call into the region that holds a known target.  The budget is
 * the interpreter's: master cycles left in the slice, signed. */
#ifndef NES_GEN_H
#define NES_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "nes.h"
#include "nes_native.h"
#include "m0_idle.h"

typedef int32_t (*nes_region_fn)(int32_t budget, unsigned entry);

/* the entry at pc, or -1 (generated) */
int nes_gen_lookup(uint16_t pc);
/* runs from entry `entry`, returns the budget left (generated) */
int32_t nes_gen_enter(int entry, int32_t budget);
/* 0: everything through the interpreter (the reference) */
extern int nes_native;

extern const uint8_t nes_prg[];

#define NES_C (machine.cpu)
#define NES_RAM (machine.ram)
/* inside a region, RAM through a pointer of its own: machine.ram sits past
 * Thumb-1's immediate offsets, and a base register saves an add per access */
#define NES_RAM_BASE() uint8_t *const ram_ = machine.ram

/* the bus outside the folded constants, as the interpreter sees it: RAM
 * direct, the PRG through its two windows (the bank at $8000-$BFFF is the
 * machine's — banked cartridges switch it at run time), the rest the bus */
static inline __attribute__((always_inline)) uint8_t nes_gen_rd(uint16_t at) {
    if (at < 0x2000u) return NES_RAM[at & 0x07FFu];
    if (at >= 0x8000u)
        return nes_prg[(at & 0x3FFFu) + (at >= 0xC000u ? NES_C.fixed_base : NES_C.bank_base)];
    return nes_bus_read(at);
}
static inline __attribute__((always_inline)) void nes_gen_wr(uint16_t at, uint8_t v) {
    if (at < 0x2000u) { NES_RAM[at & 0x07FFu] = v; return; }
    nes_bus_write(at, v);
}

/* the region's prologue and epilogue: P split and joined back (U always
 * set, B never kept, as the interpreter keeps it) */
#define NES_LOAD() \
    uint8_t a = NES_C.a, x = NES_C.x, y = NES_C.y, s = NES_C.s; \
    uint8_t n = NES_C.p, z = (uint8_t)(~NES_C.p & NES_P_Z), c = (uint8_t)(NES_C.p & NES_P_C), \
            v = (uint8_t)(NES_C.p & NES_P_V), id = (uint8_t)(NES_C.p & (NES_P_I | NES_P_D))
#define NES_P() ((uint8_t)((n & NES_P_N) | v | NES_P_U | id | (z ? 0 : NES_P_Z) | c))
#define NES_SAVE() do { \
    NES_C.a = a; NES_C.x = x; NES_C.y = y; NES_C.s = s; NES_C.p = NES_P(); \
} while (0)
#define NES_SETP(p_) do { const uint8_t p__ = (p_); \
    n = p__; z = (uint8_t)(~p__ & NES_P_Z); c = (uint8_t)(p__ & NES_P_C); \
    v = (uint8_t)(p__ & NES_P_V); id = (uint8_t)(p__ & (NES_P_I | NES_P_D)); } while (0)

#define NES_PUSH(v_) do { ram_[0x100u | s] = (v_); s--; } while (0)
#define NES_POP() (s++, ram_[0x100u | s])

#define NES_ZN(r_) (n = z = (r_))
/* JMP to itself (3 cycles) until the slice is spent, as the interpreter
 * jumps (m0_idle.h): nothing that could end a slice early (an NMI, an
 * IRQ, a DMA stall) changes inside one.  b > 0. */
#define NES_SPIN3(b_) ((b_) - 3 * (int32_t)m0_idle_loops((uint32_t)(b_), 3u))
#define NES_ADC(m_) do { const uint8_t m__ = (m_); const unsigned sum__ = (unsigned)a + m__ + c; \
    v = (uint8_t)((~(a ^ m__) & (a ^ sum__) & 0x80u) >> 1); c = (uint8_t)(sum__ >> 8); \
    a = (uint8_t)sum__; NES_ZN(a); } while (0)
#define NES_SBC(m_) NES_ADC((uint8_t)~(m_))
/* the same arithmetic with only V and C kept: no flag read wants N or Z */
#define NES_ADC_NC(m_) do { const uint8_t m__ = (m_); const unsigned sum__ = (unsigned)a + m__ + c; \
    v = (uint8_t)((~(a ^ m__) & (a ^ sum__) & 0x80u) >> 1); c = (uint8_t)(sum__ >> 8); \
    a = (uint8_t)sum__; } while (0)
#define NES_SBC_NC(m_) NES_ADC_NC((uint8_t)~(m_))
#define NES_CMP(r_, m_) do { const uint8_t r__ = (r_), m__ = (m_); \
    c = (uint8_t)(r__ >= m__); NES_ZN((uint8_t)(r__ - m__)); } while (0)
#define NES_BIT(m_) do { const uint8_t m__ = (m_); \
    z = (uint8_t)(a & m__); n = m__; v = (uint8_t)(m__ & 0x40u); } while (0)
#define NES_ASL(m_) (__extension__({ const uint8_t m__ = (m_); c = (uint8_t)(m__ >> 7); \
    const uint8_t r__ = (uint8_t)(m__ << 1); NES_ZN(r__); r__; }))
#define NES_LSR(m_) (__extension__({ const uint8_t m__ = (m_); c = (uint8_t)(m__ & 1u); \
    const uint8_t r__ = (uint8_t)(m__ >> 1); NES_ZN(r__); r__; }))
#define NES_ROL(m_) (__extension__({ const uint8_t m__ = (m_); \
    const uint8_t r__ = (uint8_t)((m__ << 1) | c); c = (uint8_t)(m__ >> 7); NES_ZN(r__); r__; }))
#define NES_ROR(m_) (__extension__({ const uint8_t m__ = (m_); \
    const uint8_t r__ = (uint8_t)((m__ >> 1) | (c << 7)); c = (uint8_t)(m__ & 1u); NES_ZN(r__); r__; }))

#ifdef __cplusplus
}
#endif

#endif
