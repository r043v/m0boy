#ifndef M0BOY_GP32_BLIT_H
#define M0BOY_GP32_BLIT_H

/* Lines converted, summed and sent at once. */
#define GP32_BLIT_LINES 8

#ifndef __ASSEMBLER__
#include <stdint.h>

/* One 8 bit GP32 LCD line to a row of the panel in its own orientation:
   240 pixels, out[c] = colors[in[c ^ turn]].  `in` and `out` are aligned on
   four bytes, turn is 0-3. */
void gp32_line8(uint16_t *out, const uint8_t *in, const uint16_t *colors, unsigned turn);

/* The routines below use the vector (PIE) instructions: every buffer is aligned
   on sixteen bytes.  Task context only. */

/* One 16 bit GP32 LCD line (5:5:5 and a shared low bit) to RGB565: 240 pixels,
   out[c] = rgb565(in[c ^ swap]), swap 0 or 1.  `constants` is
   {0xffc0, 0x001f, 0x0001}. */
void gp32_line16(uint16_t *out, const uint16_t *in, unsigned swap, const uint16_t *constants);

/* A checksum of GP32_BLIT_LINES lines of 240 halfwords.  Each line is summed as signed
   products with `weights` (240 values), in the 40 bit accumulator; the lines are
   then chained: h = (h * P + low word) * P + high byte, P = 0x01000193. */
uint32_t gp32_hash(const uint16_t *lines, const int16_t *weights);

/* The 256 palette registers (a 16 bit GP32 colour in the low half of each word)
   to RGB565.  `constants` as for gp32_line16. */
void gp32_palette(uint16_t *colors, const uint32_t *palette, const uint16_t *constants);

/* Copies `blocks` times 64 bytes. */
void gp32_copy64(void *to, const void *from, unsigned blocks);

#endif /* __ASSEMBLER__ */

#endif
