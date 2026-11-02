/* SM83 base T-cycles per opcode; conditional control flow lists the
 * not-taken cost.  One source for the interpreter (gbl_cpu.c, which also
 * exports it for gbl_interp_asm.S) and the recompiler's emitter
 * (recompiler/src/codegen/meta_emitter.cpp): the two must agree, a
 * differing table would desynchronise compiled code from the interpreter
 * on every cycle-dependent write. */
#ifndef GBL_CYCLES_H
#define GBL_CYCLES_H
#include <stdint.h>
const uint8_t gbl_cyc_main[256] = {
     4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,
     4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8,12,12,16,12,16, 8,16, 8,16,12, 4,12,24, 8,16,
     8,12,12, 4,12,16, 8,16, 8,16,12, 4,12, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,16, 4,16, 4, 4, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,12, 8,16, 4, 4, 4, 8,16,
};
/* Opcodes whose only data access (the write, for INC/DEC (HL)) falls in
 * their last M-cycle: the bus sees it at the start of that M-cycle, 4
 * cycles before the end of the instruction (SameBoy and Gambatte agree:
 * LDH A,(n) reads at +8 of 12).  The interpreter charges cycles - 4 before
 * the access and 4 after; the precise emitter does the same. */
const uint8_t gbl_acc_last[256] = {
    [0x02] = 1, [0x12] = 1, [0x0A] = 1, [0x1A] = 1, [0x22] = 1, [0x2A] = 1, [0x32] = 1, [0x3A] = 1,
    [0x34] = 1, [0x35] = 1, [0x36] = 1,
    [0x46] = 1, [0x4E] = 1, [0x56] = 1, [0x5E] = 1, [0x66] = 1, [0x6E] = 1, [0x7E] = 1,
    [0x70] = 1, [0x71] = 1, [0x72] = 1, [0x73] = 1, [0x74] = 1, [0x75] = 1, [0x77] = 1,
    [0x86] = 1, [0x8E] = 1, [0x96] = 1, [0x9E] = 1, [0xA6] = 1, [0xAE] = 1, [0xB6] = 1, [0xBE] = 1,
    [0xE0] = 1, [0xF0] = 1, [0xE2] = 1, [0xF2] = 1, [0xEA] = 1, [0xFA] = 1,
};
#endif
