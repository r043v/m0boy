/*
 * gbl CPU: flag helpers shared with generated code, and the fallback
 * interpreter used for code the recompiler did not cover (RAM code, data
 * driven jumps nobody exercised during profiling, etc.).
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_internal.h"

#include <stddef.h>

/* Offsets used by gbl_core_asm.S and gbl_interp_asm.S. */
_Static_assert(offsetof(gbl_cpu_t, sp) == 6 && offsetof(gbl_cpu_t, a) == 10 &&
               offsetof(gbl_cpu_t, zr) == 11 && offsetof(gbl_cpu_t, cf) == 12 &&
               offsetof(gbl_cpu_t, hf) == 13 && offsetof(gbl_cpu_t, nf) == 14 &&
               offsetof(gbl_cpu_t, bc) == 0 && offsetof(gbl_cpu_t, de) == 2 &&
               offsetof(gbl_cpu_t, hl) == 4 && offsetof(gbl_cpu_t, pc) == 8 &&
               offsetof(gbl_cpu_t, ime) == 15 && offsetof(gbl_cpu_t, cyc) == 20 &&
               offsetof(gbl_cpu_t, next_evt) == 24,
               "gbl_cpu_t layout");

/* ---- flag helpers --------------------------------------------------------- */

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
void gbl_add(uint8_t v) {
    uint32_t a = gbr.a, r = a + v;
    gbr.hf = ((a & 0xF) + (v & 0xF)) > 0xF;
    gbr.cf = r > 0xFF;
    gbr.nf = 0;
    gbr.a = gbr.zr = (uint8_t)r;
}

void gbl_adc(uint8_t v) {
    uint32_t a = gbr.a, c = gbr.cf, r = a + v + c;
    gbr.hf = ((a & 0xF) + (v & 0xF) + c) > 0xF;
    gbr.cf = r > 0xFF;
    gbr.nf = 0;
    gbr.a = gbr.zr = (uint8_t)r;
}

void gbl_sub(uint8_t v) {
    uint32_t a = gbr.a;
    gbr.hf = (a & 0xF) < (v & 0xFu);
    gbr.cf = a < v;
    gbr.nf = 1;
    gbr.a = gbr.zr = (uint8_t)(a - v);
}

void gbl_sbc(uint8_t v) {
    uint32_t a = gbr.a, c = gbr.cf;
    gbr.hf = (a & 0xF) < (v & 0xFu) + c;
    gbr.cf = a < v + c;
    gbr.nf = 1;
    gbr.a = gbr.zr = (uint8_t)(a - v - c);
}

void gbl_cp(uint8_t v) {
    uint32_t a = gbr.a;
    gbr.hf = (a & 0xF) < (v & 0xFu);
    gbr.cf = a < v;
    gbr.nf = 1;
    gbr.zr = (uint8_t)(a - v);
}

uint8_t gbl_inc(uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    gbr.hf = (v & 0xF) == 0xF;
    gbr.nf = 0;
    gbr.zr = r;
    return r;
}

uint8_t gbl_dec(uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    gbr.hf = (v & 0xF) == 0;
    gbr.nf = 1;
    gbr.zr = r;
    return r;
}
#endif

void gbl_addhl(uint16_t v) {
    uint32_t hl = gbr.hl, r = hl + v;
    gbr.hf = ((hl & 0xFFF) + (v & 0xFFFu)) > 0xFFF;
    gbr.cf = r > 0xFFFF;
    gbr.nf = 0;
    gbr.hl = (uint16_t)r;
}

uint16_t gbl_spofs(uint8_t v) {
    uint32_t sp = gbr.sp;
    gbr.hf = ((sp & 0xF) + (v & 0xFu)) > 0xF;
    gbr.cf = ((sp & 0xFF) + v) > 0xFF;
    gbr.nf = 0;
    gbr.zr = 1;
    return (uint16_t)(sp + (int8_t)v);
}

