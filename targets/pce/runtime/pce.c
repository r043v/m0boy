#include "pce.h"

#include <string.h>

pce_t machine; /* the CPU's bus lands here (one machine per firmware, as
                * the other runtimes' own machine) */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the NES
 * and Sega runtimes), so a session played in gm0 replays on the host
 * harness (--input).  pce_reset references it: --gc-sections would drop
 * it otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    pce_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.input, 0 };

/* the VCE: the 512-color table (B on bits 0-2, R on 3-5, G on 6-8) and
 * its RGB565 image.  A pixel whose color nibble is 0 shows the table's
 * entry 0 (the backdrop) — the substitution happens when the pixel is
 * looked up (the row callback), not here: a game may rewrite any entry,
 * entry 0 included, after the palette is set up. */
static void vce_fix_lut(unsigned entry) {
    const uint16_t c = machine.vce_colors[entry & 0x1FF];
    const unsigned b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    machine.vce_lut[entry & 0x1FF] =
        (uint16_t)((((r * 36) >> 3) << 11) | (((g * 36) >> 2) << 5) | ((b * 36) >> 3));
}

static uint8_t vce_read(unsigned at) {
    switch (at & 7) {
    case 0: return machine.vce_cr;
    case 2: return (uint8_t)machine.vce_address;
    case 3: return (uint8_t)(machine.vce_address >> 8);
    case 4: return (uint8_t)machine.vce_colors[machine.vce_address & 0x1FF];
    case 5: return (uint8_t)(machine.vce_colors[machine.vce_address & 0x1FF] >> 8);
    default: return machine.cpu.io_latch;
    }
}

static void vce_write(unsigned at, uint8_t v) {
    switch (at & 7) {
    case 0: machine.vce_cr = v; break;
    case 2: machine.vce_address = (machine.vce_address & 0x100) | v; break;
    case 3: machine.vce_address = (machine.vce_address & 0x0FF) | ((v & 1) << 8); break;
    case 4:
        machine.vce_colors[machine.vce_address & 0x1FF] &= 0x100;
        machine.vce_colors[machine.vce_address & 0x1FF] |= v;
        vce_fix_lut(machine.vce_address);
        break;
    case 5:
        machine.vce_colors[machine.vce_address & 0x1FF] &= 0x0FF;
        machine.vce_colors[machine.vce_address & 0x1FF] |= (uint16_t)((v & 1) << 8);
        vce_fix_lut(machine.vce_address);
        machine.vce_address = (machine.vce_address + 1) & 0x1FF;
        break;
    default: break;
    }
}

/* the pad: the port's nibble inverted (a pressed button reads 0), bit 4-5
 * always set, bit 6 clear (a PC Engine, not a TG-16), bit 7 set (no CD
 * attached — Raiden's boot probe keys on it) */
static uint8_t pad_read(void) {
    unsigned nibble;
    if (machine.pad_sel)
        nibble = (machine.input >> 4) & 0x0F; /* Up Right Down Left */
    else
        nibble = machine.input & 0x0F;        /* I II Select Run */
    return (uint8_t)((0x0F ^ nibble) | 0x30 | 0x80);
}

/* the timer: a tick every 1024 CPU cycles, the underflow latches the IRQ
 * until the controller's ack, the reload takes the tick after */
static void timer_tick(void) {
    if (machine.timer_reload) {
        machine.timer_reload = 0;
        machine.timer_value = machine.timer_load;
        return;
    }
    if (!machine.timer_enable) return;
    if (machine.timer_value == 0) {
        machine.timer_value = -1;
        machine.timer_irq = 1;
        machine.timer_reload = 1;
    } else {
        machine.timer_value--;
    }
}

static void update_irq(void) {
    machine.cpu.irq_low = 0;
    if (machine.timer_irq) machine.cpu.irq_low |= PCE_IQ_TIMER;
    if (machine.vdc.status & 0x3F) machine.cpu.irq_low |= PCE_IQ_IRQ1;
}

uint8_t pce_bus_read(uint16_t at) {
    switch (at & 0x1C00) {
    case 0x0000: { /* the VDC */
        const uint8_t ret = vdc_read(&machine.vdc, at & 3);
        update_irq(); /* the status read dropped the line's sources */
        return ret;
    }
    case 0x0400: return vce_read(at & 7);
    case 0x0800: /* the PSG's registers read open bus */
        return machine.cpu.io_latch;
    case 0x0C00: { /* the timer, both offsets */
        const uint8_t ret = (uint8_t)(machine.timer_value | (machine.cpu.io_latch & 0x80));
        machine.cpu.io_latch = ret;
        return ret;
    }
    case 0x1000: { /* the pad */
        const uint8_t ret = pad_read();
        machine.cpu.io_latch = ret;
        return ret;
    }
    case 0x1400: /* the interrupt controller */
        if (!(at & 2)) return machine.cpu.io_latch; /* $1400/$1401: open bus */
        if (!(at & 1)) { /* $1402: the mask, and the timer's ack */
            machine.timer_irq = 0;
            update_irq();
            return (uint8_t)(~machine.cpu.irq_mask & 7);
        }
        { /* $1403: the live status */
            unsigned status = machine.cpu.io_latch & ~7u;
            if (machine.cpu.irq_low & PCE_IQ_IRQ2) status |= 1;
            if (machine.cpu.irq_low & PCE_IQ_IRQ1) status |= 2;
            if (machine.cpu.irq_low & PCE_IQ_TIMER) status |= 4;
            return (uint8_t)status;
        }
    default: /* the CD and the expansion are not here: open bus */
        return 0xFF;
    }
}

