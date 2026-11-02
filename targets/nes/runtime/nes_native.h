/* The helpers recompiled 6502 code (nes_gen.c) shares with the interpreter:
 * the bus's two hot windows, the flags, the stack.  Everything is inline so
 * a recompiled instruction is a handful of Thumb instructions; the machine
 * behind the slow bus paths (PPU/APU/pads) is nes.c's. */
#ifndef NES_NATIVE_H
#define NES_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "6502.h"
#include <stdint.h>

extern uint8_t nes_bus_read(uint16_t);
extern void nes_bus_write(uint16_t, uint8_t);

static inline __attribute__((always_inline)) uint8_t nes_rd(nes6502_t *cpu, uint16_t at) {
    if (at < 0x2000u) return cpu->ram[at & 0x07FFu];
    if (at >= 0x8000u)
        return cpu->prg[(at & 0x3FFFu) + (at >= 0xC000u ? cpu->fixed_base : cpu->bank_base)];
    return nes_bus_read(at);
}
static inline __attribute__((always_inline)) void nes_wr(nes6502_t *cpu, uint16_t at, uint8_t v) {
    if (at < 0x2000u) { cpu->ram[at & 0x07FFu] = v; return; }
    nes_bus_write(at, v);
}
static inline __attribute__((always_inline)) void nes_zn(nes6502_t *cpu, uint8_t v) {
    cpu->p = (uint8_t)((cpu->p & ~(0x80u | 0x02u)) | (v ? 0 : 0x02u) | (v & 0x80u));
}
static inline __attribute__((always_inline)) uint8_t nes_adc(nes6502_t *cpu, uint8_t m) {
    const unsigned sum = cpu->a + m + (cpu->p & 1u);
    cpu->p = (uint8_t)((cpu->p & ~(0x01u | 0x40u)) | (sum > 0xFF ? 0x01u : 0) |
                       ((~(cpu->a ^ m) & (cpu->a ^ sum) & 0x80u) ? 0x40u : 0));
    cpu->a = (uint8_t)sum;
    nes_zn(cpu, cpu->a);
    return cpu->a;
}
static inline __attribute__((always_inline)) uint8_t nes_sbc(nes6502_t *cpu, uint8_t m) {
    return nes_adc(cpu, (uint8_t)~m);
}
static inline __attribute__((always_inline)) void nes_cmp(nes6502_t *cpu, uint8_t r, uint8_t m) {
    const unsigned d = (unsigned)r - m;
    cpu->p = (uint8_t)((cpu->p & ~0x01u) | (r >= m ? 0x01u : 0));
    nes_zn(cpu, (uint8_t)d);
}
static inline __attribute__((always_inline)) void nes_push(nes6502_t *cpu, uint8_t v) {
    cpu->ram[0x0100u | cpu->s] = v;
    cpu->s--;
}
static inline __attribute__((always_inline)) uint8_t nes_pop(nes6502_t *cpu) {
    cpu->s++;
    return cpu->ram[0x0100u | cpu->s];
}
static inline __attribute__((always_inline)) uint8_t nes_asl(nes6502_t *cpu, uint8_t m) {
    cpu->p = (uint8_t)((cpu->p & ~0x01u) | (m >> 7));
    return (uint8_t)(m << 1);
}
static inline __attribute__((always_inline)) uint8_t nes_lsr(nes6502_t *cpu, uint8_t m) {
    cpu->p = (uint8_t)((cpu->p & ~0x01u) | (m & 1u));
    return (uint8_t)(m >> 1);
}
static inline __attribute__((always_inline)) uint8_t nes_rol(nes6502_t *cpu, uint8_t m) {
    const uint8_t c = (uint8_t)(cpu->p & 1u);
    cpu->p = (uint8_t)((cpu->p & ~0x01u) | (m >> 7));
    const uint8_t r = (uint8_t)((m << 1) | c);
    nes_zn(cpu, r);
    return r;
}
static inline __attribute__((always_inline)) uint8_t nes_ror(nes6502_t *cpu, uint8_t m) {
    const uint8_t c = (uint8_t)((cpu->p & 1u) ? 0x80u : 0);
    cpu->p = (uint8_t)((cpu->p & ~0x01u) | (m & 1u));
    const uint8_t r = (uint8_t)((m >> 1) | c);
    nes_zn(cpu, r);
    return r;
}

#ifdef __cplusplus
}
#endif

#endif
