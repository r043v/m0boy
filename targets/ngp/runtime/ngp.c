/* Neo Geo Pocket machine: the bus, the TLCS-900H's internal I/O (timers,
 * interrupt controller, micro-DMA, RTC, serial), the cartridge windows with
 * their flash-write behaviour, and the frame loop.  The register semantics
 * follow the model the NGP emulation lineage settled on (the NeoPop family,
 * cross-checked against the TMP95C061 datasheet notes the Ngpcraft project
 * collected): 515 ticks a line, 198 lines a frame, the V-INT raised when
 * line 152 is entered. */
#ifndef M0_TL
#define M0_TL
#endif
#include "ngp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

M0_TL ngp_t machine;

/* the reset values the modelled I/O page registers hold (the values the
 * BIOS's power-on code and the power-on state leave; unmodelled registers
 * read zero) */
static const uint8_t system_memory[0x100] = {
    [0x20] = 0x80, [0x22] = 0x01, [0x23] = 0x90, [0x24] = 0x03, [0x25] = 0xB0,
    [0x26] = 0x90, [0x27] = 0x62, [0x28] = 0x05, [0x29] = 0x00,
    [0x70] = 0x02, [0x71] = 0x32, [0xB8] = 0xAA, [0xB9] = 0xAA,
};

/* ---- flash saves ----------------------------------------------------------- */

static void flash_mark(uint32_t addr) {
    const uint32_t block = addr >> 13;
    if (block < NGP_FLASH_BLOCKS) machine.flash_dirty[block >> 5] |= 1u << (block & 31);
}

/* a cartridge-window address as the offset into the rom image */
static uint32_t flash_offset(uint32_t addr) {
    if (addr >= 0x200000 && addr < 0x400000) return addr - 0x200000;
    if (addr >= 0x800000 && addr < 0xA00000) return addr - 0x800000 + 0x200000;
    return 0xFFFFFFFFu;
}

uint32_t ngp_flash_bytes(void) {
    uint32_t i, bytes = sizeof machine.flash_dirty;
    for (i = 0; i < machine.flash_blocks; i++)
        if (machine.flash_dirty[i >> 5] & (1u << (i & 31))) bytes += 8 + NGP_FLASH_BLOCK;
    return bytes;
}

void ngp_flash_io(void *ctx,
                   void (*put)(void *ctx, const void *data, uint32_t n),
                   int (*get)(void *ctx, void *data, uint32_t n)) {
    uint32_t i;
    if (get) {
        get(ctx, machine.flash_dirty, sizeof machine.flash_dirty);
        for (i = 0; i < NGP_FLASH_BLOCKS; i++)
            if ((machine.flash_dirty[i >> 5] & (1u << (i & 31))) && i * NGP_FLASH_BLOCK < machine.rom_size) {
                uint32_t header[2];
                get(ctx, header, sizeof header);
                get(ctx, machine.rom + header[0], NGP_FLASH_BLOCK);
            }
    } else {
        put(ctx, machine.flash_dirty, sizeof machine.flash_dirty);
        for (i = 0; i < machine.flash_blocks; i++)
            if (machine.flash_dirty[i >> 5] & (1u << (i & 31))) {
                const uint32_t header[2] = { i * NGP_FLASH_BLOCK, NGP_FLASH_BLOCK };
                put(ctx, header, sizeof header);
                put(ctx, machine.rom + i * NGP_FLASH_BLOCK, NGP_FLASH_BLOCK);
            }
    }
}

/* ---- interrupt controller -------------------------------------------------- */

uint32_t ngp_irq_count, ngp_vint_count;
void ngp_interrupt(unsigned index, uint8_t level) {
    ngp_irq_count++;
    if (index == NGP_INT_VBLANK) ngp_vint_count++;
    /* the entry: PC pushed, SR pushed, the mask raised to the level, and
     * the vector taken from the RAM table (the BIOS chains every interrupt
     * through it); ~13 cycles per the TLCS-900H spec */
    uint32_t sp = machine.cpu.gpr[3];
    machine.cyc += 13;
    sp -= 4;
    machine.ram[(sp - 0x4000) & (NGP_RAM_BYTES - 1)] = (uint8_t)machine.cpu.pc;
    machine.ram[(sp - 0x4000 + 1) & (NGP_RAM_BYTES - 1)] = (uint8_t)(machine.cpu.pc >> 8);
    machine.ram[(sp - 0x4000 + 2) & (NGP_RAM_BYTES - 1)] = (uint8_t)(machine.cpu.pc >> 16);
    machine.ram[(sp - 0x4000 + 3) & (NGP_RAM_BYTES - 1)] = 0;
    sp -= 2;
    machine.ram[(sp - 0x4000) & (NGP_RAM_BYTES - 1)] = (uint8_t)machine.cpu.sr;
    machine.ram[(sp - 0x4000 + 1) & (NGP_RAM_BYTES - 1)] = (uint8_t)(machine.cpu.sr >> 8);
    machine.cpu.gpr[3] = sp;
    machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0x8FFF) | (((level < 7 ? level + 1 : 7) & 7) << 12));
    {
        const uint32_t v = NGP_IRQ_TABLE - 0x4000 + index * 4;
        machine.cpu.pc = machine.ram[v] | ((uint32_t)machine.ram[v + 1] << 8) |
                         ((uint32_t)machine.ram[v + 2] << 16);
    }
    machine.cpu.halted = 0;
}

