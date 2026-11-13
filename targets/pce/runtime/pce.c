#include <stdio.h>
#ifndef M0_TL
#define M0_TL   /* deepeel builds with -DM0_TL=__thread: one machine per worker thread */
#endif
#include "pce.h"

#include <string.h>

typedef uint32_t __attribute__((may_alias)) u32a_;

M0_TL pce_t machine; /* the CPU's bus lands here (one machine per firmware, as
                * the other runtimes' own machine) */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the NES
 * and Sega runtimes), so a session played in gm0 replays on the host
 * harness (--input).  pce_reset references it: --gc-sections would drop
 * it otherwise. */
#ifndef M0_NO_INPUT_PROBE
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    pce_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.input, 0 };
#endif

/* the VCE: the 512-color table (B on bits 0-2, R on 3-5, G on 6-8) and
 * its RGB565 image.  A pixel whose color nibble is 0 shows the table's
 * entry 0 (the backdrop); instead of a test per pixel, the write rebuilds
 * vce_final, the table the pixels actually read — entry e keeps its own
 * color, the 32 color-0 entries carry entry 0's, like the mednafen core's
 * FixPCache (rebuilt at every palette write, so a game may rewrite any
 * entry, entry 0 included, after the palette is set up). */
static void vce_fix_lut(unsigned entry) {
    const unsigned e = entry & 0x1FF;
    const uint16_t c = machine.vce_colors[e];
    const unsigned b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    machine.vce_lut[e] =
        (uint16_t)((((r * 36) >> 3) << 11) | (((g * 36) >> 2) << 5) | ((b * 36) >> 3));
    if (e == 0) {
        for (unsigned n = 0; n < 512; n += 16) machine.vce_final[n] = machine.vce_lut[0];
    } else if ((e & 0xF) == 0) {
        machine.vce_final[e] = machine.vce_lut[0];
    } else {
        machine.vce_final[e] = machine.vce_lut[e];
    }
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
    if (machine.pad_idx > 4) return 0x30 | 0x80;                     /* past the tap's five ports: all bits low */
    if (machine.pad_idx > 0) return (uint8_t)(0x0F | 0x30 | 0x80);   /* ports 2..5: nothing plugged */
    if (machine.pad_sel)
        nibble = (machine.input >> 4) & 0x0F; /* Up Right Down Left */
    else
        nibble = machine.input & 0x0F;        /* I II Select Run */
    return (uint8_t)((0x0F ^ nibble) | 0x30 | 0x80);
}

/* the timer: a tick every 1024 CPU cycles, the underflow latches the IRQ
 * until the controller's ack, the reload takes the tick after */
unsigned pce_timer_div = 65536;   /* 1/64 of a 7.16 MHz cycle per timer tick: 1024 cycles (calibration) */
unsigned pce_timer_clk;   /* debug: prescaler phase at timer enable */
unsigned pce_timer_phase;   /* debug: ticks added to the first timer period */
unsigned pce_timer_extra = 1;   /* 1: the reload costs a tick of its own (period load + 2) */
static void timer_tick(void) {
    if (machine.timer_reload) {
        machine.timer_reload = 0;
        machine.timer_value = machine.timer_load;
        return;
    }
    if (!machine.timer_enable) return;
    if (machine.timer_value == 0) {
        machine.timer_irq = 1;
        if (pce_timer_extra) { machine.timer_value = -1; machine.timer_reload = 1; }
        else machine.timer_value = machine.timer_load;   /* period = load + 1 ticks */
    } else {
        machine.timer_value--;
    }
}

static void update_irq(void) {
    machine.cpu.irq_low = 0;
    if (machine.timer_irq) machine.cpu.irq_low |= PCE_IQ_TIMER;
    if (machine.vdc.status & 0x3F) machine.cpu.irq_low |= PCE_IQ_IRQ1;
}

/* HuC6270 bus wait states.  The reference core parks a VRAM access behind a running SATB DMA (1024 VDC clocks, a
 * clock being 4/3/2/2 master clocks by the dot clock) and the next VDC data access waits for the DMA's end */
