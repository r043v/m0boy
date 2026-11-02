/* The 2A03's 6502 core: a plain interpreter with cycle counts, the NES
 * running it without decimal mode.  The bus (RAM, PPU, APU, mapper) is the
 * host's through NES_RD/NES_WR, defined before this header; interrupts
 * arrive as nmi_line/irq_line the host raises before a step. */
#ifndef NES_6502_H
#define NES_6502_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t nmi_edge;   /* the host sets it; the core takes the NMI once */
    uint8_t irq_line;   /* level: sampled before each instruction */
    uint32_t stall_pending; /* OAM DMA: master cycles the core must burn */
    uint32_t cycles;    /* master cycles consumed since reset */
    /* the bus's fast paths: RAM at $0000-$1FFF and the cartridge at
     * $8000+, which is where nearly every access lands (nes.c fills them).
     * The PRG sits behind two windows: $8000-$BFFF maps bank_base (0 on
     * NROM), $C000-$FFFF maps fixed_base (NROM 32 KiB: its second half,
     * banked: the last 16 KiB bank); an access reads
     * prg[(at & 0x3FFF) + window], and banked is the UxROM-style mapper a
     * write to $8000-$FFFF selects through bank. */
    uint8_t *ram;
    const uint8_t *prg;
    uint32_t bank_base;
    uint32_t fixed_base;
    uint8_t bank;
    uint8_t banked;
    uint8_t bank_count; /* the PRG's 16 KiB banks (1 when unbanked) */
} nes6502_t;

/* p: N V - B D I Z C */
#define NES_P_C 0x01
#define NES_P_Z 0x02
#define NES_P_I 0x04
#define NES_P_D 0x08
#define NES_P_B 0x10
#define NES_P_U 0x20
#define NES_P_V 0x40
#define NES_P_N 0x80

void nes6502_reset(nes6502_t *cpu);
/* Runs until `cycles` master cycles are consumed (dma included by the host
 * through nes6502_stall); returns when an instruction boundary is reached
 * and the budget is spent. */
void nes6502_run(nes6502_t *cpu, uint32_t cycles);
void nes6502_stall(nes6502_t *cpu, uint32_t cycles); /* OAM DMA, pauses */
/* NES_COVERAGE builds: the coverage tables to the cartridge's banks
 * (bank_count switchable slots, then the fixed one) */
void nes_coverage_init(unsigned banks);

#endif


#ifdef __cplusplus
}
#endif