/* The SA-1: a second 65816 core (the shared interpreter's second
 * instance — no recompiled regions) with its own bus, talking to the
 * S-CPU through $2200-$23FF.  The register semantics follow the
 * published SA-1 documentation (Anomie's regs.txt, fullsnes) with the
 * bsnes core as reading reference; the implementation is our own C.
 * Not modelled: the DMA (CDMA and character conversion), the BW-RAM's
 * bitmap mode, the H/V timer IRQ matches, the arithmetic carry-in. */
#include <stdint.h>
#include <string.h>
#include "snes.h"

void snes_interp_run(snes_cpu_t *cpu, uint32_t until);

static snes_sa1_t *const s = &machine.sa1;
static uint8_t sa1_reg_read(uint32_t at);
static void sa1_reg_write(uint32_t at, uint8_t v);

/* ---- the SA-1's own bus ---- */

static int sa1_iram_window(uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (!(bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) return 0;
    return (off < 0x0800) || (off >= 0x3000 && off < 0x3800);
}

static unsigned sa1_iram_offset(uint32_t at) {
    const unsigned off = at & 0xFFFF;
    return off < 0x0800 ? off : off - 0x3000;
}

/* the ROM through the Super MMC banks: $00-$1F/$80-$9F:8000-FFFF and
 * $C0-$CF go through bank C, $20-$3F/$A0-$BF/$D0-$DF through D, and so
 * on; a bank's xbmode bit switches the window to the programmed bank */
static uint8_t sa1_rom_read(uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    const unsigned half = off < 0x8000 ? 0 : 1; /* the LoROM half of a bank */
    uint32_t a;
    if (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) {
        /* the mapped banks: $00-$3F:8000-FFFF -> file by half-banks */
        a = (uint32_t)(bank & 0x3F) * 0x8000u + (off & 0x7FFFu);
        const unsigned chunk = (bank & 0x3F) >> 1; /* 0-31 */
        uint8_t mode = 0, nb = 0;
        if (chunk < 16)      { mode = s->cbmode; nb = s->cb; }
        else                 { mode = s->dbmode; nb = s->db; }
        if (mode && half) {
            const uint32_t lo = (uint32_t)(bank & 1) * 0x8000u + (off & 0x7FFFu);
            a = (uint32_t)nb * 0x100000u + lo;
        }
    } else if (bank >= 0xC0) {
        const unsigned chunk = bank - 0xC0; /* 0-63 */
        uint8_t mode = 0, nb = 0;
        if (chunk < 16)      { mode = s->cbmode; nb = s->cb; }
        else if (chunk < 32) { mode = s->dbmode; nb = s->db; }
        else if (chunk < 48) { mode = s->ebmode; nb = s->eb; }
        else                 { mode = s->fbmode; nb = s->fb; }
        if (mode) a = (uint32_t)nb * 0x100000u + (((bank & 0x0F) << 16) | off);
        else a = (uint32_t)(bank - 0xC0) * 0x10000u + off;
    } else {
        return machine.cpu.mdr;
    }
    return a < machine.cpu.rom_bytes ? machine.cpu.rom[a] : machine.cpu.mdr;
}

/* the S-CPU can point its own NMI/IRQ vectors at the SA-1's: the reads
 * of $00:FFEA-B and $00:FFEE-F answer SNV/SIV when the switches stand */
int snes_sa1_vector_override(uint32_t at) {
    const unsigned off = at & 0xFFFF;
    if ((at >> 16) != 0x00 || off < 0xFFE0 || off > 0xFFEF) return 0;
    if (off == 0xFFEA && s->cpu_nvsw) return s->snv & 0xFF;
    if (off == 0xFFEB && s->cpu_nvsw) return s->snv >> 8;
    if (off == 0xFFEE && s->cpu_ivsw) return s->siv & 0xFF;
    if (off == 0xFFEF && s->cpu_ivsw) return s->siv >> 8;
    return -1;
}

uint8_t snes_sa1_bus_read(uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    uint8_t v;
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x2200 && off < 0x2400)
        v = sa1_reg_read(at); /* the SA-1's side of the register file */
    else if (sa1_iram_window(at)) {
        v = s->iram[sa1_iram_offset(at)];
    } else if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x6000 && off < 0x8000) {
        v = s->bwram[((s->cbm & 0x1F) * 0x2000u + (off & 0x1FFFu)) & 0x3FFFFu];
    } else if (bank >= 0x40 && bank <= 0x4F) {
        v = s->bwram[at & 0x3FFFFu]; /* linear */
    } else if (bank >= 0x60 && bank <= 0x6F) {
        v = machine.cpu.mdr; /* the bitmap mode is not modelled */
    } else if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF) || bank >= 0xC0) && (off >= 0x8000 || bank >= 0xC0)) {
        v = sa1_rom_read(at);
    } else {
        v = machine.cpu.mdr;
    }
    machine.cpu.mdr = v;
    return v;
}