void gbl_daa(void) {
    uint32_t a = gbr.a;
    if (!gbr.nf) {
        if (gbr.cf || a > 0x99) { a += 0x60; gbr.cf = 1; }
        if (gbr.hf || (a & 0x0F) > 0x09) a += 0x06;
    } else {
        if (gbr.cf) a -= 0x60;
        if (gbr.hf) a -= 0x06;
    }
    gbr.hf = 0;
    gbr.a = gbr.zr = (uint8_t)a;
}

uint8_t gbl_cbop(uint8_t op, uint8_t v) {
    uint32_t r, c;
    switch ((op >> 3) & 7) {
    case 0: c = v >> 7; r = (uint32_t)(v << 1) | c; break;             /* RLC */
    case 1: c = v & 1; r = (uint32_t)(v >> 1) | (c << 7); break;       /* RRC */
    case 2: c = v >> 7; r = (uint32_t)(v << 1) | gbr.cf; break;        /* RL */
    case 3: c = v & 1; r = (uint32_t)(v >> 1) | ((uint32_t)gbr.cf << 7); break; /* RR */
    case 4: c = v >> 7; r = (uint32_t)v << 1; break;                   /* SLA */
    case 5: c = v & 1; r = (uint32_t)(v >> 1) | (v & 0x80u); break;    /* SRA */
    case 6: c = 0; r = (uint32_t)(v >> 4) | ((uint32_t)v << 4); break; /* SWAP */
    default: c = v & 1; r = (uint32_t)v >> 1; break;                   /* SRL */
    }
    gbr.cf = (uint8_t)c;
    gbr.hf = 0;
    gbr.nf = 0;
    gbr.zr = (uint8_t)r;
    return (uint8_t)r;
}

uint8_t gbl_getf(void) {
    return (uint8_t)((gbr.zr ? 0 : 0x80) | (gbr.nf << 6) | (gbr.hf << 5) | (gbr.cf << 4));
}

void gbl_setf(uint8_t f) {
    gbr.zr = (uint8_t)!(f & 0x80);
    gbr.nf = (f >> 6) & 1;
    gbr.hf = (f >> 5) & 1;
    gbr.cf = (f >> 4) & 1;
}

/* ---- interpreter ---------------------------------------------------------- */

/* The SM83 cycle table lives in gbl_cycles.h, shared with the recompiler's
 * emitter; here it also serves the assembly interpreter (gbl_interp_asm.S). */
#include "gbl_cycles.h"
#define cyc_main gbl_cyc_main

static uint8_t *const regp[8] = {&gbr.b, &gbr.c, &gbr.d, &gbr.e, &gbr.h, &gbr.l, 0, &gbr.a};

static inline uint8_t get_r(uint32_t i) { return i == 6 ? gbl_rd(gbr.hl) : *regp[i]; }
static inline void set_r(uint32_t i, uint8_t v) { if (i == 6) gbl_wr(gbr.hl, v); else *regp[i] = v; }

static inline int cond(uint32_t op) {
    switch ((op >> 3) & 3) {
    case 0: return gbr.zr != 0;
    case 1: return gbr.zr == 0;
    case 2: return !gbr.cf;
    default: return gbr.cf;
    }
}

static inline uint16_t rr_get(uint32_t i) {
    switch (i) { case 0: return gbr.bc; case 1: return gbr.de; case 2: return gbr.hl; default: return gbr.sp; }
}
static inline void rr_set(uint32_t i, uint16_t v) {
    switch (i) { case 0: gbr.bc = v; break; case 1: gbr.de = v; break; case 2: gbr.hl = v; break; default: gbr.sp = v; break; }
}

static inline void alu(uint32_t op, uint8_t v) {
    switch (op & 7) {
    case 0: gbl_add(v); break;
    case 1: gbl_adc(v); break;
    case 2: gbl_sub(v); break;
    case 3: gbl_sbc(v); break;
    case 4: gbr.a &= v; gbr.zr = gbr.a; gbr.nf = 0; gbr.hf = 1; gbr.cf = 0; break;
    case 5: gbr.a ^= v; gbr.zr = gbr.a; gbr.nf = 0; gbr.hf = 0; gbr.cf = 0; break;
    case 6: gbr.a |= v; gbr.zr = gbr.a; gbr.nf = 0; gbr.hf = 0; gbr.cf = 0; break;
    default: gbl_cp(v); break;
    }
}