void ngp_set_int(unsigned index, int set) {
    if (index < 24) machine.int_pending[index] = (uint8_t)(set != 0);
    ngp_check_pending();
}

/* the sources in the datasheet's own priority order (ties: lowest index) */
void ngp_check_pending(void) {
    static const struct { uint8_t index, reg, nibble; } sources[] = {
        { NGP_INT_VBLANK, 0x1, 0 }, { NGP_INT_Z80, 0x1, 1 },
        { NGP_INT_TIMER0, 0x3, 0 }, { NGP_INT_TIMER1, 0x3, 1 },
        { NGP_INT_TIMER2, 0x4, 0 }, { NGP_INT_TIMER3, 0x4, 1 },
        { NGP_INT_RX0, 0x7, 0 },    { NGP_INT_TX0, 0x7, 1 },
    };
    uint8_t mask;
    unsigned i;
    if (machine.cpu.halted) return;
    mask = (uint8_t)((machine.cpu.sr >> 12) & 7);
    if (mask == 1) return; /* DI */
    for (i = 0; i < sizeof sources / sizeof *sources; i++) {
        const uint8_t raw = (uint8_t)((machine.int_prio[sources[i].reg] >> (sources[i].nibble * 4)) & 7);
        /* level 0 and 7 are disabled; the gate is the lineage's one
         * (level >= mask, the mask stored as the level+1 of the last accept) */
        if (machine.int_pending[sources[i].index] && raw && raw != 7 && mask <= raw) {
            machine.int_pending[sources[i].index] = 0;
            ngp_interrupt(sources[i].index, raw);
            return;
        }
    }
}

/* ---- micro-DMA -------------------------------------------------------------- */

static void dma_update(unsigned channel) {
    const uint8_t mode = (uint8_t)((machine.dma_m[channel] & 0x1C) >> 2);
    const uint8_t size = (uint8_t)(machine.dma_m[channel] & 3);
    if (!machine.dma_c[channel]) return;
    switch (mode) {
        case 0: /* destination INC, I/O to memory */
            if (size == 0) { ngp_write(machine.dma_d[channel], ngp_read(machine.dma_s[channel])); machine.dma_d[channel] += 1; }
            else if (size == 1) { ngp_write16(machine.dma_d[channel], ngp_read16(machine.dma_s[channel])); machine.dma_d[channel] += 2; }
            else { ngp_write32(machine.dma_d[channel], ngp_read32(machine.dma_s[channel])); machine.dma_d[channel] += 4; }
            break;
        case 1: /* destination DEC */
            if (size == 0) { ngp_write(machine.dma_d[channel], ngp_read(machine.dma_s[channel])); machine.dma_d[channel] -= 1; }
            else if (size == 1) { ngp_write16(machine.dma_d[channel], ngp_read16(machine.dma_s[channel])); machine.dma_d[channel] -= 2; }
            else { ngp_write32(machine.dma_d[channel], ngp_read32(machine.dma_s[channel])); machine.dma_d[channel] -= 4; }
            break;
        case 2: /* source INC, memory to I/O */
            if (size == 0) { ngp_write(machine.dma_d[channel], ngp_read(machine.dma_s[channel])); machine.dma_s[channel] += 1; }
            else if (size == 1) { ngp_write16(machine.dma_d[channel], ngp_read16(machine.dma_s[channel])); machine.dma_s[channel] += 2; }
            else { ngp_write32(machine.dma_d[channel], ngp_read32(machine.dma_s[channel])); machine.dma_s[channel] += 4; }
            break;
        case 3: /* source DEC */
            if (size == 0) { ngp_write(machine.dma_d[channel], ngp_read(machine.dma_s[channel])); machine.dma_s[channel] -= 1; }
            else if (size == 1) { ngp_write16(machine.dma_d[channel], ngp_read16(machine.dma_s[channel])); machine.dma_s[channel] -= 2; }
            else { ngp_write32(machine.dma_d[channel], ngp_read32(machine.dma_s[channel])); machine.dma_s[channel] -= 4; }
            break;
        case 4: /* fixed addresses */
            if (size == 0) ngp_write(machine.dma_d[channel], ngp_read(machine.dma_s[channel]));
            else if (size == 1) ngp_write16(machine.dma_d[channel], ngp_read16(machine.dma_s[channel]));
            else ngp_write32(machine.dma_d[channel], ngp_read32(machine.dma_s[channel]));
            break;
        case 5: /* count only */
            machine.dma_s[channel]++;
            break;
    }
    machine.dma_c[channel]--;
    if (!machine.dma_c[channel]) {
        ngp_interrupt(NGP_INT_DMA0 + channel, 7);
        machine.hdma_vector[channel] = 0;
    }
}