void pce_bus_write(uint16_t at, uint8_t v) {
    machine.cpu.io_latch = v;
    switch (at & 0x1C00) {
    case 0x0000: /* the VDC */
        vdc_write(&machine.vdc, at & 3, v);
        update_irq();
        break;
    case 0x0400: vce_write(at & 7, v); break;
    case 0x0800: psg_write(&machine.psg, at & 0x0F, v); break;
    case 0x0C00: /* the timer */
        if (!(at & 1)) machine.timer_load = v & 0x7F;
        else {
            if ((v & 1) && !machine.timer_enable) {
                machine.timer_value = machine.timer_load;
                machine.timer_clock = 0;
            }
            machine.timer_enable = v & 1;
        }
        break;
    case 0x1000: /* the pad's SEL and CLR */
        machine.pad_sel = v & 1;
        break;
    case 0x1400: /* the interrupt controller */
        if (!(at & 2)) break;
        if (!(at & 1)) machine.cpu.irq_mask = (uint8_t)(~v & 7);
        else machine.timer_irq = 0; /* $1403: the timer's ack */
        update_irq();
        break;
    default: break;
    }
}

void pce_init(const uint8_t *rom, uint32_t rom_bytes) {
    pce_t *p = &machine;
    for (unsigned i = 0; i < sizeof p->wram; i++) p->wram[i] = 0;
    p->cpu.wram = p->wram;
    p->cpu.rom = rom;
    p->cpu.rom_bytes = rom_bytes;
    memset(p->vce_colors, 0, sizeof p->vce_colors);
    for (unsigned i = 0; i < 512; i++) vce_fix_lut(i);
    p->vce_cr = 0;
    p->vce_address = 0;
    p->row = 0;
    p->row_raw = 0;
    p->host = 0;
    p->input = 0;
    p->pad_sel = 0;
    p->have_frame = 0;
    p->first_active_line = 0;
    pce_reset();
}

/* the CPU-budget carry across frames and resets */
static unsigned master_carry;

void pce_reset(void) {
    pce_t *p = &machine;
    master_carry = 0;
    __asm__ volatile("" : : "r"(&pce_input_probe));
    huc6280_reset(&p->cpu);
    vdc_reset(&p->vdc);
    psg_reset(&p->psg);
    p->timer_load = 0;
    p->timer_value = 0;
    p->timer_enable = 0;
    p->timer_reload = 0;
    p->timer_irq = 0;
    p->timer_clock = 0;
    p->pad_sel = 0;
    p->frames = 0;
    p->have_frame = 0;
    update_irq();
}


void pce_frame(void) {
    pce_t *p = &machine;
    p->frames++;
    p->have_frame = 0;
    for (unsigned line = 0; line < 262; line++) {
        /* 1365 master clocks per line: 455 CPU cycles at CSH (7.16 MHz),
         * 1365/12 ≈ 114 at CSL (1.79 MHz) — the game boots in CSL and the
         * switch lands at a line boundary here */
        master_carry += 1365;
        const unsigned cycles = master_carry / (p->cpu.speed ? 3 : 12);
        master_carry %= p->cpu.speed ? 3u : 12u;
        huc6280_run(&p->cpu, p->cpu.cycles + cycles);
        vdc_line(&p->vdc, line);
        /* the timer runs on the CPU clock: a tick every 1024 of the same
         * cycles the CPU just consumed (the sweep's jitter stays under
         * one line) */
        p->timer_clock += cycles;
        while (p->timer_clock >= 1024) {
            p->timer_clock -= 1024;
            timer_tick();
        }
        update_irq();
        if (p->vdc.active_started && !p->have_frame) {
            p->first_active_line = line;
            p->have_frame = 1;
        }
        if (p->row) {
            const unsigned width = p->vdc.row_width < 1024 ? p->vdc.row_width : 1024;
            if (p->row_raw) {
                p->row(p->host, line, p->vdc.linebuf, width);
            } else {
                for (unsigned x = 0; x < width; x++) {
                    /* the bg's color 0 is transparent: the backdrop's entry, as
                     * the mednafen core picks it ((color & 0xF) ? pixel : 0) */
                    const uint16_t px = p->vdc.linebuf[x];
                    p->rowbuf[x] = p->vce_lut[(px & 0xF) ? px & 0x1FF : 0];
                }
                p->row(p->host, line, p->rowbuf, width);
            }
        }
    }
}

void pce_audio(uint8_t *dst, unsigned n, unsigned rate) {
    psg_render(&machine.psg, dst, n, rate);
}

/* the consoles' audio drivers (meta_audio.cpp and its like) call these
 * two without knowing the machine */
void pce_audio_set_rate(unsigned rate) { (void)rate; }
void pce_audio_render(uint8_t *buffer, unsigned n) {
    psg_render(&machine.psg, buffer, n, 11025u);
}
