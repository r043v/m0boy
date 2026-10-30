#include "snes.h"

#include <string.h>
#include <stddef.h>

void snes_ppu_write(uint16_t at, uint8_t v);
uint8_t snes_ppu_read(uint16_t at);
void snes_dma_start(uint8_t mask);
void snes_hdma_line(void);
void snes_smp_port_write(unsigned n, uint8_t v);

snes_t machine; /* one machine per firmware, as the other runtimes' own */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the NES
 * and Sega runtimes).  snes_reset references it: --gc-sections would drop
 * it otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    snes_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.cpu.mdr, 0 };

uint64_t snes_ctx_key(const snes_cpu_t *cpu) {
    /* the emitter's layout: K, DBR, the direct page, then M (bit 0),
     * X (bit 1), E (bit 2) */
    return (uint64_t)cpu->k | (uint64_t)cpu->dbr << 8 | (uint64_t)(cpu->d & 0xFF) << 16 |
           (uint64_t)(cpu->d >> 8) << 24 |
           (uint64_t)(((cpu->p & SNES_P_M ? 1u : 0u) | (cpu->p & SNES_P_X ? 2u : 0u) |
                       (cpu->e ? 4u : 0u))) << 32;
}

/* the cartridge's windows: LoROM banks repeat every $400000, HiROM every
 * $400000 as well, the mirrors of $40-$7D and $C0-$FF only in HiROM */
static uint32_t rom_offset(const snes_cpu_t *c, uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (c->hirom) return (uint32_t)(bank & 0x3F) * 0x10000u + off;
    return (uint32_t)(bank & 0x3F) * 0x8000u + (off & 0x7FFFu);
}

/* I/O page: PPU ports, DMA, the APU's comm ports, NMI and WRAM port.
 * Unimplemented devices return open bus. */
static uint8_t io_read(uint16_t at) {
    snes_cpu_t *c = &machine.cpu;
    if (at >= 0x2100 && at <= 0x213f) return snes_ppu_read(at);
    if (at >= 0x4300 && at <= 0x437f) return machine.dma[(at >> 4) & 7][at & 15];
    if (at >= 0x2140 && at <= 0x217f) return machine.smp.ports_out[at & 3];
    switch (at) {
    case 0x4016: {
        unsigned bit;
        if (machine.joy_strobe) bit = (machine.joypad >> 15) & 1;
        else if (machine.joy_count >= 16) bit = 1;
        else {
            bit = (machine.joy_shift >> 15) & 1;
            machine.joy_shift <<= 1;
            machine.joy_count++;
        }
        return (c->mdr & 0xfc) | bit;
    }
    case 0x4017: return c->mdr & 0xfc; /* second port disconnected */
    case 0x4210: { /* RDNMI: the VBlank flag (cleared by the read), CPU 2 */
        const uint8_t v = (uint8_t)((c->vblank_flag ? 0x80 : 0) | 0x02);
        c->vblank_flag = 0;
        return v;
    }
    case 0x4211: { /* TIMEUP: the H/V timer's flag, cleared by the read */
        const uint8_t v = machine.timeup ? 0x80 : 0;
        machine.timeup = 0;
        c->irq_line = 0;
        return v;
    }
    case 0x4212: return (machine.vblank ? 0x80 : 0) | (machine.hblank ? 0x40 : 0) | machine.joy_busy;
    case 0x4214: return (uint8_t)machine.div_quot; /* the division's quotient */
    case 0x4215: return (uint8_t)(machine.div_quot >> 8);
    case 0x4216: return machine.mpyed ? (uint8_t)machine.mpy_product : machine.div_rem;
    case 0x4217: return machine.mpyed ? (uint8_t)(machine.mpy_product >> 8) : (uint8_t)(c->mdr & 0x40);
    case 0x2180: { /* the WRAM port */
        const uint8_t v = machine.wram[c->wram_addr & 0x1FFFF];
        c->wram_addr = (c->wram_addr + 1) & 0x1FFFF;
        return v;
    }
    case 0x2181: return (uint8_t)c->wram_addr;
    case 0x2182: return (uint8_t)(c->wram_addr >> 8);
    case 0x2183: return (uint8_t)(c->wram_addr >> 16);
    case 0x4218: return (uint8_t)machine.joy_auto;
    case 0x4219: return (uint8_t)(machine.joy_auto >> 8);
    case 0x421A: case 0x421B: case 0x421C: case 0x421D: case 0x421E: case 0x421F:
        return 0; /* disconnected ports */
    default: return c->mdr; /* open bus */
    }
}

