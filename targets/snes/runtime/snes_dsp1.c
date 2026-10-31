/* The DSP-1 coprocessor: the command protocol on the cartridge bus (the
 * data register DR and the status register SR share the mapped window,
 * even byte DR, odd byte SR) and the math commands computed directly.
 * The commands and their constants follow the published reverse-
 * engineering of the chip (Overload, The Dumper, Neviksti, Andreas Naive,
 * 2006): the data ROM below is the chip's own constant store as logged
 * from a DSP-1B — published reverse-engineered constants, code and not
 * cartridge data. */

#include "snes.h"
#include <string.h>

typedef void (*dsp1_fn)(int16_t *in, int16_t *out);
typedef struct {
    dsp1_fn fn;
    uint16_t reads, writes;
} dsp1_cmd;

/* the status bits: the transfer size (DRC), the 16-bit transfer's half
 * (DRS) and the request for master (RQM) */
#define DSP1_DRC 0x04
#define DSP1_DRS 0x10
#define DSP1_RQM 0x80

enum { DSP1_WAIT, DSP1_READ, DSP1_WRITE };

#define I16(v) ((int16_t)(uint16_t)(v))


/* the chip's data ROM as logged from a DSP-1B (the tables the commands
 * consult: the two shift tables at 0x21 and 0x31, the inverse's initial
 * guesses at 0x65, the square root's at 0xd5, the sine/cosine nodes at
 * 0x116, the azimuth-clipping Taylor constants at 0x324); then the sine
 * table the sin/cos interpolate over (its own arrangement), its slope
 * table, and the zenith angle's clipping boundary per exponent */
