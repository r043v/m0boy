/* The HuC6280: the 6502's register set plus the MPR page registers, the T
 * flag and the Hudson instructions.  The interpreter runs the documented
 * opcode set, cross-checked against the mednafen core (huc6280_ops.inc):
 * cycle counts are mednafen's, the illegal opcodes are one-byte two-cycle
 * NOPs.  The bus windows land on rbase/wbase, kept in step with mpr[] by
 * huc6280_map(): a ROM or WRAM page reads and writes through a pointer,
 * the hardware page ($FF) and unmapped pages go through pce_bus_read and
 * pce_bus_write.  The zero page ($2000-$20FF) and the stack ($2100-$21FF)
 * are ordinary addresses of MPR1's window — usually the WRAM ($F8). */
#ifndef PCE_CPU_H
#define PCE_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PCE_P_C 0x01
#define PCE_P_Z 0x02
#define PCE_P_I 0x04
#define PCE_P_D 0x08
#define PCE_P_B 0x10
#define PCE_P_T 0x20
#define PCE_P_V 0x40
#define PCE_P_N 0x80

/* the interrupt controller's sources (the vectors: $FFF6 BRK and IRQ2,
 * $FFF8 IRQ1 = the VDC's line, $FFFA the timer) */
#define PCE_IQ_IRQ2  0x01
#define PCE_IQ_IRQ1  0x02
#define PCE_IQ_TIMER 0x04
#define PCE_IQ_RESET 0x20

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint32_t cycles;
    uint32_t steal;       /* wait states the bus asked for: charged after the instruction */
    uint8_t mpr[8];
    uint8_t irq_low;      /* the latched sources the mask lets through */
    uint8_t irq_mask;     /* $1402's value: bits 0-2 set = source enabled */
    int speed;            /* CSH/CSL; the cycle counts below ignore it */
    /* the bus's fast windows, in step with mpr[] */
    const uint8_t *rbase[8];
    uint8_t *wbase[8];
    /* the MPR bytes packed MPR0 first: the dispatcher's entry key, kept
     * in step by huc6280_map (every mpr[] write goes through it) */
    uint64_t mpr_key;
    /* the bus lands here */
    uint8_t *wram;
    const uint8_t *rom;
    uint32_t rom_bytes;
    uint8_t io_latch;     /* the I/O data bus's last value (open bus) */
} huc6280_t;

/* the ROM and WRAM pointers must be set before these */
void huc6280_power(huc6280_t *cpu);
void huc6280_reset(huc6280_t *cpu);
/* run until the cycle count reaches `until` (past the last instruction
 * that spends it, as the budget goes negative) */
void huc6280_run(huc6280_t *cpu, uint32_t until);
/* rebuild rbase/wbase from mpr[] (after TAM, or the reset) */
void huc6280_map(huc6280_t *cpu);

/* pce.c's bus: the hardware page and the unmapped windows */
uint8_t pce_bus_read(uint16_t at);
void pce_bus_write(uint16_t at, uint8_t v);

#ifdef __cplusplus
}
#endif
#endif