static void io_write(uint16_t at, uint8_t v) {
    snes_cpu_t *c = &machine.cpu;
    if (at >= 0x2100 && at <= 0x213f) { snes_ppu_write(at, v); return; }
    if (at >= 0x4300 && at <= 0x437f) { machine.dma[(at >> 4) & 7][at & 15] = v; return; }
    if (at >= 0x2140 && at <= 0x217f) { snes_smp_port_write(at & 3, v); return; }
    switch (at) {
    case 0x4016:
        if ((v & 1) || machine.joy_strobe) {
            machine.joy_shift = machine.joypad;
            machine.joy_count = 0;
        }
        machine.joy_strobe = v & 1;
        break;
    case 0x420b: snes_dma_start(v); break;
    case 0x420c: machine.hdma_enable = v; break;
    case 0x4202: machine.mpy_a = v; break;
    case 0x4203: /* signed 8x8 -> 16, the product latched by the B write */
        machine.mpy_product = (uint16_t)((int16_t)(int8_t)machine.mpy_a * (int8_t)v);
        machine.mpyed = 1;
        break;
    case 0x4204: machine.div_l = v; break;
    case 0x4205: machine.div_h = v; break;
    case 0x4206: { /* unsigned 16/8 -> quotient + remainder 8; /0 = $FFFF/$FF */
        const uint16_t n = (uint16_t)(machine.div_l | machine.div_h << 8);
        machine.div_quot = v ? (uint16_t)(n / v) : 0xFFFFu;
        machine.div_rem = v ? (uint8_t)(n % v) : 0xFFu;
        machine.mpyed = 0;
        break;
    }
    case 0x4207: case 0x4208:
        machine.htime = (uint16_t)(((unsigned)v << 8 | machine.timer_latch) & 0x1FF);
        machine.timer_latch = v;
        break;
    case 0x4209: case 0x420a:
        machine.vtime = (uint16_t)(((unsigned)v << 8 | machine.timer_latch) & 0x1FF);
        machine.timer_latch = v;
        break;
    case 0x4200: /* NMI, the H/V timer IRQ enables and the auto-read */
        if ((v & 0x80) && !c->nmi_enable && c->vblank_flag) c->nmi_pending = 1;
        c->nmi_enable = (v & 0x80) != 0;
        machine.h_irq_en = (v & 0x10) != 0;
        machine.v_irq_en = (v & 0x20) != 0;
        machine.joy_auto_enable = v & 1;
        break;
    case 0x2180:
        machine.wram[c->wram_addr & 0x1FFFF] = v;
        c->wram_addr = (c->wram_addr + 1) & 0x1FFFF;
        break;
    case 0x2181: c->wram_addr = (c->wram_addr & 0x1FF00) | v; break;
    case 0x2182: c->wram_addr = (c->wram_addr & 0x100FF) | (uint32_t)v << 8; break;
    case 0x2183: c->wram_addr = (c->wram_addr & 0x0FFFF) | (uint32_t)(v & 1) << 16; break;
    default: break; /* unimplemented device */
    }
}

/* the SRAM window $6000-$7FFF: LoROM banks $00-$3F/$80-$BF, HiROM banks
 * $20-$3F/$A0-$BF ($20-$3F of the masked bank); the window mirrors the
 * cartridge's declared size */
static int sram_window(snes_cpu_t *c, uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (off < 0x6000 || off >= 0x8000) return 0;
    if (bank <= 0x3F) return !c->hirom || (bank & 0x3F) >= 0x20;
    return bank >= 0x80 && bank <= 0xBF && !c->hirom;
}