static const int16_t dsp1_data_rom[1024] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
    0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000,
    0x4000, 0x7fff, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200,
    0x0100, 0x0080, 0x0040, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002,
    0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x8000, 0xffe5, 0x0100, 0x7fff, 0x7f02, 0x7e08,
    0x7d12, 0x7c1f, 0x7b30, 0x7a45, 0x795d, 0x7878, 0x7797, 0x76ba,
    0x75df, 0x7507, 0x7433, 0x7361, 0x7293, 0x71c7, 0x70fe, 0x7038,
    0x6f75, 0x6eb4, 0x6df6, 0x6d3a, 0x6c81, 0x6bca, 0x6b16, 0x6a64,
    0x69b4, 0x6907, 0x685b, 0x67b2, 0x670b, 0x6666, 0x65c4, 0x6523,
    0x6484, 0x63e7, 0x634c, 0x62b3, 0x621c, 0x6186, 0x60f2, 0x6060,
    0x5fd0, 0x5f41, 0x5eb5, 0x5e29, 0x5d9f, 0x5d17, 0x5c91, 0x5c0c,
    0x5b88, 0x5b06, 0x5a85, 0x5a06, 0x5988, 0x590b, 0x5890, 0x5816,
    0x579d, 0x5726, 0x56b0, 0x563b, 0x55c8, 0x5555, 0x54e4, 0x5474,
    0x5405, 0x5398, 0x532b, 0x52bf, 0x5255, 0x51ec, 0x5183, 0x511c,
    0x50b6, 0x5050, 0x4fec, 0x4f89, 0x4f26, 0x4ec5, 0x4e64, 0x4e05,
    0x4da6, 0x4d48, 0x4cec, 0x4c90, 0x4c34, 0x4bda, 0x4b81, 0x4b28,
    0x4ad0, 0x4a79, 0x4a23, 0x49cd, 0x4979, 0x4925, 0x48d1, 0x487f,
    0x482d, 0x47dc, 0x478c, 0x473c, 0x46ed, 0x469f, 0x4651, 0x4604,
    0x45b8, 0x456c, 0x4521, 0x44d7, 0x448d, 0x4444, 0x43fc, 0x43b4,
    0x436d, 0x4326, 0x42e0, 0x429a, 0x4255, 0x4211, 0x41cd, 0x4189,
    0x4146, 0x4104, 0x40c2, 0x4081, 0x4040, 0x3fff, 0x41f7, 0x43e1,
    0x45bd, 0x478d, 0x4951, 0x4b0b, 0x4cbb, 0x4e61, 0x4fff, 0x5194,
    0x5322, 0x54a9, 0x5628, 0x57a2, 0x5914, 0x5a81, 0x5be9, 0x5d4a,
    0x5ea7, 0x5fff, 0x6152, 0x62a0, 0x63ea, 0x6530, 0x6672, 0x67b0,
    0x68ea, 0x6a20, 0x6b53, 0x6c83, 0x6daf, 0x6ed9, 0x6fff, 0x7122,
    0x7242, 0x735f, 0x747a, 0x7592, 0x76a7, 0x77ba, 0x78cb, 0x79d9,
    0x7ae5, 0x7bee, 0x7cf5, 0x7dfa, 0x7efe, 0x7fff, 0x0000, 0x0324,
    0x0647, 0x096a, 0x0c8b, 0x0fab, 0x12c8, 0x15e2, 0x18f8, 0x1c0b,
    0x1f19, 0x2223, 0x2528, 0x2826, 0x2b1f, 0x2e11, 0x30fb, 0x33de,
    0x36ba, 0x398c, 0x3c56, 0x3f17, 0x41ce, 0x447a, 0x471c, 0x49b4,
    0x4c3f, 0x4ebf, 0x5133, 0x539b, 0x55f5, 0x5842, 0x5a82, 0x5cb4,
    0x5ed7, 0x60ec, 0x62f2, 0x64e8, 0x66cf, 0x68a6, 0x6a6d, 0x6c24,
    0x6dca, 0x6f5f, 0x70e2, 0x7255, 0x73b5, 0x7504, 0x7641, 0x776c,
    0x7884, 0x798a, 0x7a7d, 0x7b5d, 0x7c29, 0x7ce3, 0x7d8a, 0x7e1d,
    0x7e9d, 0x7f09, 0x7f62, 0x7fa7, 0x7fd8, 0x7ff6, 0x7fff, 0x7ff6,
    0x7fd8, 0x7fa7, 0x7f62, 0x7f09, 0x7e9d, 0x7e1d, 0x7d8a, 0x7ce3,
    0x7c29, 0x7b5d, 0x7a7d, 0x798a, 0x7884, 0x776c, 0x7641, 0x7504,
    0x73b5, 0x7255, 0x70e2, 0x6f5f, 0x6dca, 0x6c24, 0x6a6d, 0x68a6,
    0x66cf, 0x64e8, 0x62f2, 0x60ec, 0x5ed7, 0x5cb4, 0x5a82, 0x5842,
    0x55f5, 0x539b, 0x5133, 0x4ebf, 0x4c3f, 0x49b4, 0x471c, 0x447a,
    0x41ce, 0x3f17, 0x3c56, 0x398c, 0x36ba, 0x33de, 0x30fb, 0x2e11,
    0x2b1f, 0x2826, 0x2528, 0x2223, 0x1f19, 0x1c0b, 0x18f8, 0x15e2,
    0x12c8, 0x0fab, 0x0c8b, 0x096a, 0x0647, 0x0324, 0x7fff, 0x7ff6,
    0x7fd8, 0x7fa7, 0x7f62, 0x7f09, 0x7e9d, 0x7e1d, 0x7d8a, 0x7ce3,
    0x7c29, 0x7b5d, 0x7a7d, 0x798a, 0x7884, 0x776c, 0x7641, 0x7504,
    0x73b5, 0x7255, 0x70e2, 0x6f5f, 0x6dca, 0x6c24, 0x6a6d, 0x68a6,
    0x66cf, 0x64e8, 0x62f2, 0x60ec, 0x5ed7, 0x5cb4, 0x5a82, 0x5842,
    0x55f5, 0x539b, 0x5133, 0x4ebf, 0x4c3f, 0x49b4, 0x471c, 0x447a,
    0x41ce, 0x3f17, 0x3c56, 0x398c, 0x36ba, 0x33de, 0x30fb, 0x2e11,
    0x2b1f, 0x2826, 0x2528, 0x2223, 0x1f19, 0x1c0b, 0x18f8, 0x15e2,
    0x12c8, 0x0fab, 0x0c8b, 0x096a, 0x0647, 0x0324, 0x0000, 0xfcdc,
    0xf9b9, 0xf696, 0xf375, 0xf055, 0xed38, 0xea1e, 0xe708, 0xe3f5,
    0xe0e7, 0xdddd, 0xdad8, 0xd7da, 0xd4e1, 0xd1ef, 0xcf05, 0xcc22,
    0xc946, 0xc674, 0xc3aa, 0xc0e9, 0xbe32, 0xbb86, 0xb8e4, 0xb64c,
    0xb3c1, 0xb141, 0xaecd, 0xac65, 0xaa0b, 0xa7be, 0xa57e, 0xa34c,
    0xa129, 0x9f14, 0x9d0e, 0x9b18, 0x9931, 0x975a, 0x9593, 0x93dc,
    0x9236, 0x90a1, 0x8f1e, 0x8dab, 0x8c4b, 0x8afc, 0x89bf, 0x8894,
    0x877c, 0x8676, 0x8583, 0x84a3, 0x83d7, 0x831d, 0x8276, 0x81e3,
    0x8163, 0x80f7, 0x809e, 0x8059, 0x8028, 0x800a, 0x6488, 0x0080,
    0x03ff, 0x0116, 0x0002, 0x0080, 0x4000, 0x3fd7, 0x3faf, 0x3f86,
    0x3f5d, 0x3f34, 0x3f0c, 0x3ee3, 0x3eba, 0x3e91, 0x3e68, 0x3e40,
    0x3e17, 0x3dee, 0x3dc5, 0x3d9c, 0x3d74, 0x3d4b, 0x3d22, 0x3cf9,
    0x3cd0, 0x3ca7, 0x3c7f, 0x3c56, 0x3c2d, 0x3c04, 0x3bdb, 0x3bb2,
    0x3b89, 0x3b60, 0x3b37, 0x3b0e, 0x3ae5, 0x3abc, 0x3a93, 0x3a69,
    0x3a40, 0x3a17, 0x39ee, 0x39c5, 0x399c, 0x3972, 0x3949, 0x3920,
    0x38f6, 0x38cd, 0x38a4, 0x387a, 0x3851, 0x3827, 0x37fe, 0x37d4,
    0x37aa, 0x3781, 0x3757, 0x372d, 0x3704, 0x36da, 0x36b0, 0x3686,
    0x365c, 0x3632, 0x3609, 0x35df, 0x35b4, 0x358a, 0x3560, 0x3536,
    0x350c, 0x34e1, 0x34b7, 0x348d, 0x3462, 0x3438, 0x340d, 0x33e3,
    0x33b8, 0x338d, 0x3363, 0x3338, 0x330d, 0x32e2, 0x32b7, 0x328c,
    0x3261, 0x3236, 0x320b, 0x31df, 0x31b4, 0x3188, 0x315d, 0x3131,
    0x3106, 0x30da, 0x30ae, 0x3083, 0x3057, 0x302b, 0x2fff, 0x2fd2,
    0x2fa6, 0x2f7a, 0x2f4d, 0x2f21, 0x2ef4, 0x2ec8, 0x2e9b, 0x2e6e,
    0x2e41, 0x2e14, 0x2de7, 0x2dba, 0x2d8d, 0x2d60, 0x2d32, 0x2d05,
    0x2cd7, 0x2ca9, 0x2c7b, 0x2c4d, 0x2c1f, 0x2bf1, 0x2bc3, 0x2b94,
    0x2b66, 0x2b37, 0x2b09, 0x2ada, 0x2aab, 0x2a7c, 0x2a4c, 0x2a1d,
    0x29ed, 0x29be, 0x298e, 0x295e, 0x292e, 0x28fe, 0x28ce, 0x289d,
    0x286d, 0x283c, 0x280b, 0x27da, 0x27a9, 0x2777, 0x2746, 0x2714,
    0x26e2, 0x26b0, 0x267e, 0x264c, 0x2619, 0x25e7, 0x25b4, 0x2581,
    0x254d, 0x251a, 0x24e6, 0x24b2, 0x247e, 0x244a, 0x2415, 0x23e1,
    0x23ac, 0x2376, 0x2341, 0x230b, 0x22d6, 0x229f, 0x2269, 0x2232,
    0x21fc, 0x21c4, 0x218d, 0x2155, 0x211d, 0x20e5, 0x20ad, 0x2074,
    0x203b, 0x2001, 0x1fc7, 0x1f8d, 0x1f53, 0x1f18, 0x1edd, 0x1ea1,
    0x1e66, 0x1e29, 0x1ded, 0x1db0, 0x1d72, 0x1d35, 0x1cf6, 0x1cb8,
    0x1c79, 0x1c39, 0x1bf9, 0x1bb8, 0x1b77, 0x1b36, 0x1af4, 0x1ab1,
    0x1a6e, 0x1a2a, 0x19e6, 0x19a1, 0x195c, 0x1915, 0x18ce, 0x1887,
    0x183f, 0x17f5, 0x17ac, 0x1761, 0x1715, 0x16c9, 0x167c, 0x162e,
    0x15df, 0x158e, 0x153d, 0x14eb, 0x1497, 0x1442, 0x13ec, 0x1395,
    0x133c, 0x12e2, 0x1286, 0x1228, 0x11c9, 0x1167, 0x1104, 0x109e,
    0x1036, 0x0fcc, 0x0f5f, 0x0eef, 0x0e7b, 0x0e04, 0x0d89, 0x0d0a,
    0x0c86, 0x0bfd, 0x0b6d, 0x0ad6, 0x0a36, 0x098d, 0x08d7, 0x0811,
    0x0736, 0x063e, 0x0519, 0x039a, 0x0000, 0x7fff, 0x0100, 0x0080,
    0x021d, 0x00c8, 0x00ce, 0x0048, 0x0a26, 0x277a, 0x00ce, 0x6488,
    0x14ac, 0x0001, 0x00f9, 0x00fc, 0x00ff, 0x00fc, 0x00f9, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
};