int pce_trace;
unsigned pce_stat_waits;
unsigned pce_vdc_waits = 1;   /* 0: no VRAM wait states behind SATB DMA / the commit window (PCEHawk does not model them) */
extern unsigned vdc_master_per_cycle;
static uint64_t now_master(void) {
    return machine.line_master + (uint64_t)(machine.cpu.cycles - machine.line_cycles) * (machine.cpu.speed ? 3u : 12u);
}
static void vdc_wait(void) {
    if (!machine.vdc_pending) return;
    machine.vdc_pending = 0;
    const uint64_t now = now_master();
    if (now >= machine.sat_end) return;
    const unsigned unit = machine.cpu.speed ? 3u : 12u;
    if (pce_trace) fprintf(stderr, "WAIT now=%llu end=%llu\n", (unsigned long long)now, (unsigned long long)machine.sat_end);
    pce_stat_waits++; machine.cpu.steal += (uint32_t)((machine.sat_end - now + unit - 1) / unit);
}
unsigned pce_mys_off = 1, pce_mys_w = 0;   /* 1: the mystery window is derived from the HuC6270 (16 dot clocks busy, starting 48 dot clocks after the hsync begins at x=1128); else [mys_off-1, +mys_w] masters, fitted */
unsigned pce_sat_waits = 1;   /* 0: the SATB DMA window holds no access (the reference's WS:sat never fired on Tonma's boot) */
static void vdc_park(void) {
    if (!pce_vdc_waits) return;
    const uint64_t now = now_master();
    if (pce_sat_waits && now < machine.sat_end) machine.vdc_pending = 1;
    else if (pce_mys_off) {   /* the HuC6270 holds a VRAM commit written during the mystery phase: 16 dot clocks busy after 48, from the hsync start at x=1128; commits when the phase exits */
        const uint64_t r = vdc_master_per_cycle;
        const uint64_t ws = pce_mys_w ? machine.line_master + (pce_mys_off - 1) : machine.line_master + 1128 + 48 * r;
        const uint64_t we = ws + (pce_mys_w ? pce_mys_w : 16 * r);
        if (now >= ws && now < we) { machine.sat_end = we; machine.vdc_pending = 1; }
    }
}

