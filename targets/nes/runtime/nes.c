#include "nes.h"
#ifdef NES_TRACE
#include "nes_trace.h"
#endif

nes_t machine; /* the CPU's bus lands here (one machine per firmware, as
                * the Z80 runtime's own machine) */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the SG-1000
 * and Game Gear runtimes), so a session played in gm0 replays on the host
 * harness (--input).  nes_reset references it: --gc-sections would drop it
 * otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    nes_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.input, 0 };

uint8_t nes_bus_read(uint16_t at) {
    nes_t *nes = &machine;
    if (at < 0x2000) return nes->ram[at & 0x7FF];
    if (at < 0x4000) return nes_ppu_read(&nes->ppu, at & 7);
    if (at == 0x4015) return nes_apu_read(&nes->apu);
    if (at == 0x4016 || at == 0x4017) {
        const unsigned pad = at & 1;
        if (nes->strobe) return (uint8_t)(nes->input & 1);
        const uint8_t bit = (uint8_t)(nes->shift[pad] & 1);
        nes->shift[pad] >>= 1;
        /* past the 8 buttons the pads read 0 (SMB keys its title timers
         * on that signature) */
        return bit;
    }
    if (at >= 0x6000 && at < 0x8000) return nes->wram[at & 0x1FFF];
    if (at >= 0x8000) {
        /* the cartridge's two windows: the bank at $8000-$BFFF, the fixed
         * one at $C000-$FFFF (NROM: the PRG fills $8000-$FFFF, a 16 KiB
         * one mirrors) */
        return nes->prg[(at & 0x3FFF) + (at >= 0xC000 ? nes->cpu.fixed_base : nes->cpu.bank_base)];
    }
    return 0; /* the APU's registers and the expander read open bus */
}

void nes_bus_write(uint16_t at, uint8_t value) {
    nes_t *nes = &machine;
    if (at < 0x2000) { nes->ram[at & 0x7FF] = value; return; }
    if (at >= 0x6000 && at < 0x8000) { nes->wram[at & 0x1FFF] = value; return; }
    if (at >= 0x8000) {
        /* UxROM: the write's value is the bank the $8000 window maps (the
         * bus conflict is ignored: the games' write tables agree with the
         * ROM byte); NROM has no register */
        if (nes->cpu.banked) {
            const unsigned banks = (unsigned)(nes->prg_size / 16384u);
            nes->cpu.bank = (uint8_t)(value & (banks - 1));
            nes->cpu.bank_base = (uint32_t)nes->cpu.bank * 16384u;
        }
        return;
    }
    if (at < 0x4000) { nes_ppu_write(&nes->ppu, at & 7, value); return; }
    if (at == 0x4014) { /* OAM DMA: 256 bytes, the CPU stalls for it */
        for (unsigned i = 0; i < 256; i++)
            nes->oam[(unsigned)((nes->ppu.oam_addr + i) & 0xFF)] = nes_bus_read((uint16_t)((value << 8) | i));
        nes6502_stall(&nes->cpu, 513);
        nes->ppu.oam_dirty = 1;
        return;
    }
    if (at == 0x4016) {
        if ((value & 1) && !nes->strobe) {
            nes->shift[0] = nes->input;
            nes->shift[1] = 0;
        }
        nes->strobe = value & 1;
        return;
    }
    if (at < 0x4018) { nes_apu_write(&nes->apu, at - 0x4000, value); return; }
}

#ifdef NES_PPU_REFERENCE
static void row_bridge(void *host, unsigned line) {
    nes_t *nes = (nes_t *)host;
    if (nes->row) nes->row(nes->host, line, nes->ppu.row);
}
#endif

void nes_init(const uint8_t *prg, uint32_t prg_bytes,
              const uint8_t *chr, uint32_t chr_bytes, int vertical_mirror) {
    nes_t *nes = &machine;
    for (unsigned i = 0; i < sizeof nes->ram; i++) nes->ram[i] = 0;
    for (unsigned i = 0; i < sizeof nes->oam; i++) nes->oam[i] = 0;
    for (unsigned i = 0; i < sizeof nes->wram; i++) nes->wram[i] = 0;
    static uint8_t nametable[2048], palette[32];
    for (unsigned i = 0; i < sizeof nametable; i++) nametable[i] = 0;
    for (unsigned i = 0; i < sizeof palette; i++) palette[i] = 0;
    nes->prg = prg;
    nes->prg_size = prg_bytes;
#ifndef NES_CHR_RAM
#define NES_CHR_RAM 0
#endif
    const int chr_is_ram = chr_bytes == 0 || NES_CHR_RAM;
    if (chr_is_ram) {
        for (unsigned i = 0; i < sizeof nes->chr_ram; i++) nes->chr_ram[i] = 0;
        nes->chr = nes->chr_ram;
    } else {
        nes->chr = chr;
    }
    /* the CPU's fast paths (6502.c): RAM, the two PRG windows */
    nes->cpu.ram = nes->ram;
    nes->cpu.prg = prg;
    nes->cpu.bank_base = 0;
    nes->cpu.fixed_base = prg_bytes - 16384u;
    nes->cpu.bank = 0;
    nes->cpu.banked = (uint8_t)(prg_bytes > 32768u);
    nes->cpu.bank_count = (uint8_t)(prg_bytes / 16384u);
    nes->ppu.nametable = nametable;
    nes->ppu.palette = palette;
    nes->ppu.oam = nes->oam;
    nes->ppu.chr = nes->chr;
    nes->ppu.chr_writable = (uint8_t)chr_is_ram;
    nes->ppu.vertical_mirror = vertical_mirror != 0;
#ifdef NES_PPU_REFERENCE
    nes->ppu.render_row = row_bridge;
    nes->row = 0;
#endif
    nes->ppu.panel = 0;
    nes_ppu_set_rows(&nes->ppu, 128); /* the frontend may pick another */
    nes->ppu.skip = 0;
    nes->ppu.host = nes;
    nes->host = 0;
    nes_reset();
}