/* One instruction whose opcode and operands were fetched and whose base
 * cycles were counted (gbr.pc is past the opcode).  Returns bit 0 when
 * control was transferred, bit 1 after EI.  The assembly interpreter
 * (gbl_interp_asm.S) runs the common opcodes itself and calls
 * gbl_interp_op for the others. */
static inline __attribute__((always_inline)) uint32_t exec_op(uint32_t op, uint8_t n8, uint16_t n16,
                                                              uint16_t pc) {
    int jump = 0, ei = 0;

#if !GBL_INTERP_ASM
    if (op >= 0x40 && op < 0x80 && op != 0x76) {
        set_r((op >> 3) & 7, get_r(op & 7));
        return 0;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu(op >> 3, get_r(op & 7));
        return 0;
    }
#endif
    switch (op) {
#if !GBL_INTERP_ASM
    /* The opcodes the assembly interpreter runs itself (gbl_interp_asm.S). */
    case 0x00: break;
    case 0x01: case 0x11: case 0x21: case 0x31: rr_set(op >> 4, n16); gbr.pc += 2; break;
    case 0x02: gbl_wr(gbr.bc, gbr.a); break;
    case 0x12: gbl_wr(gbr.de, gbr.a); break;
    case 0x22: gbl_wr(gbr.hl, gbr.a); gbr.hl++; break;
    case 0x32: gbl_wr(gbr.hl, gbr.a); gbr.hl--; break;
    case 0x0A: gbr.a = gbl_rd(gbr.bc); break;
    case 0x1A: gbr.a = gbl_rd(gbr.de); break;
    case 0x2A: gbr.a = gbl_rd(gbr.hl); gbr.hl++; break;
    case 0x3A: gbr.a = gbl_rd(gbr.hl); gbr.hl--; break;
    case 0x03: case 0x13: case 0x23: case 0x33: rr_set(op >> 4, (uint16_t)(rr_get(op >> 4) + 1)); break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: rr_set(op >> 4, (uint16_t)(rr_get(op >> 4) - 1)); break;
    case 0x09: case 0x19: case 0x29: case 0x39: gbl_addhl(rr_get(op >> 4)); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        set_r(op >> 3, gbl_inc(get_r(op >> 3))); break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        set_r(op >> 3, gbl_dec(get_r(op >> 3))); break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        set_r(op >> 3, n8); gbr.pc++; break;
    case 0x07: gbr.a = gbl_cbop(0x00, gbr.a); gbr.zr = 1; break;
    case 0x0F: gbr.a = gbl_cbop(0x08, gbr.a); gbr.zr = 1; break;
    case 0x17: gbr.a = gbl_cbop(0x10, gbr.a); gbr.zr = 1; break;
    case 0x1F: gbr.a = gbl_cbop(0x18, gbr.a); gbr.zr = 1; break;
    case 0x18: gbr.pc = (uint16_t)(gbr.pc + 1 + (int8_t)n8); jump = 1; break;
    case 0x2F: gbr.a = (uint8_t)~gbr.a; gbr.nf = 1; gbr.hf = 1; break;
    case 0x37: gbr.cf = 1; gbr.nf = 0; gbr.hf = 0; break;
    case 0x3F: gbr.cf ^= 1; gbr.nf = 0; gbr.hf = 0; break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        if (cond(op)) { gbr.pc = gbl_pop(); gbr.cyc += 12; jump = 1; }
        break;
    case 0xC9: gbr.pc = gbl_pop(); jump = 1; break;
    case 0xC1: gbr.bc = gbl_pop(); break;
    case 0xD1: gbr.de = gbl_pop(); break;
    case 0xE1: gbr.hl = gbl_pop(); break;
    case 0xC5: gbl_push(gbr.bc); break;
    case 0xD5: gbl_push(gbr.de); break;
    case 0xE5: gbl_push(gbr.hl); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        gbr.pc += 2;
        if (cond(op)) { gbr.pc = n16; gbr.cyc += 4; jump = 1; }
        break;
    case 0xC3: gbr.pc = n16; jump = 1; break;
    case 0xE9: gbr.pc = gbr.hl; jump = 1; break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        gbr.pc += 2;
        if (cond(op)) { gbl_push(gbr.pc); gbr.pc = n16; gbr.cyc += 12; jump = 1; }
        break;
    case 0xCD: gbr.pc += 2; gbl_push(gbr.pc); gbr.pc = n16; jump = 1; break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        gbl_push(gbr.pc); gbr.pc = (uint16_t)(op & 0x38); jump = 1; break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(op >> 3, n8); gbr.pc++; break;
    case 0xE0: gbl_wr((uint16_t)(0xFF00 | n8), gbr.a); gbr.pc++; break;
    case 0xF0: gbr.a = gbl_rd((uint16_t)(0xFF00 | n8)); gbr.pc++; break;
    case 0xE2: gbl_wr((uint16_t)(0xFF00 | gbr.c), gbr.a); break;
    case 0xF2: gbr.a = gbl_rd((uint16_t)(0xFF00 | gbr.c)); break;
    case 0xEA: gbl_wr(n16, gbr.a); gbr.pc += 2; break;
    case 0xFA: gbr.a = gbl_rd(n16); gbr.pc += 2; break;
    case 0xF9: gbr.sp = gbr.hl; break;
    case 0xF3: gbr.ime = 0; break;
    case 0xCB: {
        uint32_t r = n8 & 7, grp = n8 >> 6, bit = (n8 >> 3) & 7;
        gbr.pc++;
        gbr.cyc += (r == 6) ? (grp == 1 ? 8 : 12) : 4;
        uint8_t v = get_r(r);
        if (grp == 0) set_r(r, gbl_cbop((uint8_t)n8, v));
        else if (grp == 1) { gbr.zr = v & (1u << bit); gbr.nf = 0; gbr.hf = 1; }
        else if (grp == 2) set_r(r, (uint8_t)(v & ~(1u << bit)));
        else set_r(r, (uint8_t)(v | (1u << bit)));
        break;
    }
#endif
    /* Always here (the assembly sends these to gbl_interp_op). */
    case 0x76: gbl_halt_wait(); jump = 1; break;
    case 0x08: gbl_wr(n16, (uint8_t)gbr.sp); gbl_wr((uint16_t)(n16 + 1), (uint8_t)(gbr.sp >> 8)); gbr.pc += 2; break;
    case 0x10: gbr.pc++; break; /* STOP: treated as a two-byte NOP */
    case 0x20: case 0x28: case 0x30: case 0x38: /* the assembly: JR NZ,-3 only */
        gbr.pc++;
        if (cond(op)) {
            gbr.pc = (uint16_t)(gbr.pc + (int8_t)n8);
            gbr.cyc += 4;
            jump = 1;
#if GBL_INTERP_DELAY_LOOP && !defined(GBL_COVERAGE) && !defined(GBL_TRACE)
            /* DEC A / JR NZ,-3: skip whole 16-cycle iterations while no
             * instruction of them would start at or after next_evt (the
             * last one to check is the JR, 4 cycles in: measure from
             * there). */
            if (op == 0x20 && n8 == 0xFD && gbl_rd(gbr.pc) == 0x3D) {
                gbr.cyc += 4;
                uint32_t k = gbl_spin_n(16, gbr.a - 1u);
                gbr.cyc -= 4;
                if (k) {
                    /* Flags as the last skipped DEC A left them. */
                    gbr.a = (uint8_t)(gbr.a - k);
                    gbr.zr = gbr.a;
                    gbr.hf = (gbr.a & 0x0F) == 0x0F;
                }
            }
#endif
        }
        break;
    case 0x27: gbl_daa(); break;
    case 0xD9:
        gbr.pc = gbl_pop();
#if GBL_DEBUG_RESTART
        gbl_dbg_reti();
#endif
        gbr.ime = 1;
        gbl_update_evt();
        jump = 1;
        break;
    case 0xF1: { uint16_t v = gbl_pop(); gbr.a = (uint8_t)(v >> 8); gbl_setf((uint8_t)v); break; }
    case 0xF5: gbl_push((uint16_t)((gbr.a << 8) | gbl_getf())); break;
    case 0xE8: gbr.sp = gbl_spofs(n8); gbr.pc++; break;
    case 0xF8: gbr.hl = gbl_spofs(n8); gbr.pc++; break;
    case 0xFB: gbr.ime = 1; gbl_update_evt(); ei = 1; break;
    default:
        /* Undefined opcode: the DMG locks up.  Spin in place so the
         * frontend keeps running. */
        gbr.pc = pc;
#if GBL_DEBUG_RESTART
        if (!gbl_dbg_restart.lock_frame) {
            gbl_dbg_restart.lock_frame = gbl_frame | 1u;
            gbl_dbg_restart.lock_pc = pc;
            gbl_dbg_restart.lock_bank = gbr.rom_bank;
        }
#endif
        gbl_halt_wait();
        jump = 1;
        break;
    }
    return (uint32_t)jump | (uint32_t)ei << 1;
}

