/* Vectrex core shared by the PC host and the console frontends: the 6809,
 * the bus (cartridge, 1 KiB of RAM, 6522, system ROM), the analog vector
 * generator driven by the VIA and the AY-3-8912.
 *
 * The core has no picture of its own: each straight stroke of the beam
 * reaches the frontend through vx_draw() (the frontend's function), in the
 * beam's coordinates, so the host can draw 512x512 and a console 128x128. */
#pragma once
#if __has_include("vx_config.h") /* a generated project's: VX_NATIVE */
#include "vx_config.h"
#endif
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The beam's coordinates: 1/256 of a pixel of a 512x512 square, (0,0) the
 * top left corner; the integrators rest at its centre. */
#define VX_GRID 512
#define VX_CENTER (VX_GRID * 128)
/* 1.5 MHz, a 50 Hz frame (Wait_Recal's T2 of 30000 cycles) */
#define VX_CLOCK 1500000u
#define VX_FRAME_CYCLES 30000u

typedef struct {
  uint8_t a, b, dp, cc;
  uint16_t x, y, u, s, pc;
  uint32_t cycles;
  uint32_t start;  /* the current instruction's first cycle: the VIA's time */
  uint32_t writes; /* bus writes so far (the spin detector's witness) */
  uint32_t illegal;
  uint8_t waiting; /* 1 CWAI (state stacked), 2 SYNC */
} M6809;
typedef struct {
  uint8_t ora, orb, ddra, ddrb, acr, pcr, ifr, ier, sr;
  uint16_t t1, t1_latch, t2;
  uint8_t t2_latch;
  uint8_t t1_armed, t2_armed; /* t1_armed: PB7 low, the ramp under T1 */
  uint8_t xsh, ysh, rsh, intensity;
  uint8_t sr_bits, sr_wait, cb2;
  /* whether the ramp moves the beam: analog()'s bit 8, kept with `sig`
   * below (here, within Thumb-1's byte offsets: via_asm.S) */
  uint8_t ramp;
  int32_t beam_x, beam_y;
  /* the stroke being drawn: from (pen_x, pen_y) to the beam, pen_cycles
   * long, lit or not; ended by any change of the analog state */
  int32_t pen_x, pen_y;
  uint32_t pen_cycles, pen_sig;
  uint8_t pen_lit, pen_z;
  uint8_t ay_reg, ay[16];
  uint32_t ay_epoch;
  uint32_t segments;
  /* the VIA is brought up to date when the CPU touches it (via_sync):
   * `time` is the cycle it has reached */
  uint32_t time, accesses;
  /* cycles from `time` before T1, T2 or the shift register can act, and
   * the beam's velocity meanwhile (0 without the ramp): within `calm` a
   * sync is a straight move (via_sync) */
  uint32_t calm;
  int32_t vel_x, vel_y;
  /* the analog state's signature (what analog() returns), kept current
   * where its inputs change (via.c: derive) rather than recomputed at
   * every look */
  uint32_t sig;
  /* the last IFR read: a loop polling it with nothing else changing is
   * fast-forwarded to the VIA's next event (via.c) */
  uint32_t spin_cycles, spin_accesses, spin_writes;
  uint16_t spin_pc, spin_x, spin_y, spin_u, spin_s;
  uint8_t spin_a, spin_b, spin_dp, spin_cc, spin_ifr;
  uint32_t skipped;
} VIA;
typedef struct {
  uint32_t tone_count[3], noise_count, env_count;
  uint32_t step_frac, lfsr, last_epoch;
  uint8_t tone_level[3], noise_level, env_level, env_hold;
  int8_t env_dir;
} AySynth;
typedef struct {
  M6809 cpu;
  VIA via;
  AySynth sound;
  uint8_t ram[1024];
  const uint8_t *bios; /* 8 KiB at E000 (a 4 KiB Executive in its upper half) */
  /* the bus by 1 KiB pages for the run loop: plain memory, or 0 for the
   * VIA, the unmapped space and a partial last cartridge page (vx_read and
   * vx_write then); built by vx_reset */
  const uint8_t *rmap[64];
  uint8_t *wmap[64];
  uint8_t buttons, joy_x, joy_y;
  uint32_t frame_end; /* the current frame's last cycle */
  uint8_t fast_boot;
  /* nonzero when the run loop must look closer before an instruction: an
   * enabled VIA interrupt, a wait (SYNC, CWAI), the boot shortcut, the
   * illegal-opcode limit (m6809_run.c) */
  uint8_t attention;
} Vectrex;