static const int16_t dsp1_sin_table[256] = {
    0x0000, 0x0324, 0x0647, 0x096a, 0x0c8b, 0x0fab, 0x12c8, 0x15e2,
    0x18f8, 0x1c0b, 0x1f19, 0x2223, 0x2528, 0x2826, 0x2b1f, 0x2e11,
    0x30fb, 0x33de, 0x36ba, 0x398c, 0x3c56, 0x3f17, 0x41ce, 0x447a,
    0x471c, 0x49b4, 0x4c3f, 0x4ebf, 0x5133, 0x539b, 0x55f5, 0x5842,
    0x5a82, 0x5cb4, 0x5ed7, 0x60ec, 0x62f2, 0x64e8, 0x66cf, 0x68a6,
    0x6a6d, 0x6c24, 0x6dca, 0x6f5f, 0x70e2, 0x7255, 0x73b5, 0x7504,
    0x7641, 0x776c, 0x7884, 0x798a, 0x7a7d, 0x7b5d, 0x7c29, 0x7ce3,
    0x7d8a, 0x7e1d, 0x7e9d, 0x7f09, 0x7f62, 0x7fa7, 0x7fd8, 0x7ff6,
    0x7fff, 0x7ff6, 0x7fd8, 0x7fa7, 0x7f62, 0x7f09, 0x7e9d, 0x7e1d,
    0x7d8a, 0x7ce3, 0x7c29, 0x7b5d, 0x7a7d, 0x798a, 0x7884, 0x776c,
    0x7641, 0x7504, 0x73b5, 0x7255, 0x70e2, 0x6f5f, 0x6dca, 0x6c24,
    0x6a6d, 0x68a6, 0x66cf, 0x64e8, 0x62f2, 0x60ec, 0x5ed7, 0x5cb4,
    0x5a82, 0x5842, 0x55f5, 0x539b, 0x5133, 0x4ebf, 0x4c3f, 0x49b4,
    0x471c, 0x447a, 0x41ce, 0x3f17, 0x3c56, 0x398c, 0x36ba, 0x33de,
    0x30fb, 0x2e11, 0x2b1f, 0x2826, 0x2528, 0x2223, 0x1f19, 0x1c0b,
    0x18f8, 0x15e2, 0x12c8, 0x0fab, 0x0c8b, 0x096a, 0x0647, 0x0324,
    0x0000, 0xfcdc, 0xf9b9, 0xf696, 0xf375, 0xf055, 0xed38, 0xea1e,
    0xe708, 0xe3f5, 0xe0e7, 0xdddd, 0xdad8, 0xd7da, 0xd4e1, 0xd1ef,
    0xcf05, 0xcc22, 0xc946, 0xc674, 0xc3aa, 0xc0e9, 0xbe32, 0xbb86,
    0xb8e4, 0xb64c, 0xb3c1, 0xb141, 0xaecd, 0xac65, 0xaa0b, 0xa7be,
    0xa57e, 0xa34c, 0xa129, 0x9f14, 0x9d0e, 0x9b18, 0x9931, 0x975a,
    0x9593, 0x93dc, 0x9236, 0x90a1, 0x8f1e, 0x8dab, 0x8c4b, 0x8afc,
    0x89bf, 0x8894, 0x877c, 0x8676, 0x8583, 0x84a3, 0x83d7, 0x831d,
    0x8276, 0x81e3, 0x8163, 0x80f7, 0x809e, 0x8059, 0x8028, 0x800a,
    0x8001, 0x800a, 0x8028, 0x8059, 0x809e, 0x80f7, 0x8163, 0x81e3,
    0x8276, 0x831d, 0x83d7, 0x84a3, 0x8583, 0x8676, 0x877c, 0x8894,
    0x89bf, 0x8afc, 0x8c4b, 0x8dab, 0x8f1e, 0x90a1, 0x9236, 0x93dc,
    0x9593, 0x975a, 0x9931, 0x9b18, 0x9d0e, 0x9f14, 0xa129, 0xa34c,
    0xa57e, 0xa7be, 0xaa0b, 0xac65, 0xaecd, 0xb141, 0xb3c1, 0xb64c,
    0xb8e4, 0xbb86, 0xbe32, 0xc0e9, 0xc3aa, 0xc674, 0xc946, 0xcc22,
    0xcf05, 0xd1ef, 0xd4e1, 0xd7da, 0xdad8, 0xdddd, 0xe0e7, 0xe3f5,
    0xe708, 0xea1e, 0xed38, 0xf055, 0xf375, 0xf696, 0xf9b9, 0xfcdc,
};

static const int16_t dsp1_mul_table[256] = {
    0x0000, 0x0003, 0x0006, 0x0009, 0x000c, 0x000f, 0x0012, 0x0015,
    0x0019, 0x001c, 0x001f, 0x0022, 0x0025, 0x0028, 0x002b, 0x002f,
    0x0032, 0x0035, 0x0038, 0x003b, 0x003e, 0x0041, 0x0045, 0x0048,
    0x004b, 0x004e, 0x0051, 0x0054, 0x0057, 0x005b, 0x005e, 0x0061,
    0x0064, 0x0067, 0x006a, 0x006d, 0x0071, 0x0074, 0x0077, 0x007a,
    0x007d, 0x0080, 0x0083, 0x0087, 0x008a, 0x008d, 0x0090, 0x0093,
    0x0096, 0x0099, 0x009d, 0x00a0, 0x00a3, 0x00a6, 0x00a9, 0x00ac,
    0x00af, 0x00b3, 0x00b6, 0x00b9, 0x00bc, 0x00bf, 0x00c2, 0x00c5,
    0x00c9, 0x00cc, 0x00cf, 0x00d2, 0x00d5, 0x00d8, 0x00db, 0x00df,
    0x00e2, 0x00e5, 0x00e8, 0x00eb, 0x00ee, 0x00f1, 0x00f5, 0x00f8,
    0x00fb, 0x00fe, 0x0101, 0x0104, 0x0107, 0x010b, 0x010e, 0x0111,
    0x0114, 0x0117, 0x011a, 0x011d, 0x0121, 0x0124, 0x0127, 0x012a,
    0x012d, 0x0130, 0x0133, 0x0137, 0x013a, 0x013d, 0x0140, 0x0143,
    0x0146, 0x0149, 0x014d, 0x0150, 0x0153, 0x0156, 0x0159, 0x015c,
    0x015f, 0x0163, 0x0166, 0x0169, 0x016c, 0x016f, 0x0172, 0x0175,
    0x0178, 0x017c, 0x017f, 0x0182, 0x0185, 0x0188, 0x018b, 0x018e,
    0x0192, 0x0195, 0x0198, 0x019b, 0x019e, 0x01a1, 0x01a4, 0x01a8,
    0x01ab, 0x01ae, 0x01b1, 0x01b4, 0x01b7, 0x01ba, 0x01be, 0x01c1,
    0x01c4, 0x01c7, 0x01ca, 0x01cd, 0x01d0, 0x01d4, 0x01d7, 0x01da,
    0x01dd, 0x01e0, 0x01e3, 0x01e6, 0x01ea, 0x01ed, 0x01f0, 0x01f3,
    0x01f6, 0x01f9, 0x01fc, 0x0200, 0x0203, 0x0206, 0x0209, 0x020c,
    0x020f, 0x0212, 0x0216, 0x0219, 0x021c, 0x021f, 0x0222, 0x0225,
    0x0228, 0x022c, 0x022f, 0x0232, 0x0235, 0x0238, 0x023b, 0x023e,
    0x0242, 0x0245, 0x0248, 0x024b, 0x024e, 0x0251, 0x0254, 0x0258,
    0x025b, 0x025e, 0x0261, 0x0264, 0x0267, 0x026a, 0x026e, 0x0271,
    0x0274, 0x0277, 0x027a, 0x027d, 0x0280, 0x0284, 0x0287, 0x028a,
    0x028d, 0x0290, 0x0293, 0x0296, 0x029a, 0x029d, 0x02a0, 0x02a3,
    0x02a6, 0x02a9, 0x02ac, 0x02b0, 0x02b3, 0x02b6, 0x02b9, 0x02bc,
    0x02bf, 0x02c2, 0x02c6, 0x02c9, 0x02cc, 0x02cf, 0x02d2, 0x02d5,
    0x02d8, 0x02db, 0x02df, 0x02e2, 0x02e5, 0x02e8, 0x02eb, 0x02ee,
    0x02f1, 0x02f5, 0x02f8, 0x02fb, 0x02fe, 0x0301, 0x0304, 0x0307,
    0x030b, 0x030e, 0x0311, 0x0314, 0x0317, 0x031a, 0x031d, 0x0321,
};