uint8_t snes_bus_read(uint32_t at) {
    snes_cpu_t *c = &machine.cpu;
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    uint8_t v;
    if (bank == 0x7E || bank == 0x7F) { /* the whole 128 KiB */
        v = machine.wram[at & 0x1FFFF];
    } else if (off < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        v = machine.wram[at & 0x1FFF]; /* the WRAM mirror */
    } else if (off >= 0x2000 && off < 0x6000 &&
               (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        v = io_read(off);
    } else if (machine.sram_bytes && sram_window(c, at)) {
        v = machine.sram[(off - 0x6000u) & (machine.sram_bytes - 1)];
    } else if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0) {
        v = c->hirom ? c->rom[rom_offset(c, at) % (c->rom_bytes ? c->rom_bytes : 1)] : c->mdr;
    } else { /* banks $00-$3F and $80-$BF from $8000: the cartridge */
        const uint32_t fo = rom_offset(c, at);
        v = fo < c->rom_bytes ? c->rom[fo] : c->mdr;
    }
    c->mdr = v;
    return v;
}

void snes_bus_write(uint32_t at, uint8_t v) {
    snes_cpu_t *c = &machine.cpu;
    c->mdr = v;
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) {
        machine.wram[at & 0x1FFFF] = v;
    } else if (off < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        machine.wram[at & 0x1FFF] = v;
    } else if (off >= 0x2000 && off < 0x6000 &&
               (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        io_write(off, v);
    } else if (machine.sram_bytes && sram_window(c, at)) {
        machine.sram[(off - 0x6000u) & (machine.sram_bytes - 1)] = v;
        machine.sram_dirty = 1;
    }
    /* cartridge writes: nothing (the games' registers live in the I/O page) */
}

void snes_init(const uint8_t *rom, uint32_t rom_bytes, int hirom, uint32_t sram_bytes) {
    snes_cpu_t *c = &machine.cpu;
    memset(&machine, 0, sizeof machine);
    c->wram = machine.wram;
    c->rom = rom;
    c->rom_bytes = rom_bytes;
    c->hirom = hirom;
    machine.sram_bytes = sram_bytes;
    snes_reset();
}

void snes_set_joypad(uint16_t buttons) { machine.joypad = buttons & 0xfff0u; }

void snes_reset(void) {
    snes_cpu_t *c = &machine.cpu;
    machine.frames = 0;
    __asm__ volatile("" : : "r"(&snes_input_probe));
    c->a = c->x = c->y = 0;
    c->s = 0x01FF;
    c->d = 0;
    c->k = 0;
    c->dbr = 0;
    c->p = SNES_P_I | SNES_P_M | SNES_P_X;
    c->e = 1;
    c->cycles = 0;
    c->mdr = 0;
    c->nmi_enable = 0;
    c->vblank_flag = 0;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->halted = 0;
    c->waiting = 0;
    c->wram_addr = 0;
    machine.joypad = machine.joy_auto = machine.joy_shift = 0;
    machine.joy_strobe = machine.joy_count = machine.joy_auto_enable = machine.joy_busy = 0;
    /* the S-SMP waits for the boot handshake, the DSP silent; the IPL's
     * ready signature answers $AA/$BB on ports 0/1 */
    memset(&machine.smp, 0, offsetof(snes_smp_t, ram) + sizeof machine.smp.ram);
    machine.smp.sp = 0xEF;
    machine.smp.psw = 0x02;
    machine.smp.ports_out[0] = 0xAA;
    machine.smp.ports_out[1] = 0xBB;
    /* the reset vector, always of bank $00 in emulation mode */
    c->pc = (uint16_t)(snes_bus_read(0x00FFFC) | snes_bus_read(0x00FFFD) << 8);
}

/* the interrupt sequence shared by the dispatcher and the interpreter:
 * the B bit on the stack is clear (BRK/COP/PHP push it set), D is
 * cleared, S stays in page $01 in the emulation mode */