/* a hardware IRQ source fires: it either starts a micro-DMA channel whose
 * start vector names it (the vector numbers are the hardware IRQ numbers:
 * 0x0B V-INT, 0x0C Z80, 0x10-0x13 timers, 0x18/0x19 serial), or raises the
 * software interrupt */
void ngp_test_int_hdma(unsigned index, uint8_t vector) {
    unsigned i;
    for (i = 0; i < 4; i++)
        if (machine.hdma_vector[i] == vector) { dma_update(i); return; }
    ngp_set_int(index, 1);
}

/* ---- timers ----------------------------------------------------------------- */

#define TIMER_T1_RATE  (8 * 32)
#define TIMER_T4_RATE  (32 * 32)
#define TIMER_T16_RATE (128 * 32)
#define TIMER_T256_RATE (2048 * 32)

void ngp_tick(uint32_t ticks) {
    machine.cyc += ticks;
    machine.timer_hint += ticks;
    while (machine.timer_hint >= NGP_LINE_TICKS) {
        machine.timer_hint -= NGP_LINE_TICKS;
        machine.h_int = (uint8_t)ngp_gfx_line();
        if (machine.v_int) return; /* the frame boundary: stop here */
    }
    /* timer 0 */
    machine.timer0_tick = 0;
    machine.timer_clock[0] += ticks;
    if (machine.trun & 0x01) {
        switch (machine.t01mod & 3) {
            case 0: /* H-INT trigger */
                if (machine.h_int) { machine.timer_tick[0]++; machine.timer_clock[0] = 0; machine.h_int = 0; }
                break;
            case 1:
                while (machine.timer_clock[0] >= TIMER_T1_RATE) { machine.timer_tick[0]++; machine.timer_clock[0] -= TIMER_T1_RATE; }
                break;
            case 2:
                while (machine.timer_clock[0] >= TIMER_T4_RATE) { machine.timer_tick[0]++; machine.timer_clock[0] -= TIMER_T4_RATE; }
                break;
            case 3:
                while (machine.timer_clock[0] >= TIMER_T16_RATE) { machine.timer_tick[0]++; machine.timer_clock[0] -= TIMER_T16_RATE; }
                break;
        }
        if (machine.timer_threshold[0] && machine.timer_tick[0] >= machine.timer_threshold[0]) {
            machine.timer_tick[0] = 0;
            machine.timer0_tick = 1;
            ngp_test_int_hdma(NGP_INT_TIMER0, 0x10);
        }
    }
    /* timer 1 */
    machine.timer_clock[1] += ticks;
    if (machine.trun & 0x02) {
        switch ((machine.t01mod >> 2) & 3) {
            case 0:
                if (machine.timer0_tick) { machine.timer_tick[1]++; machine.timer_clock[1] = 0; }
                break;
            case 1:
                while (machine.timer_clock[1] >= TIMER_T1_RATE) { machine.timer_tick[1]++; machine.timer_clock[1] -= TIMER_T1_RATE; }
                break;
            case 2:
                while (machine.timer_clock[1] >= TIMER_T16_RATE) { machine.timer_tick[1]++; machine.timer_clock[1] -= TIMER_T16_RATE; }
                break;
            case 3:
                while (machine.timer_clock[1] >= TIMER_T256_RATE) { machine.timer_tick[1]++; machine.timer_clock[1] -= TIMER_T256_RATE; }
                break;
        }
        if (machine.timer_threshold[1] && machine.timer_tick[1] >= machine.timer_threshold[1]) {
            machine.timer_tick[1] = 0;
            ngp_test_int_hdma(NGP_INT_TIMER1, 0x11);
        }
    }
    /* timer 2 */
    machine.timer2_tick = 0;
    machine.timer_clock[2] += ticks;
    if (machine.trun & 0x04) {
        switch (machine.t23mod & 3) {
            case 0: break;
            case 1:
                while (machine.timer_clock[2] >= TIMER_T1_RATE / 2) { machine.timer_tick[2]++; machine.timer_clock[2] -= TIMER_T1_RATE / 2; }
                break;
            case 2:
                while (machine.timer_clock[2] >= TIMER_T4_RATE) { machine.timer_tick[2]++; machine.timer_clock[2] -= TIMER_T4_RATE; }
                break;
            case 3:
                while (machine.timer_clock[2] >= TIMER_T16_RATE) { machine.timer_tick[2]++; machine.timer_clock[2] -= TIMER_T16_RATE; }
                break;
        }
        if (machine.timer_threshold[2] && machine.timer_tick[2] >= machine.timer_threshold[2]) {
            machine.timer_tick[2] = 0;
            machine.timer2_tick = 1;
            ngp_test_int_hdma(NGP_INT_TIMER2, 0x12);
        }
    }
    /* timer 3: the threshold also IRQs the sound Z80 */
    machine.timer_clock[3] += ticks;
    if (machine.trun & 0x08) {
        switch ((machine.t23mod >> 2) & 3) {
            case 0:
                if (machine.timer2_tick) { machine.timer_tick[3]++; machine.timer_clock[3] = 0; }
                break;
            case 1:
                while (machine.timer_clock[3] >= TIMER_T1_RATE) { machine.timer_tick[3]++; machine.timer_clock[3] -= TIMER_T1_RATE; }
                break;
            case 2:
                while (machine.timer_clock[3] >= TIMER_T16_RATE) { machine.timer_tick[3]++; machine.timer_clock[3] -= TIMER_T16_RATE; }
                break;
            case 3:
                while (machine.timer_clock[3] >= TIMER_T256_RATE) { machine.timer_tick[3]++; machine.timer_clock[3] -= TIMER_T256_RATE; }
                break;
        }
        if (machine.timer_threshold[3] && machine.timer_tick[3] >= machine.timer_threshold[3]) {
            machine.timer_tick[3] = 0;
            ngp_z80_irq();
            ngp_test_int_hdma(NGP_INT_TIMER3, 0x13);
        }
    }
    /* the T6W28 runs at half the CPU clock: one of its ticks per two */
    machine.psg_runtime += (int32_t)ticks;
    while (machine.psg_runtime >= 2) {
        ngp_psg_tick();
        machine.psg_runtime -= 2;
    }
    ngp_z80_run(ticks);
    ngp_check_pending();
}