static const int16_t dsp1_max_azs_exp[16] = {
    0x38b4, 0x38b7, 0x38ba, 0x38be, 0x38c0, 0x38c4, 0x38c7, 0x38ca,
    0x38ce, 0x38d0, 0x38d4, 0x38d7, 0x38da, 0x38dd, 0x38e0, 0x38e4,
};


static int16_t dsp1_sin(int16_t angle) {
    if (angle < 0) {
        if (angle == -32768) return 0;
        return (int16_t)-dsp1_sin((int16_t)(0 - angle));
    }
    int32_t s = dsp1_sin_table[angle >> 8] +
                ((int32_t)dsp1_mul_table[angle & 0xff] * dsp1_sin_table[0x40 + (angle >> 8)] >> 15);
    return s > 32767 ? (int16_t)32767 : (int16_t)s;
}

static int16_t dsp1_cos(int16_t angle) {
    if (angle < 0) {
        if (angle == -32768) return -32768;
        angle = (int16_t)(0 - angle);
    }
    int32_t s = dsp1_sin_table[0x40 + (angle >> 8)] -
                ((int32_t)dsp1_mul_table[angle & 0xff] * dsp1_sin_table[angle >> 8] >> 15);
    return s < -32768 ? (int16_t)-32767 : (int16_t)s;
}

/* the inverse: 1/(Coefficient*2^Exponent) as iCoefficient*2^iExponent,
 * the coefficient normalized between 1/2 and 1 — a table guess then two
 * Newton steps */
static void dsp1_inverse(int16_t coefficient, int16_t exponent,
                         int16_t *i_c, int16_t *i_e) {
    if (coefficient == 0) {
        *i_c = 0x7fff;
        *i_e = 0x002f;
        return;
    }
    int sign = 1;
    if (coefficient < 0) {
        if (coefficient < -32767) coefficient = -32767;
        coefficient = (int16_t)-coefficient;
        sign = -1;
    }
    while (coefficient < 0x4000) {
        coefficient = (int16_t)(coefficient << 1);
        exponent--;
    }
    if (coefficient == 0x4000 && sign == 1) {
        *i_c = 0x7fff;
    } else if (coefficient == 0x4000) {
        *i_c = (int16_t)0xc000;
        exponent--;
    } else {
        int16_t i = I16(dsp1_data_rom[((coefficient - 0x4000) >> 7) + 0x0065]);
        i = (int16_t)((i + I16((-i * I16(coefficient * i >> 15) >> 15)) << 1));
        i = (int16_t)((i + I16((-i * I16(coefficient * i >> 15) >> 15)) << 1));
        *i_c = (int16_t)(i * sign);
    }
    *i_e = (int16_t)(1 - exponent);
}

/* normalize a [-1,1) number to Coefficient*2^Exponent, |Coefficient|>=1/2 */
static void dsp1_normalize(int16_t m, int16_t *coefficient, int16_t *exponent) {
    uint16_t i = 0x4000;
    int e = 0;
    if (m < 0)
        while ((m & (int16_t)i) && i) { i >>= 1; e++; }
    else
        while (!(m & (int16_t)i) && i) { i >>= 1; e++; }
    if (e > 0)
        *coefficient = I16(m * dsp1_data_rom[0x21 + e] << 1);
    else
        *coefficient = m;
    *exponent = (int16_t)(*exponent - e);
}

/* the same for a 32-bit number (its low 15 bits carry precision too) */
static void dsp1_normalize_double(int32_t product, int16_t *coefficient, int16_t *exponent) {
    int16_t n = (int16_t)(product & 0x7fff);
    int16_t m = (int16_t)(product >> 15);
    uint16_t i = 0x4000;
    int e = 0;
    if (m < 0)
        while ((m & (int16_t)i) && i) { i >>= 1; e++; }
    else
        while (!(m & (int16_t)i) && i) { i >>= 1; e++; }
    if (e > 0) {
        *coefficient = I16(m * dsp1_data_rom[0x21 + e] << 1);
        if (e < 15)
            *coefficient = (int16_t)(*coefficient + I16(n * dsp1_data_rom[0x40 - e] >> 15));
        else {
            i = 0x4000;
            if (m < 0)
                while ((n & (int16_t)i) && i) { i >>= 1; e++; }
            else
                while (!(n & (int16_t)i) && i) { i >>= 1; e++; }
            if (e > 15)
                *coefficient = I16(n * dsp1_data_rom[0x12 + e] << 1);
            else
                *coefficient = (int16_t)(*coefficient + n);
        }
    } else
        *coefficient = m;
    *exponent = (int16_t)e;
}

/* back to a 16-bit number, clipped to the representable range */
static int16_t dsp1_denormalize_clip(int16_t c, int16_t e) {
    if (e > 0)
        return c > 0 ? (int16_t)32767 : (c < 0 ? (int16_t)-32767 : (int16_t)0);
    if (e < 0)
        return I16(c * dsp1_data_rom[0x31 + e] >> 15);
    return c;
}

static int16_t dsp1_shift_right(int16_t c, int16_t e) {
    return I16(c * dsp1_data_rom[0x31 + e] >> 15);
}

