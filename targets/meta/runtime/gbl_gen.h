/* Helpers for code emitted by m0recomp --target meta. */
#ifndef GBL_GEN_H
#define GBL_GEN_H

#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"

#if GBL_VRAM_HOOK
#define GBL_VRAM_WR(off, v) gbl_vram_wr((off), (v))
#else
#define GBL_VRAM_WR(off, v) (gbl_vram[(off)] = (uint8_t)(v))
#endif

/* Region entry: compiled regions call each other directly, so a guest
 * recursing without returning (a crashed game looping through RST $38)
 * would overflow the C stack.  Below gbl_stack_limit the region returns at
 * once with gbr.pc untouched; its callers see an unexpected pc and unwind
 * to a gbl_exec loop higher up, which resumes from gbr.pc. */
#if defined(__arm__)
#define GBL_SP() ({ uintptr_t sp_; __asm__ volatile("mov %0, sp" : "=r"(sp_)); sp_; })
#else
#define GBL_SP() ((uintptr_t)__builtin_frame_address(0))
#endif
#ifndef GBL_STACK_GUARD
#define GBL_STACK_GUARD 1
#endif
#if GBL_STACK_GUARD
#define ENTER() do { if (GBL_SP() < gbl_stack_limit) return; } while (0)
#else
#define ENTER() do { } while (0)
#endif

/* RETI: with GBL_DEBUG_RESTART, the registers are checked against those at
 * the interrupt's entry (gbl_dbg_reti). */
#if GBL_DEBUG_RESTART
#define RETI() do { gbr.pc = gbl_pop(); gbl_dbg_reti(); gbr.ime = 1; gbl_update_evt(); } while (0)
#else
#define RETI() do { gbr.pc = gbl_pop(); gbr.ime = 1; gbl_update_evt(); } while (0)
#endif

#define CY(n) (gbr.cyc += (uint32_t)(n))
#define EVT() ((int32_t)(gbr.cyc - gbr.next_evt) >= 0)
/* Poll devices/interrupts; leave the region if an interrupt did not return
 * to pc (the enclosing gbl_exec loop then continues from gbr.pc). */
#define SYNC(pc) do { if (EVT() && gbl_event(pc)) return; } while (0)

#endif
