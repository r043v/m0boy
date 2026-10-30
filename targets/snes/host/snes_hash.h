/* The run fingerprint's recipe, shared by the harness and the PC window:
 * the registers field by field, the memories, the APU's state and the
 * frame's audio — a scripted session must print the same run_hash (and
 * audio_hash) from both.  image_hash hashes the displayed frame's rows
 * (machine.screen). */
#ifndef SNES_HASH_H
#define SNES_HASH_H
#include <stdint.h>
#include <stddef.h>
/* included after snes.h: the machine is the includer's */

static uint32_t snes_fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t snes_image_hash(void) {
    return snes_fnv(machine.screen, sizeof machine.screen, 2166136261u);
}

static uint32_t snes_run_hash(void) {
    uint32_t h = 2166136261u;
    const uint8_t regs[16] = {
        (uint8_t)machine.cpu.a, (uint8_t)(machine.cpu.a >> 8),
        (uint8_t)machine.cpu.x, (uint8_t)(machine.cpu.x >> 8),
        (uint8_t)machine.cpu.y, (uint8_t)(machine.cpu.y >> 8),
        (uint8_t)machine.cpu.s, (uint8_t)(machine.cpu.s >> 8),
        (uint8_t)machine.cpu.d, (uint8_t)(machine.cpu.d >> 8),
        machine.cpu.k, machine.cpu.dbr, machine.cpu.p, machine.cpu.e,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
    };
    h = snes_fnv(regs, sizeof regs, h);
    h = snes_fnv(&machine.cpu.cycles, 4, h);
    h = snes_fnv(machine.wram, sizeof machine.wram, h);
    h = snes_fnv(machine.vram, sizeof machine.vram, h);
    h = snes_fnv(machine.cgram, sizeof machine.cgram, h);
    h = snes_fnv(machine.oam, sizeof machine.oam, h);
    h = snes_fnv(machine.ppu_regs, sizeof machine.ppu_regs, h);
    h = snes_fnv(machine.dma, sizeof machine.dma, h);
    h = snes_fnv(machine.scroll, sizeof machine.scroll, h);
    h = snes_fnv(machine.sram, machine.sram_bytes, h);
    const uint8_t state[] = {
        machine.cpu.mdr, (uint8_t)machine.cpu.nmi_enable,
        (uint8_t)machine.cpu.vblank_flag, (uint8_t)machine.cpu.nmi_pending,
        (uint8_t)machine.cpu.irq_line, (uint8_t)machine.cpu.halted,
        (uint8_t)machine.cpu.waiting, (uint8_t)machine.cpu.wram_addr,
        (uint8_t)(machine.cpu.wram_addr >> 8), (uint8_t)(machine.cpu.wram_addr >> 16),
        (uint8_t)machine.vmaddr, (uint8_t)(machine.vmaddr >> 8),
        (uint8_t)machine.cgaddr, (uint8_t)(machine.cgaddr >> 8),
        (uint8_t)machine.oamaddr, (uint8_t)(machine.oamaddr >> 8),
        machine.cg_latch, machine.oam_latch, machine.scroll_latch, machine.vblank, machine.hblank,
        machine.obj_range_over, machine.obj_time_over,
        machine.timer_latch,
        (uint8_t)machine.htime, (uint8_t)(machine.htime >> 8),
        (uint8_t)machine.vtime, (uint8_t)(machine.vtime >> 8),
        machine.timeup, machine.h_irq_en, machine.v_irq_en,
        machine.hdma_enable,
        machine.hdma_run[0], machine.hdma_run[1], machine.hdma_run[2], machine.hdma_run[3],
        machine.hdma_run[4], machine.hdma_run[5], machine.hdma_run[6], machine.hdma_run[7],
        machine.hdma_done[0], machine.hdma_done[1], machine.hdma_done[2], machine.hdma_done[3],
        machine.hdma_done[4], machine.hdma_done[5], machine.hdma_done[6], machine.hdma_done[7],
        machine.hdma_count[0], machine.hdma_count[1], machine.hdma_count[2], machine.hdma_count[3],
        machine.hdma_count[4], machine.hdma_count[5], machine.hdma_count[6], machine.hdma_count[7],
        machine.hdma_len[0], machine.hdma_len[1], machine.hdma_len[2], machine.hdma_len[3],
        machine.hdma_len[4], machine.hdma_len[5], machine.hdma_len[6], machine.hdma_len[7],
        machine.hdma_rep[0], machine.hdma_rep[1], machine.hdma_rep[2], machine.hdma_rep[3],
        machine.hdma_rep[4], machine.hdma_rep[5], machine.hdma_rep[6], machine.hdma_rep[7],
        (uint8_t)machine.joypad, (uint8_t)(machine.joypad >> 8),
        (uint8_t)machine.joy_auto, (uint8_t)(machine.joy_auto >> 8),
        (uint8_t)machine.joy_shift, (uint8_t)(machine.joy_shift >> 8),
        machine.joy_strobe, machine.joy_count, machine.joy_auto_enable, machine.joy_busy,
    };
    h = snes_fnv(state, sizeof state, h);
    h = snes_fnv(&machine.frames, 4, h);
    /* the APU: the SPC700's registers and A-RAM, the DSP's register file */
    const uint8_t smp_state[] = {
        (uint8_t)machine.smp.pc, (uint8_t)(machine.smp.pc >> 8),
        machine.smp.a, machine.smp.x, machine.smp.y, machine.smp.sp, machine.smp.psw,
        machine.smp.control, machine.smp.dsp_addr, machine.smp.halted,
        machine.smp.timer_phase[0], machine.smp.timer_phase[1], machine.smp.timer_phase[2],
        machine.smp.timer_out[0], machine.smp.timer_out[1], machine.smp.timer_out[2],
        machine.smp.ipl_state, machine.smp.ipl_y,
        (uint8_t)machine.smp.ipl_addr, (uint8_t)(machine.smp.ipl_addr >> 8),
        machine.smp.ports_in[0], machine.smp.ports_in[1], machine.smp.ports_in[2], machine.smp.ports_in[3],
        machine.smp.ports_out[0], machine.smp.ports_out[1], machine.smp.ports_out[2], machine.smp.ports_out[3],
    };
    h = snes_fnv(smp_state, sizeof smp_state, h);
    h = snes_fnv(machine.smp.ram, sizeof machine.smp.ram, h);
    h = snes_fnv(machine.dsp.regs, sizeof machine.dsp.regs, h);
    h = snes_fnv(&machine.dsp.key_on, 3, h);
    for (unsigned v = 0; v < 8; v++) {
        const uint8_t vs[] = {
            (uint8_t)machine.dsp.voice[v].env, (uint8_t)(machine.dsp.voice[v].env >> 8),
            (uint8_t)machine.dsp.voice[v].pos, (uint8_t)(machine.dsp.voice[v].pos >> 8),
            machine.dsp.voice[v].adsr_state, machine.dsp.voice[v].rate_count,
            machine.dsp.voice[v].hdr, machine.dsp.voice[v].nib,
            (uint8_t)machine.dsp.voice[v].src, (uint8_t)(machine.dsp.voice[v].src >> 8),
            (uint8_t)machine.dsp.voice[v].start, (uint8_t)(machine.dsp.voice[v].start >> 8),
            (uint8_t)machine.dsp.voice[v].loop, (uint8_t)(machine.dsp.voice[v].loop >> 8),
        };
        h = snes_fnv(vs, sizeof vs, h);
    }
    return h;
}

static uint32_t snes_audio_hash(const int16_t *audio, size_t n) {
    return snes_fnv(audio, n * sizeof *audio, 2166136261u);
}
#endif