/* ---- the commands ---- */

static void dsp1_memory_test(int16_t *in, int16_t *out) {
    (void)in;
    out[0] = 0;
}

static void dsp1_memory_dump(int16_t *in, int16_t *out) {
    (void)in;
    memcpy(out, dsp1_data_rom, 1024 * sizeof(int16_t));
}

static void dsp1_memory_size(int16_t *in, int16_t *out) {
    (void)in;
    out[0] = 0x0100;
}

static void dsp1_multiply(int16_t *in, int16_t *out) {
    out[0] = I16(in[0] * in[1] >> 15);
}

static void dsp1_multiply2(int16_t *in, int16_t *out) {
    out[0] = I16(in[0] * in[1] >> 15) + 1;
}

static void dsp1_inverse_cmd(int16_t *in, int16_t *out) {
    dsp1_inverse(in[0], in[1], &out[0], &out[1]);
}

static void dsp1_triangle(int16_t *in, int16_t *out) {
    out[0] = I16(dsp1_sin(in[0]) * in[1] >> 15);
    out[1] = I16(dsp1_cos(in[0]) * in[1] >> 15);
}

static void dsp1_radius(int16_t *in, int16_t *out) {
    int32_t r = (in[0] * in[0] + in[1] * in[1] + in[2] * in[2]) << 1;
    out[0] = (int16_t)r;
    out[1] = (int16_t)(r >> 16);
}

static void dsp1_range(int16_t *in, int16_t *out) {
    out[0] = I16(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] - in[3] * in[3] >> 15);
}

static void dsp1_range2(int16_t *in, int16_t *out) {
    out[0] = I16(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] - in[3] * in[3] >> 15) + 1;
}

static void dsp1_distance(int16_t *in, int16_t *out) {
    const int32_t radius = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (radius == 0) { out[0] = 0; return; }
    int16_t c, e;
    dsp1_normalize_double(radius, &c, &e);
    if (e & 1) c = I16(c * 0x4000 >> 15);
    const int16_t pos = I16(c * 0x0040 >> 15);
    const int16_t node1 = I16(dsp1_data_rom[0x00d5 + pos]);
    const int16_t node2 = I16(dsp1_data_rom[0x00d6 + pos]);
    int16_t distance = I16((node2 - node1) * (c & 0x1ff) >> 9) + node1;
    distance >>= e >> 1;
    out[0] = distance;
}

static void dsp1_rotate(int16_t *in, int16_t *out) {
    const int16_t angle = in[0], x1 = in[1], y1 = in[2];
    out[0] = I16((y1 * dsp1_sin(angle) >> 15) + (x1 * dsp1_cos(angle) >> 15));
    out[1] = I16((y1 * dsp1_cos(angle) >> 15) - (x1 * dsp1_sin(angle) >> 15));
}

static void dsp1_polar(int16_t *in, int16_t *out) {
    int16_t az = in[0], ay = in[1], ax = in[2];
    int16_t x1 = in[3], y1 = in[4], z1 = in[5];
    int16_t x, y, z;
    /* around Z */
    x = I16((y1 * dsp1_sin(az) >> 15) + (x1 * dsp1_cos(az) >> 15));
    y = I16((y1 * dsp1_cos(az) >> 15) - (x1 * dsp1_sin(az) >> 15));
    x1 = x; y1 = y;
    /* around Y */
    z = I16((x1 * dsp1_sin(ay) >> 15) + (z1 * dsp1_cos(ay) >> 15));
    x = I16((x1 * dsp1_cos(ay) >> 15) - (z1 * dsp1_sin(ay) >> 15));
    out[0] = x; z1 = z;
    /* around X */
    y = I16((z1 * dsp1_sin(ax) >> 15) + (y1 * dsp1_cos(ax) >> 15));
    z = I16((z1 * dsp1_cos(ax) >> 15) - (y1 * dsp1_sin(ax) >> 15));
    out[1] = y;
    out[2] = z;
}

/* the attitude matrix: S/2 times the rotation (Z, then Y, then X) */
static void dsp1_attitude(int16_t *in, int which) {
    int16_t(*m)[3] = machine.dsp1.matrix[which];
    const int16_t s = (int16_t)(in[0] >> 1);
    const int16_t sin_rz = dsp1_sin(in[1]), cos_rz = dsp1_cos(in[1]);
    const int16_t sin_ry = dsp1_sin(in[2]), cos_ry = dsp1_cos(in[2]);
    const int16_t sin_rx = dsp1_sin(in[3]), cos_rx = dsp1_cos(in[3]);

    m[0][0] = I16(I16(s * cos_rz >> 15) * cos_ry >> 15);
    m[0][1] = I16(I16(I16(s * sin_rz >> 15) * cos_rx >> 15) +
                  I16(I16(I16(s * cos_rz >> 15) * sin_rx >> 15) * sin_ry >> 15));
    m[0][2] = I16(I16(I16(s * sin_rz >> 15) * sin_rx >> 15) -
                  I16(I16(I16(s * cos_rz >> 15) * cos_rx >> 15) * sin_ry >> 15));
    m[1][0] = (int16_t)-I16(I16(s * sin_rz >> 15) * cos_ry >> 15);
    m[1][1] = I16(I16(I16(s * cos_rz >> 15) * cos_rx >> 15) -
                  I16(I16(I16(s * sin_rz >> 15) * sin_rx >> 15) * sin_ry >> 15));
    m[1][2] = I16(I16(I16(s * cos_rz >> 15) * sin_rx >> 15) +
                  I16(I16(I16(s * sin_rz >> 15) * cos_rx >> 15) * sin_ry >> 15));
    m[2][0] = I16(s * sin_ry >> 15);
    m[2][1] = (int16_t)-I16(I16(s * sin_rx >> 15) * cos_ry >> 15);
    m[2][2] = I16(I16(s * cos_rx >> 15) * cos_ry >> 15);
}

static void dsp1_attitude_a(int16_t *in, int16_t *out) { (void)out; dsp1_attitude(in, 0); }
static void dsp1_attitude_b(int16_t *in, int16_t *out) { (void)out; dsp1_attitude(in, 1); }
static void dsp1_attitude_c(int16_t *in, int16_t *out) { (void)out; dsp1_attitude(in, 2); }

static void dsp1_objective(int16_t *in, int16_t *out, int which) {
    const int16_t(*m)[3] = machine.dsp1.matrix[which];
    const int16_t x = in[0], y = in[1], z = in[2];
    out[0] = I16(I16(m[0][0] * x >> 15) + I16(m[1][0] * y >> 15) + I16(m[2][0] * z >> 15));
    out[1] = I16(I16(m[0][1] * x >> 15) + I16(m[1][1] * y >> 15) + I16(m[2][1] * z >> 15));
    out[2] = I16(I16(m[0][2] * x >> 15) + I16(m[1][2] * y >> 15) + I16(m[2][2] * z >> 15));
}