void nes_reset(void) {
    nes_t *nes = &machine;
    __asm__ volatile("" : : "r"(&nes_input_probe));
    nes6502_reset(&nes->cpu);
    nes_ppu_reset(&nes->ppu);
    nes_apu_reset(&nes->apu);
    nes->strobe = 0;
    nes->shift[0] = nes->shift[1] = 0;
    nes->dots = 0;
    nes->line = 0;
}

uint32_t nes_state_bytes(void) {
    return (uint32_t)(sizeof machine + 2048u + 32u);
}

void nes_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load) {
    nes_t *nes = &machine;
    /* The saved pointers belong to an earlier process or invocation.  The
     * live cartridge, frontend callbacks and memory allocations own them. */
    uint8_t *ram = nes->cpu.ram, *nametable = nes->ppu.nametable;
    uint8_t *palette = nes->ppu.palette, *oam = nes->ppu.oam;
    const uint8_t *prg = nes->prg, *chr = nes->chr;
    uint16_t *panel_out = nes->ppu.panel_out;
    void (*panel)(void *, unsigned) = nes->ppu.panel;
#ifdef NES_PPU_REFERENCE
    void (*render_row)(void *, unsigned) = nes->ppu.render_row;
    void (*row)(void *, unsigned, const uint16_t *) = nes->row;
#endif
    void *host = nes->host;
    io(nes, (uint32_t)sizeof *nes, ctx);
    if (load) {
        nes->cpu.ram = ram;
        nes->cpu.prg = prg;
        nes->prg = prg;
        nes->chr = chr;
        nes->ppu.nametable = nametable;
        nes->ppu.palette = palette;
        nes->ppu.oam = oam;
        nes->ppu.chr = chr;
        nes->ppu.panel_out = panel_out;
        nes->ppu.panel = panel;
        nes->ppu.host = nes;
#ifdef NES_PPU_REFERENCE
        nes->ppu.render_row = render_row;
        nes->row = row;
#endif
        nes->host = host;
    }
    io(nametable, 2048u, ctx);
    io(palette, 32u, ctx);
    if (load) nes_ppu_state_loaded(&nes->ppu);
}

uint32_t nes_state_frame(void) { return machine.frames; }

uint32_t nes_state_identity(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < machine.prg_size; i++) h = (h ^ machine.prg[i]) * 16777619u;
    if (machine.chr)
        for (uint32_t i = 0; i < 8192u; i++) h = (h ^ machine.chr[i]) * 16777619u;
    return h | 1u;
}

void nes_frame(void) {
    nes_t *nes = &machine;
    nes->frames++;
    /* the strobe takes the pads' state once, at the frame's start */
    if (nes->strobe) { nes->shift[0] = nes->input; }
    nes->line = 0;
    nes_frame_rest();
}

/* The current frame's remaining lines: nes_frame runs the whole 262, an
 * observer cut mid-frame (deepeel's explorer stops the CPU slice through
 * nes_trace.stop) resumes through this — machine.line holds the next
 * un-run line, the frame's dots carry over. */
void nes_frame_rest(void) {
    nes_t *nes = &machine;
    for (; nes->line < 262; nes->line++) {
        /* 341 PPU dots = 113 CPU cycles and two thirds: the thirds add up
         * across lines, as they do on the machine (341 = 113*3 + 2, no
         * division: ARMv6-M would call __aeabi_uidiv) */
        const unsigned line = nes->line;
        uint32_t step = 113u;
        nes->dots += 2u;
        if (nes->dots >= 3u) { nes->dots -= 3u; step++; }
        const uint32_t cpu_until = nes->cpu.cycles + step;
        nes6502_run(&nes->cpu, cpu_until);
        if (nes_ppu_line(&nes->ppu, line)) nes->cpu.nmi_edge = 1;
#ifdef NES_TRACE
        if (nes_trace.stop) { nes->line++; break; } /* the observer froze the
            * CPU mid-frame: this line's PPU step is skipped, the observer's
            * snapshot keeps machine.line past it */
#endif
    }
}

void nes_audio(uint8_t *dst, unsigned n, unsigned rate) {
    nes_apu_render(&machine.apu, dst, n, rate);
}

/* the consoles' audio drivers (meta_audio.cpp and its like) call these two
 * without knowing the machine; the samples are rendered at the rate the
 * driver plays (the META's 22049 Hz: a frame asks for 367 of them) */
static unsigned audio_rate = 11025u;
void nes_audio_set_rate(unsigned rate) { audio_rate = rate ? rate : 11025u; }
void nes_audio_render(uint8_t *buffer, unsigned n) {
    nes_apu_render(&machine.apu, buffer, n, audio_rate);
}
