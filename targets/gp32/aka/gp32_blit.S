/* The routines of gp32_blit.h for the ESP32-S3.  Their sizes are fixed; each is
   unrolled where that removes work or a load's wait inside its zero overhead
   loop (such a loop has no branch cost to save).  Kept in internal memory. */
#ifdef __XTENSA__
#include "gp32_blit.h"
    .section .iram1.gp32_line8, "ax"

/* Four pixels of the source word \word, stored at offset \at. */
    .macro  quad turn, word, at
    extui   a9, \word, ((0 ^ \turn) * 8), 8
    extui   a10, \word, ((1 ^ \turn) * 8), 8
    extui   a11, \word, ((2 ^ \turn) * 8), 8
    extui   a12, \word, ((3 ^ \turn) * 8), 8
    addx2   a9, a9, a4
    addx2   a10, a10, a4
    addx2   a11, a11, a4
    addx2   a12, a12, a4
    l16ui   a9, a9, 0
    l16ui   a10, a10, 0
    l16ui   a11, a11, 0
    l16ui   a12, a12, 0
    slli    a10, a10, 16
    slli    a12, a12, 16
    or      a9, a9, a10
    or      a11, a11, a12
    s32i    a9, a2, \at
    s32i    a11, a2, \at + 4
    .endm

    .macro  turned turn
.Lturn\turn:
    loop    a6, .Lend\turn
    l32i    a8, a3, 0
    l32i    a7, a3, 4
    addi    a3, a3, 8
    quad    \turn, a8, 0
    quad    \turn, a7, 8
    addi    a2, a2, 16
.Lend\turn:
    retw.n
    .endm

    .align  4
    .global gp32_line8
    .type   gp32_line8, @function
gp32_line8:
    entry   a1, 32
    movi    a6, 30              /* eight pixels per turn */
    beqi    a5, 1, .Lgo1
    beqi    a5, 2, .Lgo2
    beqi    a5, 3, .Lgo3
    j       .Lturn0
.Lgo1:
    j       .Lturn1
.Lgo2:
    j       .Lturn2
.Lgo3:
    j       .Lturn3
    .align  4
    turned  0
    .align  4
    turned  1
    .align  4
    turned  2
    .align  4
    turned  3
    .size   gp32_line8, . - gp32_line8

/* Eight pixels of \in to RGB565: (c & 0xffc0) | (c >> 1 & 0x1f) | (c & 1) << 5.
   q5, q6 and q7 hold the three constants. */
    .macro  convert in
    ssai    1
    ee.vsr.32   q2, \in
    ee.andq     q2, q2, q6
    ee.andq     q3, \in, q7
    ssai    5
    ee.vsl.32   q3, q3
    ee.andq     q4, \in, q5
    ee.orq      q4, q4, q2
    ee.orq      q4, q4, q3
    ee.vst.128.ip   q4, a2, 16
    .endm

    .section .iram1.gp32_line16, "ax"
    .align  4
    .global gp32_line16
    .type   gp32_line16, @function
gp32_line16:
    entry   a1, 32
    ee.vldbc.16.ip  q5, a5, 2
    ee.vldbc.16.ip  q6, a5, 2
    ee.vldbc.16.ip  q7, a5, 2
    movi    a6, 15              /* sixteen pixels per turn */
    bnez    a4, .Lswapped
    loop    a6, .Lstraight_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a3, 16
    convert q0
    convert q1
.Lstraight_end:
    retw.n
    .align  4
.Lswapped:
    loop    a6, .Lswapped_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vunzip.16    q0, q1      /* q0: even halfwords, q1: odd ones */
    ee.vzip.16      q1, q0      /* q1 then q0: the pairs exchanged */
    convert q1
    convert q0
.Lswapped_end:
    retw.n
    .size   gp32_line16, . - gp32_line16

/* One line: thirty vectors.  A multiply-accumulate also loads the weights of a
   later vector, so nothing waits: two instructions per eight pixels.  Three
   pairs of registers take turns; two are filled before the loop. */
    .section .iram1.gp32_hash, "ax"
    .align  4
    .global gp32_hash
    .type   gp32_hash, @function
gp32_hash:
    entry   a1, 32
    movi    a4, 0               /* the checksum */
    movi    a5, 0x01000193
    movi    a6, GP32_BLIT_LINES
.Lhash_line:
    mov     a7, a3              /* the weights */
    ee.zero.accx
    ee.vld.128.ip   q0, a2, 16
    ee.vld.128.ip   q1, a7, 16
    ee.vld.128.ip   q2, a2, 16
    ee.vld.128.ip   q3, a7, 16
    movi    a8, 9
    loop    a8, .Lhash_end
    ee.vld.128.ip   q4, a2, 16
    ee.vmulas.s16.accx.ld.ip    q5, a7, 16, q0, q1
    ee.vld.128.ip   q0, a2, 16
    ee.vmulas.s16.accx.ld.ip    q1, a7, 16, q2, q3
    ee.vld.128.ip   q2, a2, 16
    ee.vmulas.s16.accx.ld.ip    q3, a7, 16, q4, q5
.Lhash_end:
    ee.vld.128.ip   q4, a2, 16
    ee.vmulas.s16.accx.ld.ip    q5, a7, 16, q0, q1
    ee.vmulas.s16.accx  q2, q3
    ee.vmulas.s16.accx  q4, q5
    rur.accx_0  a8
    rur.accx_1  a9
    extui   a9, a9, 0, 8
    mull    a4, a4, a5
    add     a4, a4, a8
    mull    a4, a4, a5
    add     a4, a4, a9
    addi    a6, a6, -1
    bnez    a6, .Lhash_line
    mov     a2, a4
    retw.n
    .size   gp32_hash, . - gp32_hash

    .section .iram1.gp32_palette, "ax"
    .align  4
    .global gp32_palette
    .type   gp32_palette, @function
gp32_palette:
    entry   a1, 32
    ee.vldbc.16.ip  q5, a4, 2
    ee.vldbc.16.ip  q6, a4, 2
    ee.vldbc.16.ip  q7, a4, 2
    movi    a6, 32              /* eight colours per turn */
    loop    a6, .Lpalette_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vunzip.16    q0, q1      /* q0: the eight low halves */
    convert q0
.Lpalette_end:
    retw.n
    .size   gp32_palette, . - gp32_palette

    .section .iram1.gp32_copy64, "ax"
    .align  4
    .global gp32_copy64
    .type   gp32_copy64, @function
gp32_copy64:
    entry   a1, 32
    loop    a4, .Lcopy_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vld.128.ip   q2, a3, 16
    ee.vld.128.ip   q3, a3, 16
    ee.vst.128.ip   q0, a2, 16
    ee.vst.128.ip   q1, a2, 16
    ee.vst.128.ip   q2, a2, 16
    ee.vst.128.ip   q3, a2, 16
.Lcopy_end:
    retw.n
    .size   gp32_copy64, . - gp32_copy64
#endif
