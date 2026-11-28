/* The sound Z80: the Neo Geo Pocket's second CPU, 4 KiB of shared RAM at
 * main-bus 0x7000, the T6W28's two ports at its own 0x4000/0x4001, the
 * command byte at 0x8000, and 0xC00D... 0xC000 writes IRQ the main CPU.
 * It runs at half the main CPU's tick rate (one of its T-states per two
 * main ticks), the ratio the reference model uses.  The core is the
 * machine's own Z80 (targets/sg1000/runtime/z80.c). */
#ifndef M0_TL
#define M0_TL
#endif
#include "ngp.h"
#include <stddef.h>
#include <string.h>
#include "z80_ops.h"

static M0_TL z80_t z80_cpu;   /* one per machine instance, like `machine` */

static uint8_t ngp_z80_read(void *u, uint16_t addr) {
    (void)u;
    if (addr <= 0x0FFF) return machine.ram[0x3000 + addr];
    if (addr == 0x8000) return machine.comm_byte;
    return 0;
}

static void ngp_z80_write(void *u, uint16_t addr, uint8_t v) {
    (void)u;
    if (addr <= 0x0FFF) { machine.ram[0x3000 + addr] = v; return; }
    switch (addr) {
        case 0x8000: machine.comm_byte = v; return;
        case 0x4001: ngp_psg_write_left(v); return;
        case 0x4000: ngp_psg_write_right(v); return;
        case 0xC000: ngp_test_int_hdma(NGP_INT_Z80, 0x0C); return;
    }
}

static uint8_t ngp_z80_in(void *u, uint16_t port) {
    (void)u; (void)port;
    return 0;
}

static void ngp_z80_out(void *u, uint16_t port, uint8_t v) {
    (void)u; (void)port; (void)v;
    z80_cpu.irq = 0; /* a port write acknowledges the sound IRQ */
}

void ngp_z80_reset(void) {
    machine.z80_enabled = 0;
    memset(&z80_cpu, 0, sizeof z80_cpu);
    z80_cpu.read = ngp_z80_read;
    z80_cpu.write = ngp_z80_write;
    z80_cpu.in = ngp_z80_in;
    z80_cpu.out = ngp_z80_out;
    z80_reset(&z80_cpu);
}

/* the registers travel in the machine state: everything ahead of the host
 * pointers, which each instance keeps as its own reset set them */
_Static_assert(offsetof(z80_t, user) == NGP_Z80_STATE_BYTES, "NGP_Z80_STATE_BYTES");

void ngp_z80_state_io(uint8_t *p, int load) {
    if (!load) { memcpy(p, &z80_cpu, NGP_Z80_STATE_BYTES); return; }
    memcpy(&z80_cpu, p, NGP_Z80_STATE_BYTES);
    z80_cpu.read = ngp_z80_read;
    z80_cpu.write = ngp_z80_write;
    z80_cpu.in = ngp_z80_in;
    z80_cpu.out = ngp_z80_out;
}

void ngp_z80_irq(void) { z80_cpu.irq = 1; }
void ngp_z80_nmi(void) { z80_cpu.nmi = 1; }

void ngp_z80_run(uint32_t ticks) {
    machine.z80_runtime += (int32_t)ticks;
    while (machine.z80_runtime > 0) {
        if (!machine.z80_enabled) { machine.z80_runtime = 0; return; }
        const unsigned t = z80_step(&z80_cpu);
        if (!t) { z80_cpu.fault = 1; machine.z80_runtime = 0; return; }
        machine.z80_runtime -= (int32_t)(t << 1);
    }
}