void snes_take_interrupt(snes_cpu_t *c, uint16_t vector) {
    if (!c->e) {
        snes_bus_write(c->s, c->k);
        c->s--;
    }
    snes_bus_write(0x000000u | c->s, (uint8_t)(c->pc >> 8));
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    snes_bus_write(0x000000u | c->s, (uint8_t)c->pc);
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    snes_bus_write(0x000000u | c->s, (uint8_t)(c->e ? (c->p | 0x20) & ~SNES_P_X : c->p));
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    c->p |= SNES_P_I;
    c->p &= (uint8_t)~SNES_P_D;
    c->k = 0;
    if (c->e) c->s = (uint16_t)(0x0100u | (c->s & 0xFF));
    c->pc = (uint16_t)(snes_bus_read(0x000000u | vector) |
                       snes_bus_read(0x000000u | (uint32_t)(vector + 1)) << 8);
}

/* the H/V timer's IRQ: TIMEUP rises at the match and stays until the
 * $4211 read; the level is what the CPU samples */
static void irq_fire(void) {
    machine.timeup = 1;
    machine.cpu.irq_line = 1;
}

/* Line-level approximation: the line's pixels go out with the registers as
 * the previous lines' code left them (a write during a line's HBlank
 * reaches the next line), the CPU runs the visible part, the HBlank flag
 * rises and the rest of the line runs.  VBlank and the NMI enter at line
 * 225; the auto-read latches there and its time is approximated at three
 * lines.  The H/V timers compare at the line's start (a V match without
 * H) and at one of the line's two slice boundaries for the H match: the
 * visible part is 3/4 of the line, HTIME's exact dot is not modelled. */
void snes_frame(void) {
    const uint32_t start = machine.cpu.cycles;
    uint32_t at_line = start;
    machine.frames++;
    machine.vblank = 0;
    machine.hblank = 0;
    machine.cpu.vblank_flag = 0;
    memset(machine.hdma_run, 0, sizeof machine.hdma_run);
    memset(machine.hdma_done, 0, sizeof machine.hdma_done);
    for (unsigned line = 0; line < 262; line++) {
        if (line == 225) {
            machine.vblank = 1;
            machine.cpu.vblank_flag = 1;
            if (machine.cpu.nmi_enable) machine.cpu.nmi_pending = 1;
            if (machine.joy_auto_enable && !machine.joy_strobe) {
                machine.joy_auto = machine.joypad;
                machine.joy_shift = 0;
                machine.joy_count = 16;
                machine.joy_busy = 1;
            }
        }
        if (line == 228) machine.joy_busy = 0;
        const int v_match = line == machine.vtime;
        machine.cpu.irq_line = machine.timeup; /* the level persists */
        if (machine.v_irq_en && v_match && !machine.h_irq_en) irq_fire();
        if (machine.h_irq_en && (!machine.v_irq_en || v_match) && machine.htime < 256) irq_fire();
        if (line < 224) snes_render_line(line, machine.screen + line * 256u * 3u);
        const uint32_t until = start + (uint32_t)(((uint64_t)(line + 1) * SNES_FRAME_CYCLES) / 262);
        /* three quarters of the line's cycles before the HBlank flag, as
         * the visible part's share of the 1364 dots, rounded */
        const uint32_t visible = start +
            (uint32_t)(((uint64_t)line * SNES_FRAME_CYCLES) / 262 +
                       ((((uint64_t)(line + 1) * SNES_FRAME_CYCLES) / 262 -
                         ((uint64_t)line * SNES_FRAME_CYCLES) / 262) * 3) / 4);
        snes_run(&machine.cpu, visible);
        const uint32_t at_visible = machine.cpu.cycles;
        machine.hblank = 1;
        snes_hdma_line();
        if (machine.h_irq_en && (!machine.v_irq_en || v_match) && machine.htime >= 256) irq_fire();
        snes_run(&machine.cpu, until);
        /* the S-SMP and the S-DSP follow the CPU's clock through both
         * slices of the line */
        snes_apu_run(at_visible - at_line);
        snes_apu_run(machine.cpu.cycles - at_visible);
        at_line = machine.cpu.cycles;
    }
    machine.hblank = 0;
}