#ifdef M0_ROM_EXTERNAL /* PC build whose cartridge is loaded at run time (m0_rom_ext.h) */
extern const uint8_t *vectrex_cart;
#else
extern const uint8_t vectrex_cart[];
#endif
extern const unsigned vectrex_cart_size;

/* the frontend's: a stroke from (x0, y0) to (x1, y1) in beam coordinates,
 * brightness z (1..127), drawn during `cycles` CPU cycles */
void vx_draw(Vectrex *, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
             unsigned z, unsigned cycles);

uint8_t vx_read(Vectrex *, uint16_t);
void vx_write(Vectrex *, uint16_t, uint8_t);
uint16_t vx_read16(Vectrex *, uint16_t);
void vx_reset(Vectrex *);
/* run instructions until the cycle counter reaches `stop`; vx_frame: one
 * 50 Hz frame (a slice-wise caller runs to frame_end + VX_FRAME_CYCLES
 * itself, then calls vx_end_frame) */
void vx_run_until(Vectrex *, uint32_t stop);
void vx_frame(Vectrex *);
void vx_end_frame(Vectrex *);
uint32_t vx_machine_state_bytes(void);
void vx_machine_state_io(Vectrex *, void (*io)(void *, uint32_t, void *), void *ctx, int load);
/* one instruction (the reference core), or instructions until the clock
 * reaches `stop` (the run loop, m6809_run.c: the same results) */
void m6809_step(Vectrex *);
void m6809_run(Vectrex *, uint32_t stop);
void via_tick(Vectrex *, unsigned);
void via_flush(Vectrex *);
/* after setting the VIA's fields directly (vx_reset) */
void via_derive(Vectrex *);
/* bring the VIA (timers, beam) up to cycle t */
static inline void via_sync(Vectrex *v, uint32_t t) {
  VIA *const p = &v->via;
  const int32_t n = (int32_t)(t - p->time);
  if (n <= 0)
    return;
  if ((uint32_t)n < p->calm) {
    p->calm -= (uint32_t)n;
    p->time = t;
    p->t1 = (uint16_t)(p->t1 - (uint32_t)n);
    p->t2 = (uint16_t)(p->t2 - (uint32_t)n);
    p->pen_cycles += (uint32_t)n;
    p->beam_x += n * p->vel_x;
    p->beam_y += n * p->vel_y;
    return;
  }
  via_tick(v, (unsigned)n);
}
uint8_t via_read(Vectrex *, unsigned);
void via_write(Vectrex *, unsigned, uint8_t);
/* the AY's output at `rate` Hz, for the current registers */
void ay_render(Vectrex *, int16_t *, unsigned, unsigned);

/* Clip the stroke to [0, w) x [0, h) (any scale): 0 when nothing is left. */
static inline int vx_clip(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1,
                          int32_t w, int32_t h) {
  for (int pass = 0; pass < 4; pass++) {
    int32_t ax = *x0, ay = *y0, bx = *x1, by = *y1;
    if (ax >= 0 && ax < w && ay >= 0 && ay < h && bx >= 0 && bx < w &&
        by >= 0 && by < h)
      return 1;
    if ((ax < 0 && bx < 0) || (ay < 0 && by < 0) || (ax >= w && bx >= w) ||
        (ay >= h && by >= h))
      return 0;
    /* move the endpoint outside onto the edge it crosses */
    int out_a = !(ax >= 0 && ax < w && ay >= 0 && ay < h);
    int32_t *px = out_a ? x0 : x1, *py = out_a ? y0 : y1;
    int32_t ox = out_a ? bx : ax, oy = out_a ? by : ay;
    int32_t dx = *px - ox, dy = *py - oy;
    if (*px < 0) {
      *py = oy + (int32_t)((int64_t)dy * (0 - ox) / dx);
      *px = 0;
    } else if (*px >= w) {
      *py = oy + (int32_t)((int64_t)dy * (w - 1 - ox) / dx);
      *px = w - 1;
    } else if (*py < 0) {
      *px = ox + (int32_t)((int64_t)dx * (0 - oy) / dy);
      *py = 0;
    } else {
      *px = ox + (int32_t)((int64_t)dx * (h - 1 - oy) / dy);
      *py = h - 1;
    }
  }
  return *x0 >= 0 && *x0 < w && *y0 >= 0 && *y0 < h && *x1 >= 0 && *x1 < w &&
         *y1 >= 0 && *y1 < h;
}

#ifdef __cplusplus
}
#endif