void snes_sa1_bus_write(uint32_t at, uint8_t v) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    machine.cpu.mdr = v;
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x2200 && off < 0x2400) {
        sa1_reg_write(at, v);
        return;
    }
    if (sa1_iram_window(at)) {
        const unsigned o = sa1_iram_offset(at);
        if (!(s->ciwp & (1u << (o >> 8)))) s->iram[o] = v;
        return;
    }
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x6000 && off < 0x8000) {
        const unsigned o = ((s->cbm & 0x1F) * 0x2000u + (off & 0x1FFFu)) & 0x3FFFFu;
        if (s->cwen) s->bwram[o] = v;
        return;
    }
    if (bank >= 0x40 && bank <= 0x4F) {
        if (s->cwen) s->bwram[at & 0x3FFFFu] = v;
        return;
    }
    /* ROM and unmapped: ignored */
}

/* ---- the register file: the S-CPU's window is $2200-$23FF too ---- */

int snes_sa1_window(const snes_cpu_t *c, uint32_t at) {
    (void)c;
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (!(bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) return 0;
    return (off >= 0x2200 && off < 0x2400) || (off >= 0x3000 && off < 0x3800) ||
           (off >= 0x6000 && off < 0x8000); /* the BW-RAM's 8 KiB window */
}

static void sa1_message_to_cpu(void) {
    /* SCNT bit 7: the SA-1's interrupt request, a level until cleared */
    machine.sa1_cpu_irq = (s->cpu_irqfl && s->cpu_irqen) ||
                          (s->dma_irqfl && s->dma_irqen && 0); /* no DMA */
}

uint8_t snes_sa1_cpu_read(uint32_t at) {
    const unsigned off = at & 0xFFFF;
    if (off >= 0x3000 && off < 0x3800) return s->iram[off - 0x3000];
    if (off >= 0x6000)
        return s->bwram[((uint32_t)s->sbm * 0x2000u + (off & 0x1FFFu)) & 0x3FFFFu];
    switch (0x2200u | (off & 0x1FF)) {
    case 0x2300: /* SFR: the S-CPU's flag read */
        return (uint8_t)((s->cpu_irqfl << 7) | (s->cpu_ivsw << 6) |
                         (s->cpu_nvsw << 4) | s->cmeg);
    case 0x2302: /* HCR: the counters latch on the read */
        s->hcr = (uint16_t)(machine.cpu.cycles & 0x3FF); /* the line's dot/4 */
        s->vcr = (uint16_t)machine.frames;
        return (uint8_t)s->hcr;
    case 0x2303: return (uint8_t)(s->hcr >> 8);
    case 0x2304: return (uint8_t)s->vcr;
    case 0x2305: return (uint8_t)(s->vcr >> 8);
    case 0x2306: return (uint8_t)s->mr;
    case 0x2307: return (uint8_t)(s->mr >> 8);
    case 0x2308: return (uint8_t)(s->mr >> 16);
    case 0x2309: return (uint8_t)(s->mr >> 24);
    case 0x230A: return (uint8_t)(s->mr >> 32);
    case 0x230B: return (uint8_t)(s->overflow << 7);
    default: return machine.cpu.mdr; /* open bus */
    }
}

void snes_sa1_cpu_write(uint32_t at, uint8_t v) {
    const unsigned off = at & 0xFFFF;
    if (off >= 0x3000 && off < 0x3800) { /* the I-RAM, page-protected by SIWP */
        const unsigned o = off - 0x3000;
        if (!(s->siwp & (1u << (o >> 8)))) s->iram[o] = v;
        return;
    }
    if (off >= 0x6000) { /* the BW-RAM, writes only when SWEN stands */
        if (s->swen)
            s->bwram[((uint32_t)s->sbm * 0x2000u + (off & 0x1FFFu)) & 0x3FFFFu] = v;
        return;
    }
    switch (0x2200u | (off & 0x1FF)) {
    case 0x2200: { /* CCNT: the CPU's control of the SA-1 */
        const uint8_t resb = s->sa1_resb;
        s->sa1_irq = (uint8_t)(v & 0x80);
        s->sa1_rdyb = (uint8_t)(v & 0x40);
        s->sa1_resb = (uint8_t)(v & 0x20);
        s->sa1_nmi = (uint8_t)(v & 0x10);
        s->smeg = (uint8_t)(v & 0x0F);
        if (resb && !s->sa1_resb) { /* the reset released: PC bank 0 */
            s->cpu.pc = s->crv;
            s->cpu.k = 0;
            s->cpu.halted = 0;
        }
        if (s->sa1_irq) s->sa1_irqfl = 1;
        if (s->sa1_nmi) s->sa1_nmifl = 1;
        return;
    }
    case 0x2201: /* SIE: the S-CPU's interrupt enable */
        s->cpu_irqen = (uint8_t)(v & 0x80);
        s->dma_irqen = (uint8_t)(v & 0x20);
        sa1_message_to_cpu();
        return;
    case 0x2202: /* SIC: the S-CPU's interrupt clear */
        if (v & 0x80) s->cpu_irqfl = 0;
        if (v & 0x20) s->dma_irqfl = 0;
        sa1_message_to_cpu();
        return;
    case 0x2203: s->crv = (uint16_t)((s->crv & 0xFF00) | v); return;
    case 0x2204: s->crv = (uint16_t)((v << 8) | (s->crv & 0xFF)); return;
    case 0x2205: s->cnv = (uint16_t)((s->cnv & 0xFF00) | v); return;
    case 0x2206: s->cnv = (uint16_t)((v << 8) | (s->cnv & 0xFF)); return;
    case 0x2207: s->civ = (uint16_t)((s->civ & 0xFF00) | v); return;
    case 0x2208: s->civ = (uint16_t)((v << 8) | (s->civ & 0xFF)); return;
    case 0x2220: s->cbmode = (uint8_t)(v & 0x80); s->cb = (uint8_t)(v & 7); return;
    case 0x2221: s->dbmode = (uint8_t)(v & 0x80); s->db = (uint8_t)(v & 7); return;
    case 0x2222: s->ebmode = (uint8_t)(v & 0x80); s->eb = (uint8_t)(v & 7); return;
    case 0x2223: s->fbmode = (uint8_t)(v & 0x80); s->fb = (uint8_t)(v & 7); return;
    case 0x2224: s->sbm = (uint8_t)(v & 0x1F); return;
    case 0x2226: s->swen = (uint8_t)(v & 0x80); return;
    case 0x2228: s->bwp = (uint8_t)(v & 0x0F); return;
    case 0x2229: s->siwp = v; return;
    /* the arithmetic unit: the multiply/divide latches on the MBH write
     * like the S-CPU's own unit latches on its B write */
    case 0x2250:
        s->acm = (uint8_t)(v & 0x02);
        s->md = (uint8_t)(v & 0x01);
        if (s->acm) s->mr = 0;
        return;
    case 0x2251: s->ma = (uint16_t)((s->ma & 0xFF00) | v); return;
    case 0x2252: s->ma = (uint16_t)((v << 8) | (s->ma & 0xFF)); return;
    case 0x2253: s->mb = (uint16_t)((s->mb & 0xFF00) | v); return;
    case 0x2254: {
        s->mb = (uint16_t)((v << 8) | (s->mb & 0xFF));
        if (!s->acm) {
            if (!s->md) { /* signed multiply */
                s->mr = (uint32_t)((int16_t)s->ma * (int16_t)s->mb);
                s->mb = 0;
            } else { /* unsigned divide: remainder high, quotient low */
                if (s->mb == 0) s->mr = 0;
                else {
                    const int16_t dividend = (int16_t)s->ma;
                    const uint16_t divisor = s->mb;
                    uint16_t rem = dividend >= 0 ? (uint16_t)(dividend % divisor)
                                                 : (uint16_t)((dividend % divisor + divisor) % divisor);
                    const uint16_t quot = (uint16_t)((dividend - rem) / divisor);
                    s->mr = (uint32_t)(rem << 16 | quot);
                }
                s->ma = 0;
                s->mb = 0;
            }
        } else { /* the 40-bit sigma */
            s->mr += (uint64_t)(int32_t)((int16_t)s->ma * (int16_t)s->mb);
            s->overflow = (uint8_t)(s->mr >> 40);
            s->mr &= (1ULL << 40) - 1;
            s->mb = 0;
        }
        return;
    }
    case 0x2258: /* VBD: the variable-length decoder's step */
        s->hl = (uint8_t)(v & 0x80);
        s->vb = (uint8_t)((v & 0x0F) ? (v & 0x0F) : 16);
        if (!s->hl) {
            s->vbit = (uint8_t)(s->vbit + s->vb);
            s->va += s->vbit >> 3;
            s->vbit &= 7;
        }
        return;
    case 0x2259: s->va = (s->va & 0xFFFF00u) | (uint32_t)v << 0; return;
    case 0x225A: s->va = (s->va & 0xFF00FFu) | (uint32_t)v << 8; return;
    case 0x225B: s->va = (s->va & 0x00FFFFu) | (uint32_t)v << 16; return;
    default: return;
    }
}

/* ---- the SA-1's side of the register file ---- */

static uint8_t sa1_reg_read(uint32_t at) {
    const unsigned off = at & 0xFFFF;
    switch (0x2200u | (off & 0x1FF)) {
    case 0x2301: /* CFR: the SA-1's flag read */
        return (uint8_t)((s->sa1_irqfl << 7) | (s->timer_irqfl << 6) |
                         (s->dma_irqfl << 5) | (s->sa1_nmifl << 4) | s->smeg);
    case 0x2200: return (uint8_t)((s->sa1_irq << 7) | (s->sa1_rdyb << 6) |
                                  (s->sa1_resb << 5) | (s->sa1_nmi << 4) | s->smeg);
    case 0x2209: return (uint8_t)((s->cpu_irq << 7) | (s->cpu_ivsw << 6) |
                                  (s->cpu_nvsw << 4) | s->cmeg);
    default: return machine.cpu.mdr;
    }
}

static void sa1_reg_write(uint32_t at, uint8_t v) {
    const unsigned off = at & 0xFFFF;
    switch (0x2200u | (off & 0x1FF)) {
    case 0x2209: { /* SCNT: the SA-1's control of the S-CPU */
        s->cpu_irq = (uint8_t)(v & 0x80);
        s->cpu_ivsw = (uint8_t)(v & 0x40);
        s->cpu_nvsw = (uint8_t)(v & 0x10);
        s->cmeg = (uint8_t)(v & 0x0F);
        if (s->cpu_irq) s->cpu_irqfl = 1;
        sa1_message_to_cpu();
        return;
    }
    case 0x220A: /* CIE */
        s->sa1_irqen = (uint8_t)(v & 0x80);
        s->timer_irqen = (uint8_t)(v & 0x40);
        s->dma_irqen = (uint8_t)(v & 0x20);
        s->sa1_nmien = (uint8_t)(v & 0x10);
        return;
    case 0x220B: /* CIC */
        if (v & 0x80) s->sa1_irqfl = 0;
        if (v & 0x40) s->timer_irqfl = 0;
        if (v & 0x20) s->dma_irqfl = 0;
        if (v & 0x10) s->sa1_nmifl = 0;
        return;
    case 0x220C: s->snv = (uint16_t)((s->snv & 0xFF00) | v); return;
    case 0x220D: s->snv = (uint16_t)((v << 8) | (s->snv & 0xFF)); return;
    case 0x220E: s->siv = (uint16_t)((s->siv & 0xFF00) | v); return;
    case 0x220F: s->siv = (uint16_t)((v << 8) | (s->siv & 0xFF)); return;
    case 0x2225: s->sw46 = (uint8_t)(v & 0x80); s->cbm = (uint8_t)(v & 0x7F); return;
    case 0x2227: s->cwen = (uint8_t)(v & 0x80); return;
    case 0x222A: s->ciwp = v; return;
    default: return;
    }
}

/* ---- the clock ---- */

void snes_sa1_run(uint32_t target_cpu) {
    if (!machine.sa1_attached) return;
    s->last_cpu = target_cpu;
    if (s->cpu.halted) return;
    const uint32_t target = target_cpu * 3; /* the SA-1 runs at three times */
    if ((int32_t)(target - s->cpu.cycles) <= 0) return;
    s->cpu.irq_line = (uint8_t)((s->sa1_irqfl && s->sa1_irqen) ||
                                (s->timer_irqfl && s->timer_irqen) ||
                                (s->dma_irqfl && s->dma_irqen));
    if ((s->sa1_nmifl && s->sa1_nmien) && !s->nmi_latched) {
        s->nmi_latched = 1;
        s->cpu.nmi_pending = 1;
    }
    s->active = 1;
    snes_interp_run(&s->cpu, target);
    s->active = 0;
}

void snes_sa1_reset(void) {
    snes_sa1_t *n = &machine.sa1;
    memset(n, 0, sizeof *n);
    n->sa1_resb = 1; /* the SA-1 waits in reset for CCNT */
    n->bwp = 0x0F;
    n->cpu.halted = 1;
    n->cpu.vec_custom = 1;
    n->cpu.vec_reset = 0;
    n->cpu.vec_nmi = 0;
    n->cpu.vec_irq = 0;
    machine.sa1_cpu_irq = 0;
}