/* ---- the I/O page (0x00-0xFF) ------------------------------------------------ */

static uint8_t io_read(uint32_t addr) {
    switch (addr) {
        case 0x20: return machine.trun;
        case 0x21: return 0;                    /* unused: the modelled timers read zero */
        case 0x22: return machine.timer_threshold[0];
        case 0x23: return machine.timer_threshold[1];
        case 0x24: return machine.t01mod;
        case 0x25: return machine.tffcr;
        case 0x26: return machine.timer_threshold[2];
        case 0x27: return machine.timer_threshold[3];
        case 0x28: return machine.t23mod;
        case 0x29: return machine.trdc;
        case 0x50: return machine.serial_buf;
        case 0x70: case 0x72: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A:
            return machine.int_prio[addr - 0x70];
        case 0x71:
            return (uint8_t)((machine.int_pending[NGP_INT_VBLANK] ? 0x08 : 0) |
                             (machine.int_pending[NGP_INT_Z80] ? 0x80 : 0));
        case 0x73:
            return (uint8_t)((machine.int_pending[NGP_INT_TIMER0] ? 0x08 : 0) |
                             (machine.int_pending[NGP_INT_TIMER1] ? 0x80 : 0));
        case 0x74:
            return (uint8_t)((machine.int_pending[NGP_INT_TIMER2] ? 0x08 : 0) |
                             (machine.int_pending[NGP_INT_TIMER3] ? 0x80 : 0));
        case 0x77:
            return (uint8_t)((machine.int_pending[NGP_INT_RX0] ? 0x08 : 0) |
                             (machine.int_pending[NGP_INT_TX0] ? 0x80 : 0));
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: return machine.hdma_vector[addr - 0x7C];
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: return machine.rtc_latch[addr - 0x91];
        case 0xA0: return ngp_psg_read_right();
        case 0xA1: return ngp_psg_read_left();
        case 0xBC: return machine.comm_byte;
        default: return 0;
    }
}