#if GBL_INTERP_ASM
uint32_t gbl_interp_op(uint32_t op, uint32_t n16) {
    return exec_op(op, (uint8_t)n16, (uint16_t)n16, (uint16_t)(gbr.pc - 1u));
}
#else
#ifdef GBL_COVERAGE
static uint16_t cov_next = 0xFFFF;
#endif

/* Execute until a control transfer or an interrupt entry, then return to
 * the dispatcher so compiled code can take over as soon as possible. */
void gbl_interp_block(void) {
    int ei_delay = 0;
    for (;;) {
        /* EI takes effect after the following instruction. */
        if (!ei_delay && (int32_t)(gbr.cyc - gbr.next_evt) >= 0) {
            gbl_sync();
            if (gbl_irq_enter()) return;
        }
        ei_delay = 0;
        uint16_t pc = gbr.pc;
#ifdef GBL_COVERAGE
        if (pc < 0x8000) gbl_coverage_hit(pc < 0x4000 ? 0 : gbr.rom_bank, pc, pc != cov_next);
#endif
#ifdef GBL_TRACE
        gbl_trace_hook(pc);
#endif
#ifdef GBL_ROM_TRACE
        extern uint8_t gbl_rom_fetch; /* opcode fetches are not data reads */
        gbl_rom_fetch = 1;
#endif
        uint32_t op;
        uint8_t n8;
        uint16_t n16;
        const uint8_t *page = gbl_rmap[pc >> 12];
        if (page && (pc & 0xFFFu) < 0xFFEu) {
            /* Opcode and operands in one mapped page: read them directly. */
            op = gbl_load_u8(page + pc);
            n8 = gbl_load_u8(page + pc + 1u);
            n16 = (uint16_t)(n8 | (gbl_load_u8(page + pc + 2u) << 8));
        } else {
            op = gbl_rd(pc);
            n8 = gbl_rd((uint16_t)(pc + 1));
            n16 = (uint16_t)(n8 | (gbl_rd((uint16_t)(pc + 2)) << 8));
        }
#ifdef GBL_ROM_TRACE
        gbl_rom_fetch = 0;
#endif
        gbr.pc = (uint16_t)(pc + 1);
        gbl_interp_instrs++;
        uint32_t flags;
        if (gbl_acc_last[op]) {
            /* single data access in the last M-cycle (gbl_cycles.h) */
            gbr.cyc += cyc_main[op] - 4u;
            flags = exec_op(op, n8, n16, pc);
            gbr.cyc += 4u;
        } else {
            gbr.cyc += cyc_main[op];
            flags = exec_op(op, n8, n16, pc);
        }
        int jump = (int)(flags & 1u);
        if (flags & 2u) ei_delay = 1;
#ifdef GBL_COVERAGE
        cov_next = gbr.pc;
        if (jump) cov_next = 0xFFFF;
#endif
        if (jump) return;
    }
}
#endif /* GBL_INTERP_ASM */