static void dsp1_objective_a(int16_t *i, int16_t *o) { dsp1_objective(i, o, 0); }
static void dsp1_objective_b(int16_t *i, int16_t *o) { dsp1_objective(i, o, 1); }
static void dsp1_objective_c(int16_t *i, int16_t *o) { dsp1_objective(i, o, 2); }

static void dsp1_subjective(int16_t *in, int16_t *out, int which) {
    const int16_t(*m)[3] = machine.dsp1.matrix[which];
    const int16_t f = in[0], l = in[1], u = in[2];
    out[0] = I16(I16(m[0][0] * f >> 15) + I16(m[0][1] * l >> 15) + I16(m[0][2] * u >> 15));
    out[1] = I16(I16(m[1][0] * f >> 15) + I16(m[1][1] * l >> 15) + I16(m[1][2] * u >> 15));
    out[2] = I16(I16(m[2][0] * f >> 15) + I16(m[2][1] * l >> 15) + I16(m[2][2] * u >> 15));
}

static void dsp1_subjective_a(int16_t *i, int16_t *o) { dsp1_subjective(i, o, 0); }
static void dsp1_subjective_b(int16_t *i, int16_t *o) { dsp1_subjective(i, o, 1); }
static void dsp1_subjective_c(int16_t *i, int16_t *o) { dsp1_subjective(i, o, 2); }

static void dsp1_scalar(int16_t *in, int16_t *out, int which) {
    const int16_t(*m)[3] = machine.dsp1.matrix[which];
    out[0] = I16((in[0] * m[0][0] + in[1] * m[1][0] + in[2] * m[2][0]) >> 15);
}

static void dsp1_scalar_a(int16_t *i, int16_t *o) { dsp1_scalar(i, o, 0); }
static void dsp1_scalar_b(int16_t *i, int16_t *o) { dsp1_scalar(i, o, 1); }
static void dsp1_scalar_c(int16_t *i, int16_t *o) { dsp1_scalar(i, o, 2); }

static void dsp1_gyrate(int16_t *in, int16_t *out) {
    const int16_t az = in[0], ax = in[1], ay = in[2];
    const int16_t u = in[3], f = in[4], l = in[5];
    const int16_t sin_ay = dsp1_sin(ay), cos_ay = dsp1_cos(ay);
    int16_t c_sec, e_sec, c_sin, c, e;
    dsp1_inverse(dsp1_cos(ax), 0, &c_sec, &e_sec);

    /* around Z */
    dsp1_normalize_double(u * cos_ay - f * sin_ay, &c, &e);
    e = (int16_t)(e_sec - e);
    dsp1_normalize(I16(c * c_sec >> 15), &c, &e);
    out[0] = (int16_t)(az + dsp1_denormalize_clip(c, e));

    /* around X */
    out[1] = (int16_t)(ax + I16(u * sin_ay >> 15) + I16(f * cos_ay >> 15));

    /* around Y */
    dsp1_normalize_double(u * cos_ay + f * sin_ay, &c, &e);
    e = (int16_t)(e_sec - e);
    dsp1_normalize(dsp1_sin(ax), &c_sin, &e);
    dsp1_normalize(I16(-(c * I16(c_sec * c_sin >> 15) >> 15)), &c, &e);
    out[2] = (int16_t)(ay + dsp1_denormalize_clip(c, e) + l);
}

/* the projection framework: from the base point, the viewpoint distance
 * (Lfe), the screen distance (Les) and the azimuth/zenith angles */
static void dsp1_parameter(int16_t *in, int16_t *out) {
    snes_dsp1_t *s = &machine.dsp1;
    const int16_t fx = in[0], fy = in[1], fz = in[2];
    const int16_t lfe = in[3], les = in[4], aas = in[5], azs = in[6];
    int16_t c, e;
    int16_t azs_clip = azs;

    s->les = les;
    s->e_les = 0;
    dsp1_normalize(les, &s->c_les, &s->e_les);

    s->sin_aas = dsp1_sin(aas);
    s->cos_aas = dsp1_cos(aas);
    s->sin_azs = dsp1_sin(azs);
    s->cos_azs = dsp1_cos(azs);

    /* the screen's normal, horizontal and vertical vectors (norm 1) */
    s->nx = I16(s->sin_azs * (int16_t)(0 - s->sin_aas) >> 15);
    s->ny = I16(s->sin_azs * s->cos_aas >> 15);
    s->nz = I16(s->cos_azs * 0x7fff >> 15);
    s->hx = I16(s->cos_aas * 0x7fff >> 15);
    s->hy = I16(s->sin_aas * 0x7fff >> 15);
    s->vx = I16(s->cos_azs * (int16_t)(0 - s->sin_aas) >> 15);
    s->vy = I16(s->cos_azs * s->cos_aas >> 15);
    s->vz = (int16_t)-I16(s->sin_azs * 0x7fff >> 15);

    /* the center of projection, then the screen's center */
    s->centre_x = (int16_t)(fx + I16(lfe * s->nx >> 15));
    s->centre_y = (int16_t)(fy + I16(lfe * s->ny >> 15));
    s->centre_z = (int16_t)(fz + I16(lfe * s->nz >> 15));
    s->gx = (int16_t)(s->centre_x - I16(les * s->nx >> 15));
    s->gy = (int16_t)(s->centre_y - I16(les * s->ny >> 15));
    s->gz = (int16_t)(s->centre_z - I16(les * s->nz >> 15));

    e = 0;
    dsp1_normalize(s->centre_z, &c, &e);
    s->centre_z_c = c;
    s->centre_z_e = e;

    /* the zenith angle clips when the centre gets too close to 0 */
    const int16_t max_azs = I16(dsp1_max_azs_exp[e < 0 ? -e : 0]);
    if (azs_clip < 0) {
        const int16_t neg = (int16_t)(0 - max_azs);
        if (azs_clip < neg + 1) azs_clip = (int16_t)(neg + 1);
    } else if (azs_clip > max_azs) {
        azs_clip = max_azs;
    }
    s->sin_azsc = dsp1_sin(azs_clip);
    s->cos_azsc = dsp1_cos(azs_clip);

    /* (Cx, Cy): the projection's centre slid by CentreZ*tg(AZS) */
    dsp1_inverse(s->cos_azsc, 0, &s->sec_azs_c1, &s->sec_azs_e1);
    dsp1_normalize(I16(c * s->sec_azs_c1 >> 15), &c, &e);
    e = (int16_t)(e + s->sec_azs_e1);
    c = I16(dsp1_denormalize_clip(c, e) * s->sin_azsc >> 15);
    s->centre_x = (int16_t)(s->centre_x + I16(c * s->sin_aas >> 15));
    s->centre_y = (int16_t)(s->centre_y - I16(c * s->cos_aas >> 15));
    out[2] = s->centre_x;
    out[3] = s->centre_y;

    /* the imaginary centre's line, and the horizon line */
    out[0] = 0;
    if (azs != azs_clip || azs == max_azs) {
        int16_t azs_corr = azs == -32768 ? (int16_t)-32767 : azs;
        int16_t d = (int16_t)(azs_corr - max_azs);
        if (d >= 0) d--;
        const int16_t aux = (int16_t)~(uint16_t)(d << 2);
        c = I16(aux * I16(dsp1_data_rom[0x0328]) >> 15);
        c = (int16_t)((I16(c * aux >> 15) + I16(dsp1_data_rom[0x0327])));
        out[0] = (int16_t)(out[0] - I16(I16(c * aux >> 15) * les >> 15));
        c = I16(aux * aux >> 15);
        int16_t aux2 = (int16_t)(I16(c * I16(dsp1_data_rom[0x0324]) >> 15) + I16(dsp1_data_rom[0x0325]));
        s->cos_azsc = (int16_t)(s->cos_azsc + I16(I16(c * aux2 >> 15) * s->cos_azsc >> 15));
    }

    /* the screen's vertical offset over the centre's horizontal plane */
    s->v_offset = I16(les * s->cos_azsc >> 15);

    dsp1_inverse(s->sin_azsc, 0, &c, &e);
    dsp1_normalize(s->v_offset, &c, &e);
    dsp1_normalize(I16(c * c >> 15), &c, &e);
    if (c == -32768) { c >>= 1; e = (int16_t)(e + 1); }
    out[1] = dsp1_denormalize_clip((int16_t)(0 - c), e);

    dsp1_inverse(s->cos_azsc, 0, &s->sec_azs_c2, &s->sec_azs_e2);
}