static void io_write(uint32_t addr, uint8_t v) {
    switch (addr) {
        case 0x20:
            machine.trun = v;
            if (!(v & 0x01)) machine.timer_tick[0] = 0;
            if (!(v & 0x02)) machine.timer_tick[1] = 0;
            if (!(v & 0x04)) machine.timer_tick[2] = 0;
            if (!(v & 0x08)) machine.timer_tick[3] = 0;
            return;
        case 0x22: machine.timer_threshold[0] = v; return;
        case 0x23: machine.timer_threshold[1] = v; return;
        case 0x24: machine.t01mod = v; return;
        case 0x25: machine.tffcr = v & 0x33; return;
        case 0x26: machine.timer_threshold[2] = v; return;
        case 0x27: machine.timer_threshold[3] = v; return;
        case 0x28: machine.t23mod = v; return;
        case 0x29: machine.trdc = v & 3; return;
        case 0x50: machine.serial_buf = v; return;
        case 0x6F: return; /* watchdog: the write is accepted, nothing happens */
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74:
        case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A:
            machine.int_prio[addr - 0x70] = v;
            /* a cleared enable nibble clears the source's pending latch */
            if (addr == 0x71) {
                if (!(v & 0x08)) machine.int_pending[NGP_INT_VBLANK] = 0;
                if (!(v & 0x80)) machine.int_pending[NGP_INT_Z80] = 0;
            } else if (addr == 0x73) {
                if (!(v & 0x08)) machine.int_pending[NGP_INT_TIMER0] = 0;
                if (!(v & 0x80)) machine.int_pending[NGP_INT_TIMER1] = 0;
            } else if (addr == 0x74) {
                if (!(v & 0x08)) machine.int_pending[NGP_INT_TIMER2] = 0;
                if (!(v & 0x80)) machine.int_pending[NGP_INT_TIMER3] = 0;
            } else if (addr == 0x77) {
                if (!(v & 0x08)) machine.int_pending[NGP_INT_RX0] = 0;
                if (!(v & 0x80)) machine.int_pending[NGP_INT_TX0] = 0;
            }
            ngp_check_pending();
            return;
        case 0x7C: machine.hdma_vector[0] = v; return;
        case 0x7D: machine.hdma_vector[1] = v; return;
        case 0x7E: machine.hdma_vector[2] = v; return;
        case 0x7F: machine.hdma_vector[3] = v; return;
        case 0xA0: if (machine.chip_enabled) ngp_psg_write_right(v); return;
        case 0xA1: if (machine.chip_enabled) ngp_psg_write_left(v); return;
        case 0xA2: ngp_dac_write_left(v); return;
        case 0xA3: ngp_dac_write_right(v); return;
        case 0xB2: machine.comm_status = v & 1; return;
        case 0xB8:
            if (v == 0x55) machine.chip_enabled = 1;
            else if (v == 0xAA) { machine.chip_enabled = 0; ngp_psg_reset(); }
            return;
        case 0xB9:
            if (v == 0x55) machine.z80_enabled = 1;
            else if (v == 0xAA) { machine.z80_enabled = 0; ngp_z80_reset(); }
            return;
        case 0xBA: ngp_z80_nmi(); return;
        case 0xBC: machine.comm_byte = v; return;
        default: return;
    }
}

/* ---- the bus ---------------------------------------------------------------- */

uint8_t ngp_read(uint32_t addr) {
    uint32_t off;
    addr &= 0xFFFFFF;
    if (addr < 0x100) return io_read(addr);
    if (addr >= 0x4000 && addr < 0x8000) return machine.ram[addr - 0x4000];
    if (addr >= 0x8000 && addr < 0xC000) return ngp_gfx_read(addr);
    off = flash_offset(addr);
    if (off != 0xFFFFFFFFu) {
        if (machine.flash_status && (off & 0xFF0000) == 0x20000) {
            machine.flash_status = 0;
            if (addr == 0x220000 || addr == 0x230000) return 0xFF;
        }
        return off < machine.rom_size ? machine.rom[off] : 0;
    }
    if ((addr & 0xFF0000) == 0xFF0000) return machine.bios[addr & 0xFFFF];
    return 0;
}

void ngp_write(uint32_t addr, uint8_t v) {
    uint32_t off;
    addr &= 0xFFFFFF;
    if (addr < 0x100) { io_write(addr, v); return; }
    if (addr >= 0x4000 && addr < 0x8000) { machine.ram[addr - 0x4000] = v; return; }
    if (addr >= 0x8000 && addr < 0xC000) { ngp_gfx_write(addr, v); return; }
    off = flash_offset(addr);
    if (off != 0xFFFFFFFFu) {
        if (!machine.flash_unlocked) {
            if (off == 0x2AAA || off == 0x5555) { machine.flash_command = 1; return; }
            if (off == 0x20000 || off == 0x30000) { machine.flash_status = 1; return; }
            if (machine.flash_command) {
                machine.flash_command = 0;
                if (off < machine.rom_size) { machine.rom[off] = v; flash_mark(off); }
                return;
            }
            return; /* protected: the write is ignored */
        }
        if (off < machine.rom_size) { machine.rom[off] = v; flash_mark(off); }
        return;
    }
    /* the BIOS is ROM */
}

