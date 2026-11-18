/* The Neo Geo Pocket run fingerprint: one recipe, the harness and the PC
 * window both print it, so a scripted session hashes the same on both.
 * m0_host_util.h supplies M0_RUN_HASH_INIT and m0_hash_bytes. */
#ifndef M0_NGP_HASH_H
#define M0_NGP_HASH_H

#include "ngp.h"
#include "m0_host_util.h"

static uint32_t ngp_run_hash(const uint16_t *frame) {
    uint32_t h = M0_RUN_HASH_INIT;
    const uint32_t words[] = {
        machine.cpu.pc,
        machine.cpu.bank[0][0], machine.cpu.bank[0][1], machine.cpu.bank[0][2], machine.cpu.bank[0][3],
        machine.cpu.bank[1][0], machine.cpu.bank[1][1], machine.cpu.bank[1][2], machine.cpu.bank[1][3],
        machine.cpu.bank[2][0], machine.cpu.bank[2][1], machine.cpu.bank[2][2], machine.cpu.bank[2][3],
        machine.cpu.bank[3][0], machine.cpu.bank[3][1], machine.cpu.bank[3][2], machine.cpu.bank[3][3],
        machine.cpu.gpr[0], machine.cpu.gpr[1], machine.cpu.gpr[2], machine.cpu.gpr[3],
        machine.cyc, machine.frames, machine.gfx.raster_line,
    };
    const uint8_t bytes[] = {
        (uint8_t)machine.cpu.sr, (uint8_t)(machine.cpu.sr >> 8), machine.cpu.f_dash,
        machine.cpu.halted, machine.cpu.fault,
        machine.trun, machine.t01mod, machine.t23mod, machine.trdc,
        machine.timer_tick[0], machine.timer_tick[1], machine.timer_tick[2], machine.timer_tick[3],
        machine.timer_threshold[0], machine.timer_threshold[1], machine.timer_threshold[2], machine.timer_threshold[3],
        machine.int_prio[0], machine.int_prio[1], machine.int_prio[2], machine.int_prio[3],
        machine.int_prio[4], machine.int_prio[5], machine.int_prio[6], machine.int_prio[7],
        machine.int_prio[8], machine.int_prio[9], machine.int_prio[10],
        machine.hdma_vector[0], machine.hdma_vector[1], machine.hdma_vector[2], machine.hdma_vector[3],
        machine.dma_s[0] & 0xFF, machine.dma_s[1] & 0xFF, machine.dma_s[2] & 0xFF, machine.dma_s[3] & 0xFF,
        machine.dma_d[0] & 0xFF, machine.dma_d[1] & 0xFF, machine.dma_d[2] & 0xFF, machine.dma_d[3] & 0xFF,
        (uint8_t)machine.dma_c[0], (uint8_t)(machine.dma_c[0] >> 8),
        (uint8_t)machine.dma_c[1], (uint8_t)(machine.dma_c[1] >> 8),
        (uint8_t)machine.dma_c[2], (uint8_t)(machine.dma_c[2] >> 8),
        (uint8_t)machine.dma_c[3], (uint8_t)(machine.dma_c[3] >> 8),
        machine.dma_m[0], machine.dma_m[1], machine.dma_m[2], machine.dma_m[3],
        machine.buttons, machine.serial_buf, machine.comm_byte,
        machine.chip_enabled, machine.z80_enabled,
        machine.latch_left, machine.latch_right,
        machine.sq_period[0] & 0xFF, machine.sq_period[0] >> 8,
        machine.sq_period[1] & 0xFF, machine.sq_period[1] >> 8,
        machine.sq_period[2] & 0xFF, machine.sq_period[2] >> 8,
        machine.sq_phase[0], machine.sq_phase[1], machine.sq_phase[2],
        machine.sq_volume_left[0], machine.sq_volume_left[1], machine.sq_volume_left[2], machine.sq_volume_left[3],
        machine.sq_volume_right[0], machine.sq_volume_right[1], machine.sq_volume_right[2], machine.sq_volume_right[3],
        machine.noise_period_select, machine.noise_tap, machine.noise_shifter, machine.noise_phase,
        machine.noise_period_extra & 0xFF, machine.noise_period_extra >> 8,
        machine.dac_left, machine.dac_right,
        machine.gfx.control_int, machine.gfx.wba_h, machine.gfx.wba_v,
        machine.gfx.wsi_h, machine.gfx.wsi_v, machine.gfx.screen_period,
        machine.gfx.control_2d, machine.gfx.po_h, machine.gfx.po_v, machine.gfx.p_f,
        machine.gfx.s1so_h, machine.gfx.s1so_v, machine.gfx.s2so_h, machine.gfx.s2so_v,
        machine.gfx.bg_col, machine.gfx.k2ge_mode, machine.gfx.c_ovr, machine.gfx.blink,
    };
    h = m0_hash_bytes(h, words, sizeof words);
    h = m0_hash_bytes(h, bytes, sizeof bytes);
    h = m0_hash_bytes(h, machine.ram, sizeof machine.ram);
    h = m0_hash_bytes(h, machine.rtc_latch, sizeof machine.rtc_latch);
    h = m0_hash_bytes(h, machine.gfx.scroll_vram, sizeof machine.gfx.scroll_vram);
    h = m0_hash_bytes(h, machine.gfx.character_ram, sizeof machine.gfx.character_ram);
    h = m0_hash_bytes(h, machine.gfx.sprite_vram, sizeof machine.gfx.sprite_vram);
    h = m0_hash_bytes(h, machine.gfx.sprite_pal, sizeof machine.gfx.sprite_pal);
    h = m0_hash_bytes(h, machine.gfx.palette_ram, sizeof machine.gfx.palette_ram);
    h = m0_hash_bytes(h, machine.gfx.spplt, sizeof machine.gfx.spplt);
    h = m0_hash_bytes(h, machine.gfx.scrp1plt, sizeof machine.gfx.scrp1plt);
    h = m0_hash_bytes(h, machine.gfx.scrp2plt, sizeof machine.gfx.scrp2plt);
    return m0_hash_bytes(h, frame, NGP_SCREEN_H * NGP_SCREEN_W * sizeof frame[0]);
}

#endif