/* the mode 7 matrix of one raster line (the HDMA feed) */
static void dsp1_raster(int16_t *in, int16_t *out) {
    snes_dsp1_t *s = &machine.dsp1;
    const int16_t vs = in[0];
    int16_t c, e, c1, e1;
    dsp1_inverse((int16_t)(I16(vs * s->sin_azsc >> 15) + s->v_offset), 7, &c, &e);
    e = (int16_t)(e + s->centre_z_e);
    c1 = I16(c * s->centre_z_c >> 15);
    e1 = (int16_t)(e + s->sec_azs_e2);
    dsp1_normalize(c1, &c, &e);
    c = dsp1_denormalize_clip(c, e);
    out[0] = I16(c * s->cos_aas >> 15);
    out[2] = I16(c * s->sin_aas >> 15);
    dsp1_normalize(I16(c1 * s->sec_azs_c2 >> 15), &c, &e1);
    c = dsp1_denormalize_clip(c, e1);
    out[1] = I16(c * (int16_t)(0 - s->sin_aas) >> 15);
    out[3] = I16(c * s->cos_aas >> 15);
}

/* a screen point (H,V) projected onto the ground */
static void dsp1_target(int16_t *in, int16_t *out) {
    snes_dsp1_t *s = &machine.dsp1;
    int16_t h = in[0], v = in[1];
    int16_t c, e, c1, e1;
    dsp1_inverse((int16_t)(I16(v * s->sin_azsc >> 15) + s->v_offset), 8, &c, &e);
    e = (int16_t)(e + s->centre_z_e);
    c1 = I16(c * s->centre_z_c >> 15);
    e1 = (int16_t)(e + s->sec_azs_e1);
    h = (int16_t)(h << 8);
    dsp1_normalize(c1, &c, &e);
    c = I16(dsp1_denormalize_clip(c, e) * h >> 15);
    out[0] = (int16_t)(s->centre_x + I16(c * s->cos_aas >> 15));
    out[1] = (int16_t)(s->centre_y - I16(c * s->sin_aas >> 15));
    v = (int16_t)(v << 8);
    dsp1_normalize(I16(c1 * s->sec_azs_c1 >> 15), &c, &e1);
    c = I16(dsp1_denormalize_clip(c, e1) * v >> 15);
    out[0] = (int16_t)(out[0] + I16(c * (int16_t)(0 - s->sin_aas) >> 15));
    out[1] = (int16_t)(out[1] + I16(c * s->cos_aas >> 15));
}

/* a world point (X,Y,Z) onto the screen: (H,V) and the scale M (1:1 at
 * 0x0100) */
static void dsp1_project(int16_t *in, int16_t *out) {
    snes_dsp1_t *s = &machine.dsp1;
    const int16_t x = in[0], y = in[1], z = in[2];
    int16_t e, e2, e3, e4, ref_e, e6, e7, e5;
    int16_t c2, c4, c6;
    int16_t px, py, pz;
    /* the reference reuses its `refE` for `16 - refE` from here on: the
     * de-normalization shift and the H/V/M exponents all use that one */

    e2 = e3 = e4 = e5 = e6 = e7 = 0;
    dsp1_normalize_double(x - s->gx, &px, &e4);
    dsp1_normalize_double(y - s->gy, &py, &e);
    dsp1_normalize_double(z - s->gz, &pz, &e3);
    px >>= 1; e4--; /* headroom for the scalar products */
    py >>= 1; e--;
    pz >>= 1; e3--;

    ref_e = e < e3 ? e : e3;
    ref_e = ref_e < e4 ? ref_e : e4;
    px = dsp1_shift_right(px, (int16_t)(e4 - ref_e));
    py = dsp1_shift_right(py, (int16_t)(e - ref_e));
    pz = dsp1_shift_right(pz, (int16_t)(e3 - ref_e));

    const int16_t c11 = (int16_t)-I16(px * s->nx >> 15);
    const int16_t c8 = (int16_t)-I16(py * s->ny >> 15);
    const int16_t c9 = (int16_t)-I16(pz * s->nz >> 15);
    const int16_t c12 = (int16_t)(c11 + c8 + c9);

    int32_t aux4 = c12; /* de-normalize in 32 bits */
    const int16_t re = (int16_t)(16 - ref_e); /* ref_e can be up to 3 */
    if (re >= 0) aux4 <<= re;
    else aux4 >>= -re;
    if (aux4 == -1) aux4 = 0;
    aux4 >>= 1;

    const int32_t aux = (uint16_t)s->les + aux4; /* Les - P.n */
    dsp1_normalize_double(aux, &c4, &e2);
    e2 = (int16_t)(15 - e2);

    dsp1_inverse(c4, 0, &c4, &e5);
    c2 = I16(c4 * s->c_les >> 15); /* the scale factor */

    /* H */
    const int16_t c16 = I16(px * s->hx >> 15);
    const int16_t c17 = (int16_t)(c16 + I16(py * s->hy >> 15));
    const int16_t c18 = I16(c17 * c2 >> 15);
    dsp1_normalize(c18, &c4, &e7);
    out[0] = dsp1_denormalize_clip(c4, (int16_t)(s->e_les - e2 + re + e7));

    /* V */
    const int16_t c24 = (int16_t)(I16(px * s->vx >> 15) + I16(py * s->vy >> 15) +
                                  I16(pz * s->vz >> 15));
    const int16_t c26 = I16(c24 * c2 >> 15);
    dsp1_normalize(c26, &c6, &e6);
    out[1] = dsp1_denormalize_clip(c6, (int16_t)(s->e_les - e2 + re + e6));

    /* M: the exponent feeds from the inverse's, in-out through normalize */
    dsp1_normalize(c2, &c6, &e5);
    out[2] = dsp1_denormalize_clip(c6, (int16_t)(e5 + s->e_les - e2 - 7));
}