uint16_t ngp_read16(uint32_t addr) {
    return (uint16_t)(ngp_read(addr) | (ngp_read(addr + 1) << 8));
}

void ngp_write16(uint32_t addr, uint16_t v) {
    ngp_write(addr, (uint8_t)v);
    ngp_write(addr + 1, (uint8_t)(v >> 8));
}

uint32_t ngp_read32(uint32_t addr) {
    return ngp_read16(addr) | ((uint32_t)ngp_read16(addr + 2) << 16);
}

void ngp_write32(uint32_t addr, uint32_t v) {
    ngp_write16(addr, (uint16_t)v);
    ngp_write16(addr + 2, (uint16_t)(v >> 16));
}

/* ---- cartridge ---------------------------------------------------------------- */

int ngp_load_rom(const uint8_t *data, uint32_t bytes) {
    if (bytes > NGP_ROM_MAX) return 0;
    machine.rom = (uint8_t *)data;
    machine.rom_size = bytes;
    machine.flash_blocks = (bytes + NGP_FLASH_BLOCK - 1) / NGP_FLASH_BLOCK;
    if (machine.flash_blocks > NGP_FLASH_BLOCKS) machine.flash_blocks = NGP_FLASH_BLOCKS;
    return 1;
}

/* ---- the frame ---------------------------------------------------------------- */

unsigned ngp_run_frame(void) {
    uint32_t steps = 0;
    machine.frames++;
    machine.v_int = 0;
    while (!machine.v_int) {
        if (machine.cpu.halted) {
            /* HALT waits for the next line; the ticks raise the IRQs and an
             * accepted interrupt clears the halt */
            ngp_tick(NGP_LINE_TICKS);
            machine.executed++;
            continue;
        }
        ngp_tick((uint32_t)ngp_step());
        machine.executed++;
        /* a stuck frame: the interpreter never yields (a live lock the
         * model does not reach normally) — mark and bail so the harness
         * still reports */
        if (++steps > 4000000u) {
            machine.cpu.fault = 2;
            break;
        }
    }
    return machine.gfx.raster_line;
}

/* ---- reset --------------------------------------------------------------------- */

/* the BIOS workspace the power-on code would have filled: the header the
 * game is handed (startPC, catalog, name), the default vectors, the power
 * registers.  The synthesised BIOS never runs its power-on code, so the
 * machine writes the same bytes itself. */
static void bios_workspace(void) {
    const uint8_t *h = machine.rom;
    uint32_t i;
    if (machine.rom_size >= 0x30) {
        machine.ram[0x6C00 - 0x4000] = h[0x1C];
        machine.ram[0x6C01 - 0x4000] = h[0x1D];
        machine.ram[0x6C02 - 0x4000] = h[0x1E];
        machine.ram[0x6C03 - 0x4000] = h[0x1F];
        machine.ram[0x6C04 - 0x4000] = h[0x20];
        machine.ram[0x6C05 - 0x4000] = h[0x21];
        machine.ram[0x6C06 - 0x4000] = h[0x22];
        machine.ram[0x6E82 - 0x4000] = h[0x20];
        machine.ram[0x6E83 - 0x4000] = h[0x21];
        machine.ram[0x6E84 - 0x4000] = h[0x22];
        for (i = 0; i < 12; i++) machine.ram[0x6C08 - 0x4000 + i] = h[0x24 + i];
    }
    machine.ram[0x6C55 - 0x4000] = 0x01;                        /* commercial */
    /* 0x6C58 is the flash card type the BIOS learnt by probing the chip
     * (measured ladder: 1 = 4 Mbit, 2 = 8 Mbit, 3 = 16 Mbit, 0 = none).
     * The save syscalls read it first, and the colour-aware carts read it
     * too.  0x6C59 is the CS1 development slot: 0 on a production console. */
    machine.ram[0x6C58 - 0x4000] = machine.rom_size <= 0x080000 ? 1
                                 : machine.rom_size <= 0x100000 ? 2 : 3;
    machine.ram[0x6C59 - 0x4000] = 0x00;
    machine.ram[0x6F80 - 0x4000] = 0xFF;                        /* ADC/contrast full-scale */
    machine.ram[0x6F81 - 0x4000] = 0x03;
    machine.ram[0x6F83 - 0x4000] = 0x10;                        /* the BIOS's own
     * workspace stamp, cross-checked against reference memory dumps */
    machine.ram[0x6F84 - 0x4000] = 0x40;                        /* system status */
    machine.ram[0x6F86 - 0x4000] = 0x40;
    machine.ram[0x6F87 - 0x4000] = 0x01;                        /* english */
    /* 0x6F91/0x6F92 is how a cartridge knows the machine it sits in: the
     * colour pocket stamps 0x10/0x03, the mono one 0x00 and nothing at
     * 0x6F92.  A colour-aware mono game (Samurai Shodown!) tests this byte
     * and only runs its colourisation code on a colour console. */
    machine.ram[0x6F91 - 0x4000] = 0x10;
    machine.ram[0x6F92 - 0x4000] = 0x03;
    machine.ram[0x6F95 - 0x4000] = machine.rom_size >= 0x30 ? h[0x23] : 0x10;
    /* the 18-slot vector table filled with the BIOS's RETI stub, as the
     * power-on code does (0x6FB8, four bytes an entry) */
    for (i = 0; i < NGP_IRQ_SLOTS; i++) {
        const uint32_t v = NGP_IRQ_TABLE - 0x4000 + i * 4;
        machine.ram[v] = (uint8_t)NGP_IRQ_STUB;
        machine.ram[v + 1] = (uint8_t)(NGP_IRQ_STUB >> 8);
        machine.ram[v + 2] = (uint8_t)(NGP_IRQ_STUB >> 16);
        machine.ram[v + 3] = 0;
    }
}

