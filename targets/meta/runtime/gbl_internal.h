/* Private interfaces shared between gbl runtime modules. */
#ifndef GBL_INTERNAL_H
#define GBL_INTERNAL_H

#include <stdint.h>

#include "gbl.h"
#include "gbl_options.h"

void gbl_ppu_reset(void);
/* Advance the PPU to cycle now: inline check of the next transition, the
 * work itself in gbl_ppu_advance. */
extern uint32_t gbl_ppu_due;
void gbl_ppu_advance(uint32_t now);
#if GBL_CGB
extern uint8_t gbl_dshift; /* 1 in CGB double speed */
void gbl_ppu_set_speed(uint8_t d);
void gbl_speed_switch(void);
extern uint8_t gbl_hdma_pend, gbl_hdma_active; /* an HBlank block is due (set by the PPU) */
int gbl_ppu_in_hblank(void);
extern uint8_t gbl_cgb_pal[2][64], gbl_cgb_ps[2];
void gbl_cgb_pal_reset(void);
uint8_t gbl_cgb_pal_rd(uint8_t n);
void gbl_cgb_pal_wr(uint8_t n, uint8_t v);
extern uint8_t gbl_key1, gbl_cgb, gbl_vbk, gbl_svbk;
#endif
static inline void gbl_ppu_sync(uint32_t now) {
    if ((int32_t)(now - gbl_ppu_due) >= 0) gbl_ppu_advance(now);
}
/* Call whenever device state the next timer/serial event depends on
 * changes; gbl_update_evt() then recomputes its cached earliest device
 * event instead of reusing it (PPU transitions invalidate it themselves). */
void gbl_evt_invalidate(void);
uint32_t gbl_core_next_event(uint32_t best); /* timer and serial */
uint32_t gbl_ppu_stable_until(void); /* next cycle LY or STAT can change */
uint8_t gbl_ppu_read(uint8_t n);
void gbl_ppu_write(uint8_t n, uint8_t v);
extern uint8_t gbl_oam_dirty;
void gbl_ppu_content_changed(void);       /* OAM bytes changed */
void gbl_ppu_vram_changed(uint16_t off); /* VRAM byte at 0x8000 + off changed */
/* OAM changed since the sprite index was built */

void gbl_apu_reset(void);
uint8_t gbl_apu_read(uint8_t n);
void gbl_apu_write(uint8_t n, uint8_t v);

uint8_t gbl_rd_slow(uint16_t a);
int gbl_ppu_blocked(int what);        /* GBL_ACCESS_BLOCK: 1 VRAM, 2 OAM */         /* gbl_rd for unmapped pages */
void gbl_wr_slow(uint16_t a, uint8_t v); /* gbl_wr for unmapped pages */

/* Interpreter state kept in RAM for gbl_interp_asm.S (gbl_core.c). */
typedef struct {
    uint8_t regofs[8];
    uint16_t sp0;    /* innermost gbl_exec's entry SP */
    uint8_t bounded; /* 0 under gbl_run */
} gbl_interp_ctx_t;
extern gbl_interp_ctx_t gbl_interp_ctx;

/* Save states: each module streams its state through io (gbl_state_io). */
void gbl_ppu_state_io(gbl_state_fn io, void *ctx, int load);
void gbl_apu_state_io(gbl_state_fn io, void *ctx, int load);

int gbl_irq_enter(void);     /* push PC and vector if IME and pending */
void gbl_halt_wait(void);    /* advance time until an interrupt is pending */
extern uint8_t gbl_halt_bug;

#if GBL_ERAM_SIZE > 0 && !GBL_ERAM_ON_SD
extern uint8_t gbl_eram[GBL_ERAM_SIZE];
#endif

#endif