/* ---- the FSM ---- */

/* the command table as the chip decodes it: the seven low bits pick the
 * command, the two high bits (a partial command) are ignored */
static const dsp1_cmd dsp1_commands[0x40] = {
    {dsp1_multiply, 2, 1},       {dsp1_attitude_a, 4, 0},    {dsp1_parameter, 7, 4},
    {dsp1_subjective_a, 3, 3},   {dsp1_triangle, 2, 2},      {dsp1_attitude_a, 4, 0},
    {dsp1_project, 3, 3},        {dsp1_memory_test, 1, 1},   {dsp1_radius, 3, 2},
    {dsp1_objective_a, 3, 3},    {dsp1_raster, 1, 4},        {dsp1_scalar_a, 3, 1},
    {dsp1_rotate, 3, 2},         {dsp1_objective_a, 3, 3},   {dsp1_target, 2, 2},
    {dsp1_memory_test, 1, 1},

    {dsp1_inverse_cmd, 2, 2},    {dsp1_attitude_b, 4, 0},    {dsp1_parameter, 7, 4},
    {dsp1_subjective_b, 3, 3},   {dsp1_gyrate, 6, 3},        {dsp1_attitude_b, 4, 0},
    {dsp1_project, 3, 3},        {dsp1_memory_dump, 1, 1024},{dsp1_range, 4, 1},
    {dsp1_objective_b, 3, 3},    {0, 0, 0},                  {dsp1_scalar_b, 3, 1},
    {dsp1_polar, 6, 3},          {dsp1_objective_b, 3, 3},   {dsp1_target, 2, 2},
    {dsp1_memory_dump, 1, 1024},

    {dsp1_multiply2, 2, 1},      {dsp1_attitude_c, 4, 0},    {dsp1_parameter, 7, 4},
    {dsp1_subjective_c, 3, 3},   {dsp1_triangle, 2, 2},      {dsp1_attitude_c, 4, 0},
    {dsp1_project, 3, 3},        {dsp1_memory_size, 1, 1},   {dsp1_distance, 3, 1},
    {dsp1_objective_c, 3, 3},    {0, 0, 0},                  {dsp1_scalar_c, 3, 1},
    {dsp1_rotate, 3, 2},         {dsp1_objective_c, 3, 3},   {dsp1_target, 2, 2},
    {dsp1_memory_size, 1, 1},

    {dsp1_inverse_cmd, 2, 2},    {dsp1_attitude_a, 4, 0},    {dsp1_parameter, 7, 4},
    {dsp1_subjective_a, 3, 3},   {dsp1_gyrate, 6, 3},        {dsp1_attitude_a, 4, 0},
    {dsp1_project, 3, 3},        {dsp1_memory_dump, 1, 1024},{dsp1_range2, 4, 1},
    {dsp1_objective_a, 3, 3},    {0, 0, 0},                  {dsp1_scalar_a, 3, 1},
    {dsp1_polar, 6, 3},          {dsp1_objective_a, 3, 3},   {dsp1_target, 2, 2},
    {dsp1_memory_dump, 1, 1024},
};

/* one DR access, a read or a write: the FSM binds the byte the DRS bit
 * selects and advances (the chip runs the command between the accesses,
 * the model answers at once) */
static void dsp1_step(int read, uint8_t *data) {
    snes_dsp1_t *d = &machine.dsp1;
    if (!(d->sr & DSP1_RQM)) return;
    if (read)
        *data = (uint8_t)((d->sr & DSP1_DRS) ? d->dr >> 8 : d->dr);
    else if (d->sr & DSP1_DRS) {
        d->dr &= 0x00ff;
        d->dr |= (uint16_t)*data << 8;
    } else {
        d->dr &= 0xff00;
        d->dr |= *data;
    }

    switch (d->major) {
    case DSP1_WAIT: /* a command byte (an 8-bit transfer) */
        d->command = (uint8_t)d->dr;
        if (!(d->command & 0xc0)) {
            if (d->command == 0x1a || d->command == 0x2a || d->command == 0x3a)
                d->freeze = 1; /* the chip freezes until the reset */
            else {
                d->counter = 0;
                d->major = DSP1_READ;
                d->sr &= (uint8_t)~DSP1_DRC;
            }
        }
        break;
    case DSP1_READ: /* the command's data words, low byte first */
        d->sr ^= DSP1_DRS;
        if (!(d->sr & DSP1_DRS)) {
            const dsp1_cmd *cmd = &dsp1_commands[d->command];
            d->in_buf[d->counter++] = (int16_t)d->dr;
            if (d->counter >= cmd->reads) {
                if (cmd->fn) cmd->fn(d->in_buf, d->out_buf);
                if (cmd->writes) {
                    d->counter = 0;
                    d->dr = (uint16_t)d->out_buf[0];
                    d->major = DSP1_WRITE;
                } else {
                    d->dr = 0x0080; /* valid command completion */
                    d->major = DSP1_WAIT;
                    d->sr |= DSP1_DRC;
                }
            }
        }
        break;
    case DSP1_WRITE: /* the results, low byte first */
        d->sr ^= DSP1_DRS;
        if (!(d->sr & DSP1_DRS)) {
            const dsp1_cmd *cmd = &dsp1_commands[d->command];
            d->counter++;
            if (d->counter >= cmd->writes) {
                if (d->command == 0x0a && d->dr != 0x8000) {
                    /* the raster command's continuous mode: the next
                     * line until the CPU writes 0x8000 */
                    d->in_buf[0]++;
                    if (cmd->fn) cmd->fn(d->in_buf, d->out_buf);
                    d->counter = 0;
                    d->dr = (uint16_t)d->out_buf[0];
                } else {
                    d->dr = 0x0080;
                    d->major = DSP1_WAIT;
                    d->sr |= DSP1_DRC;
                }
            } else
                d->dr = (uint16_t)d->out_buf[d->counter];
        }
        break;
    }

    if (d->freeze) d->sr &= (uint8_t)~DSP1_RQM;
}

void snes_dsp1_reset(void) {
    snes_dsp1_t *d = &machine.dsp1;
    memset(d, 0, sizeof *d);
    d->sr = DSP1_DRC | DSP1_RQM;
    d->dr = 0x0080;
    d->major = DSP1_WAIT;
}

uint8_t snes_dsp1_dr_read(void) {
    uint8_t v = 0;
    dsp1_step(1, &v);
    return v;
}

uint8_t snes_dsp1_sr_read(void) {
    /* the status alternates a zero byte and the register byte: the games
     * read it twice and take the second */
    snes_dsp1_t *d = &machine.dsp1;
    d->low_byte ^= 1;
    return d->low_byte ? 0 : d->sr;
}

void snes_dsp1_dr_write(uint8_t v) {
    dsp1_step(0, &v);
}