void ngp_reset(void) {
    uint32_t i;
    uint8_t buttons = machine.buttons;
    uint8_t *rom = machine.rom, *bios = machine.bios;
    uint32_t rom_size = machine.rom_size, flash_blocks = machine.flash_blocks;
    uint32_t dirty[sizeof machine.flash_dirty / 4];
    memcpy(dirty, machine.flash_dirty, sizeof dirty);
    memset(&machine, 0, sizeof machine);
    machine.buttons = buttons;
    machine.rom = rom; machine.bios = bios; machine.rom_size = rom_size;
    machine.flash_blocks = flash_blocks;
    memcpy(machine.flash_dirty, dirty, sizeof dirty);
    /* the RTC as a fixed power-on time: 2026-01-01, monday, 00:00:00 (BCD) */
    machine.rtc_latch[0] = 0x26; machine.rtc_latch[1] = 0x01;
    machine.rtc_latch[2] = 0x01; machine.rtc_latch[3] = 0x00;
    machine.rtc_latch[4] = 0x00; machine.rtc_latch[5] = 0x00;
    machine.rtc_latch[6] = 0x04;
    /* the CPU: SR 0xF800 (mask level 7, everything masked), XSP in the BIOS
     * workspace, the global registers as the BIOS would have left them */
    machine.cpu.sr = 0xF800;
    machine.cpu.gpr[0] = 0xFF23C3;
    machine.cpu.gpr[1] = 0xFF23DF;
    machine.cpu.gpr[2] = 0x006480;
    machine.cpu.gpr[3] = 0x00006C00;
    machine.cpu.pc = machine.rom_size >= 0x20
        ? machine.rom[0x1C] | (machine.rom[0x1D] << 8) | ((uint32_t)machine.rom[0x1E] << 16)
        : 0xFFFFFE;
    /* the modelled I/O registers' power-on values */
    for (i = 0; i < sizeof system_memory; i++)
        if (system_memory[i]) io_write(i, system_memory[i]);
    /* the K2GE's power-on state */
    machine.gfx.control_int = 0xC0;
    machine.gfx.wsi_h = 0xFF; machine.gfx.wsi_v = 0xFF;
    machine.gfx.screen_period = 0xC6;
    machine.gfx.bg_col = 0x80;
    machine.gfx.palette_ram[0x1E0] = 0xFF; machine.gfx.palette_ram[0x1E1] = 0x0F;
    machine.gfx.palette_ram[0x1F0] = 0xFF; machine.gfx.palette_ram[0x1F1] = 0x0F;
    /* the K1GE-compat palette: in mono mode a 3-bit level resolves through
     * these 12-bit entries (16 per plane: sprite 0x8380, scroll1 0x83A0,
     * scroll2 0x83C0, backdrop 0x83E0).  The BIOS computes the grey ramp
     * into all four planes at boot; a colour-aware game repaints entries
     * with real colours (the tint a colour console applies to mono carts) */
    {
        static const uint16_t grey_ramp[8] = {
            0x0FFF, 0x0DDD, 0x0BBB, 0x0999, 0x0777, 0x0444, 0x0333, 0x0000
        };
        for (unsigned base = 0x180; base <= 0x1E0; base += 0x20)
            for (unsigned e = 0; e < 16; e++) {
                machine.gfx.palette_ram[base + e * 2] = (uint8_t)(grey_ramp[e & 7] & 0xFF);
                machine.gfx.palette_ram[base + e * 2 + 1] = (uint8_t)(grey_ramp[e & 7] >> 8);
            }
    }
    memset(machine.gfx.spplt, 7, sizeof machine.gfx.spplt);
    memset(machine.gfx.scrp1plt, 7, sizeof machine.gfx.scrp1plt);
    memset(machine.gfx.scrp2plt, 7, sizeof machine.gfx.scrp2plt);
    /* the boot mode follows the cartridge header's byte 0x23 (0x10 = colour),
     * which the BIOS reads at power-on — bios_workspace() below copies the
     * same byte to 0x6F95 for the game */
    machine.gfx.k2ge_mode = (machine.rom_size >= 0x30 && (machine.rom[0x23] & 0x10)) ? 0 : 0x80;
    bios_workspace();
    /* Metal Slug 2nd Mission relies on a specific character RAM pattern the
     * power-on code leaves (a boot artifact, documented by mednafen) */
    {
        static const uint8_t char_data[64] = {
            255, 63, 255, 255, 0, 252, 255, 255, 255, 63, 3, 0, 255, 255, 255, 255,
            240, 243, 252, 243, 255, 3, 255, 195, 255, 243, 243, 243, 240, 243, 240, 195,
            207, 15, 207, 15, 207, 15, 207, 207, 207, 255, 207, 255, 207, 255, 207, 63,
            255, 192, 252, 195, 240, 207, 192, 255, 192, 255, 240, 207, 252, 195, 255, 192 };
        memcpy(machine.gfx.character_ram + 0x1C0, char_data, 64);
    }
    ngp_psg_reset();
    ngp_z80_reset();
}