unsigned pce_evt_master;   /* debug: master clocks into the line where the VDC's line events land (0 = the line's end) */
unsigned pce_cur_line;   /* trace only: the VDC line the CPU slice belongs to */
uint8_t pce_bus_read(uint16_t at) {
    if (pce_trace && !(at & 0x0C00)) fprintf(stderr, "R %04x pc=%04x ln=%u t=%llu\n", at & 0x1FFF, machine.cpu.pc, pce_cur_line, (unsigned long long)now_master());
    if (!(at & 0x1800)) machine.cpu.steal++;   /* only VDC ($0000) and VCE ($0400) accesses cost an extra cycle (Mednafen StealCycle); the pad port $1000 and the IRQ registers $1400 do not */
    switch (at & 0x1C00) {
    case 0x0000: { /* the VDC */
        if ((at & 3) >= 2) vdc_wait();
        const uint8_t ret = vdc_read(&machine.vdc, at & 3);
        update_irq(); /* the status read dropped the line's sources */
        if (pce_trace) fprintf(stderr, "RV %u=%02x pc=%04x\n", at & 3, ret, machine.cpu.pc);
        return ret;
    }
    case 0x0400: return vce_read(at & 7);
    case 0x0800: /* the PSG's registers read open bus */
        return machine.cpu.io_latch;
    case 0x0C00: { /* the timer, both offsets */
        const uint8_t ret = (uint8_t)(machine.timer_value | (machine.cpu.io_latch & 0x80));
        machine.cpu.io_latch = ret;
        if (pce_trace) fprintf(stderr, "RT %02x pc=%04x\n", ret, machine.cpu.pc);
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
    if (pce_trace && !(at & 0x0C00)) fprintf(stderr, "W %04x=%02x ln=%u t=%llu\n", at & 0x1FFF, v, pce_cur_line, (unsigned long long)now_master());
    if (!(at & 0x1800)) machine.cpu.steal++;   /* VDC / VCE only (Mednafen) */
    switch (at & 0x1C00) {
    case 0x0000: /* the VDC */
        if (((at & 3) == 3 && (machine.vdc.select == 1 || machine.vdc.select == 2)) ||
            ((at & 3) == 2 && machine.vdc.select >= 0x12 && machine.vdc.select != 0x13)) {
            vdc_wait();
            vdc_write(&machine.vdc, at & 3, v);
            vdc_park();
            update_irq();
            break;
        }
        vdc_write(&machine.vdc, at & 3, v);
        if (machine.vdc.dma_master) {
            const uint64_t e = now_master() + machine.vdc.dma_master;
            if (e > machine.sat_end) machine.sat_end = e;
            machine.vdc.dma_master = 0;
        }
        update_irq();
        break;
    case 0x0400: vce_write(at & 7, v); break;
    case 0x0800: psg_write(&machine.psg, at & 0x0F, v); break;
    case 0x0C00: /* the timer */
        if (pce_trace) fprintf(stderr, "TW %d=%02x t=%u clk=%u val=%d\n", at & 1, v, machine.cpu.cycles, machine.timer_clock, machine.timer_value);
        if (!(at & 1)) machine.timer_load = v & 0x7F;
        else {
            if ((v & 1) && !machine.timer_enable) {
                machine.timer_value = machine.timer_load + pce_timer_phase;
                machine.timer_clock = pce_timer_clk * 64u;
            }
            machine.timer_enable = v & 1;
        }
        break;
    case 0x1000: /* the pad's SEL and CLR */
        { const unsigned sel = v & 1, clr = (v >> 1) & 1;
          if (sel && !machine.pad_clr && clr) machine.pad_idx = 0;                          /* CLR 0->1 with SEL high: back to port 1 */
          else if (!machine.pad_clr && !clr && !machine.pad_sel && sel && machine.pad_idx < 255) machine.pad_idx++;   /* SEL 0->1, CLR low: next port */
          machine.pad_sel = (uint8_t)sel; machine.pad_clr = (uint8_t)clr; }
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
    p->pad_sel = 0; p->pad_clr = 0; p->pad_idx = 0;
    p->have_frame = 0;
    p->first_active_line = 0;
    pce_reset();
}

/* the CPU-budget carry across frames and resets */
static M0_TL unsigned master_carry;

void pce_reset(void) {
    pce_t *p = &machine;
    master_carry = 0;
#ifndef M0_NO_INPUT_PROBE
    __asm__ volatile("" : : "r"(&pce_input_probe));
#endif
    huc6280_reset(&p->cpu);
    vdc_reset(&p->vdc);
    psg_reset(&p->psg);
    p->timer_load = 0;
    p->timer_value = 0;
    p->timer_enable = 0;
    p->timer_reload = 0;
    p->timer_irq = 0;
    p->timer_clock = 0;
    p->pad_sel = 0; p->pad_clr = 0; p->pad_idx = 0;
    p->frames = 0;
    p->have_frame = 0;
    update_irq();
}


/* the frame cut can start on any VDC line (a reference core cuts its frames elsewhere): pce_frame_start_line */
unsigned pce_frame_start_line;
int pce_boot_offset;   /* debug: CPU cycles the very first line starts ahead (+) of / behind (-) the video */
/* debug: snapshot the work RAM this many CPU cycles before the end of the frame's last line (a sub-line frame cut); 0 = off */
unsigned pce_timer_mul = 4;   /* debug: timer clock per slow-mode CPU cycle */
unsigned pce_irq_delay;   /* debug: CPU cycles the VDC's IRQ1 reaches the CPU after the line end */
unsigned pce_cut_cycles; uint8_t pce_cut_wram[0x2000]; int pce_cut_valid;
unsigned pce_scale_permille = 1000;
int pce_sat_offset;   /* master clocks added to the SATB DMA's end (calibration) */
static uint32_t cpu_target; static int cpu_target_init;
void pce_frame(void) {
    pce_t *p = &machine;
    p->frames++;
    p->have_frame = 0;
    /* VCE control bit 2 (lc263): the frame is 263 lines, the VDC idles through the extra one */
    const unsigned total = (p->vce_cr & 4) ? 263u : 262u;
    for (unsigned fl = 0; fl < total; fl++) {
        const unsigned line = (fl + pce_frame_start_line) % total;
        pce_cur_line = line;
        if (!cpu_target_init) { cpu_target = p->cpu.cycles + (uint32_t)pce_boot_offset; cpu_target_init = 1; }
        p->line_cycles = cpu_target;   /* the nominal start of the line: a stall that overshot the last one is inside this one */
        /* 1365 master clocks per line: 455 CPU cycles at CSH (7.16 MHz),
         * 1365/12 ≈ 114 at CSL (1.79 MHz) — the game boots in CSL and the
         * switch lands at a line boundary here */
        master_carry += 1365;
        const unsigned cycles = master_carry / (p->cpu.speed ? 3 : 12);
        master_carry %= p->cpu.speed ? 3u : 12u;
        cpu_target += (cycles * pce_scale_permille) / 1000u;
        if (pce_cut_cycles && fl + 1 == total) {
            huc6280_run(&p->cpu, cpu_target - pce_cut_cycles);
            memcpy(pce_cut_wram, p->wram, sizeof pce_cut_wram); pce_cut_valid = 1;
        }
        if (pce_evt_master) {   /* the VDC's line events land this many master clocks into the line, not at its end */
            huc6280_run(&p->cpu, cpu_target - cycles + pce_evt_master / (p->cpu.speed ? 3u : 12u));
        } else {
            huc6280_run(&p->cpu, cpu_target);
        }
        { extern unsigned vdc_master_per_cycle; static const unsigned rt[4] = { 4, 3, 2, 2 }; vdc_master_per_cycle = rt[p->vce_cr & 3]; }
        const unsigned st_before = p->vdc.status & 0x3F;
        if (line < 262) vdc_line(&p->vdc, line);
        if (pce_evt_master) { update_irq(); huc6280_run(&p->cpu, cpu_target); }
        p->line_master += 1365;
        if (p->vdc.sat_started) {
            static const unsigned ratio[4] = { 4, 3, 2, 2 };
            p->vdc.sat_started = 0;
            p->sat_end = p->line_master + 1024u * ratio[p->vce_cr & 3] + pce_sat_offset;
            p->vdc_pending = 0;
            if (pce_trace) fprintf(stderr, "SAT %llu..%llu\n", (unsigned long long)p->line_master, (unsigned long long)p->sat_end);
        }
        /* the timer runs on the CPU clock: a tick every 1024 of the same
         * cycles the CPU just consumed (the sweep's jitter stays under
         * one line) */
        p->timer_clock += (p->cpu.speed ? cycles : cycles * pce_timer_mul) * 64u;   /* in 1/64 cycle */   /* the timer counts 7.16 MHz cycles in both speed modes */
        while (p->timer_clock >= pce_timer_div) {
            p->timer_clock -= pce_timer_div;
            timer_tick();
        }
        if (pce_irq_delay && !st_before && (p->vdc.status & 0x3F)) huc6280_run(&p->cpu, cpu_target + pce_irq_delay);
        update_irq();
        if (p->vdc.active_started && !p->have_frame) {
            p->first_active_line = line;
            p->have_frame = 1;
        }
        if (p->row && line < 262) {
            const unsigned width = p->vdc.row_width < 1024 ? p->vdc.row_width : 1024;
            if (p->row_raw) {
                p->row(p->host, line, p->vdc.linebuf, width);
            } else {
                /* two pixels a step (the width is a multiple of 8, the
                 * line buffer 8-aligned) through the final VCE table */
                const uint16_t *lut = p->vce_final;
                uint16_t *dst = p->rowbuf;
                for (unsigned x = 0; x < width; x += 2) {
                    const uint32_t two = *(const u32a_ *)(p->vdc.linebuf + x);
                    dst[x] = lut[two & 0x1FF];
                    dst[x + 1] = lut[(two >> 16) & 0x1FF];
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