/* ---- state --------------------------------------------------------------------- */

/* everything but the pointers (rom, bios, flash_shadow) and the bookkeeping
 * the harness owns; the flash dirty bits and the rom contents travel through
 * ngp_flash_io.  A span must never cross a pointer member: offsetof maths on
 * members in struct order, or it silently wraps. */
static const struct { size_t offset, bytes; } state_fields[] = {
    { offsetof(ngp_t, cpu), sizeof(ngp_cpu_t) },
    { offsetof(ngp_t, gfx), sizeof(ngp_gfx_t) },
    { offsetof(ngp_t, trun), offsetof(ngp_t, ram) - offsetof(ngp_t, trun) },
    { offsetof(ngp_t, ram), NGP_RAM_BYTES },
    { offsetof(ngp_t, rtc_latch), offsetof(ngp_t, z80_enabled) - offsetof(ngp_t, rtc_latch) },
    { offsetof(ngp_t, z80_enabled), offsetof(ngp_t, z80_runtime) - offsetof(ngp_t, z80_enabled) },
    { offsetof(ngp_t, z80_runtime), offsetof(ngp_t, sq_period) - offsetof(ngp_t, z80_runtime) },
    { offsetof(ngp_t, sq_period), offsetof(ngp_t, flash_shadow) - offsetof(ngp_t, sq_period) },
    { offsetof(ngp_t, flash_dirty), offsetof(ngp_t, last_pc) - offsetof(ngp_t, flash_dirty) },
};

uint32_t ngp_state_bytes(void) {
    uint32_t bytes = 4, i;
    for (i = 0; i < sizeof state_fields / sizeof *state_fields; i++) {
        /* a span that crossed a pointer or ran off the struct: refuse
         * rather than memcpy garbage over the machine */
        if (state_fields[i].offset + state_fields[i].bytes > sizeof(ngp_t)) return 0;
        bytes += (uint32_t)state_fields[i].bytes;
    }
    return bytes;
}

void ngp_state_io(void *data, uint32_t bytes, int load) {
    uint32_t i;
    uint8_t *p = (uint8_t *)data;
    uint32_t total;
    (void)bytes;
    /* the version tag */
    if (load) {
        uint32_t tag;
        memcpy(&tag, p, 4); p += 4;
        if (tag != 0x4E475031u) return; /* "NGP1": an old state, ignore */
    } else {
        const uint32_t tag = 0x4E475031u;
        memcpy(p, &tag, 4); p += 4;
    }
    for (i = 0; i < sizeof state_fields / sizeof *state_fields; i++) {
        total = (uint32_t)state_fields[i].bytes;
        if (load) memcpy((uint8_t *)&machine + state_fields[i].offset, p, total);
        else memcpy(p, (uint8_t *)&machine + state_fields[i].offset, total);
        p += total;
    }
}
