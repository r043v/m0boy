/* TMS9918A, one line at a time.
 *
 * vdp_line runs once per source line: sprite status (fifth sprite,
 * coincidence) for every line the display shows, then the pixels.  Device
 * output is SG_OUT_WIDTH RGB565 pixels taken straight from VRAM (Graphics I
 * and II: 5 of the 8 pixels of each tile, the nearest-neighbour 256 -> 160
 * columns); text and multicolour modes, and the host's reference image, go
 * through the 256-pixel colour-index renderer. */
#ifndef M0_TL
#define M0_TL   /* deepeel builds a thread-local flavour with -DM0_TL=__thread */
#endif
#include "sg1000.h"
#include <string.h>

typedef struct {
    int16_t left;
    uint16_t bits;  /* pattern row, bit 15 = leftmost pattern pixel */
    uint8_t color, width, shift;
} sprite_t;

/* Device line: a box filter from 256 to 160 pixels (m0_scale.h, the exact
 * 5-in-8 method).  Each device pixel covers 1.6 source pixels; in eighths,
 * the five pixels of a tile take
 *   o0 = 5 s0 + 3 s1          o3 = 1 s4 + 5 s5 + 2 s6
 *   o1 = 2 s1 + 5 s2 + 1 s3   o4 = 3 s6 + 5 s7
 *   o2 = 4 s3 + 4 s4
 * Colours are added as spread RGB565 (channels apart in a 32-bit word),
 * rounded, then folded back.  A background tile has two colours only, so
 * each device pixel is mix(foreground, background, w) with w the weight of
 * the foreground bits: tile_w gives the five weights of a pattern byte and
 * mix the 9 blends of a colour byte (rebuilt when the palette or the
 * backdrop changes).  Both paths compute the same values. */
#define M0_PANEL_SWAP SG_PANEL_SWAP
#include "m0_scale.h"
#include <stddef.h>
#include <string.h>
/* five weights of a pattern byte, as byte offsets 2w in 5-bit fields */
const uint32_t sg_tile_w[256] = {
    0x0000000, 0x0a00000, 0x0620000, 0x1020000, 0x0050000, 0x0a50000,
    0x0670000, 0x1070000, 0x0012000, 0x0a12000, 0x0632000, 0x1032000,
    0x0062000, 0x0a62000, 0x0682000, 0x1082000, 0x0002040, 0x0a02040,
    0x0622040, 0x1022040, 0x0052040, 0x0a52040, 0x0672040, 0x1072040,
    0x0014040, 0x0a14040, 0x0634040, 0x1034040, 0x0064040, 0x0a64040,
    0x0684040, 0x1084040, 0x0000140, 0x0a00140, 0x0620140, 0x1020140,
    0x0050140, 0x0a50140, 0x0670140, 0x1070140, 0x0012140, 0x0a12140,
    0x0632140, 0x1032140, 0x0062140, 0x0a62140, 0x0682140, 0x1082140,
    0x0002180, 0x0a02180, 0x0622180, 0x1022180, 0x0052180, 0x0a52180,
    0x0672180, 0x1072180, 0x0014180, 0x0a14180, 0x0634180, 0x1034180,
    0x0064180, 0x0a64180, 0x0684180, 0x1084180, 0x0000086, 0x0a00086,
    0x0620086, 0x1020086, 0x0050086, 0x0a50086, 0x0670086, 0x1070086,
    0x0012086, 0x0a12086, 0x0632086, 0x1032086, 0x0062086, 0x0a62086,
    0x0682086, 0x1082086, 0x00020c6, 0x0a020c6, 0x06220c6, 0x10220c6,
    0x00520c6, 0x0a520c6, 0x06720c6, 0x10720c6, 0x00140c6, 0x0a140c6,
    0x06340c6, 0x10340c6, 0x00640c6, 0x0a640c6, 0x06840c6, 0x10840c6,
    0x00001c6, 0x0a001c6, 0x06201c6, 0x10201c6, 0x00501c6, 0x0a501c6,
    0x06701c6, 0x10701c6, 0x00121c6, 0x0a121c6, 0x06321c6, 0x10321c6,
    0x00621c6, 0x0a621c6, 0x06821c6, 0x10821c6, 0x0002206, 0x0a02206,
    0x0622206, 0x1022206, 0x0052206, 0x0a52206, 0x0672206, 0x1072206,
    0x0014206, 0x0a14206, 0x0634206, 0x1034206, 0x0064206, 0x0a64206,
    0x0684206, 0x1084206, 0x000000a, 0x0a0000a, 0x062000a, 0x102000a,
    0x005000a, 0x0a5000a, 0x067000a, 0x107000a, 0x001200a, 0x0a1200a,
    0x063200a, 0x103200a, 0x006200a, 0x0a6200a, 0x068200a, 0x108200a,
    0x000204a, 0x0a0204a, 0x062204a, 0x102204a, 0x005204a, 0x0a5204a,
    0x067204a, 0x107204a, 0x001404a, 0x0a1404a, 0x063404a, 0x103404a,
    0x006404a, 0x0a6404a, 0x068404a, 0x108404a, 0x000014a, 0x0a0014a,
    0x062014a, 0x102014a, 0x005014a, 0x0a5014a, 0x067014a, 0x107014a,
    0x001214a, 0x0a1214a, 0x063214a, 0x103214a, 0x006214a, 0x0a6214a,
    0x068214a, 0x108214a, 0x000218a, 0x0a0218a, 0x062218a, 0x102218a,
    0x005218a, 0x0a5218a, 0x067218a, 0x107218a, 0x001418a, 0x0a1418a,
    0x063418a, 0x103418a, 0x006418a, 0x0a6418a, 0x068418a, 0x108418a,
    0x0000090, 0x0a00090, 0x0620090, 0x1020090, 0x0050090, 0x0a50090,
    0x0670090, 0x1070090, 0x0012090, 0x0a12090, 0x0632090, 0x1032090,
    0x0062090, 0x0a62090, 0x0682090, 0x1082090, 0x00020d0, 0x0a020d0,
    0x06220d0, 0x10220d0, 0x00520d0, 0x0a520d0, 0x06720d0, 0x10720d0,
    0x00140d0, 0x0a140d0, 0x06340d0, 0x10340d0, 0x00640d0, 0x0a640d0,
    0x06840d0, 0x10840d0, 0x00001d0, 0x0a001d0, 0x06201d0, 0x10201d0,
    0x00501d0, 0x0a501d0, 0x06701d0, 0x10701d0, 0x00121d0, 0x0a121d0,
    0x06321d0, 0x10321d0, 0x00621d0, 0x0a621d0, 0x06821d0, 0x10821d0,
    0x0002210, 0x0a02210, 0x0622210, 0x1022210, 0x0052210, 0x0a52210,
    0x0672210, 0x1072210, 0x0014210, 0x0a14210, 0x0634210, 0x1034210,
    0x0064210, 0x0a64210, 0x0684210, 0x1084210,
};
/* the same weights w (0-8) in 5-bit fields: two lines add without carry */
const uint32_t sg_tile_wv[256] = {
    0x0000000, 0x0500000, 0x0310000, 0x0810000, 0x0028000, 0x0528000,
    0x0338000, 0x0838000, 0x0009000, 0x0509000, 0x0319000, 0x0819000,
    0x0031000, 0x0531000, 0x0341000, 0x0841000, 0x0001020, 0x0501020,
    0x0311020, 0x0811020, 0x0029020, 0x0529020, 0x0339020, 0x0839020,
    0x000a020, 0x050a020, 0x031a020, 0x081a020, 0x0032020, 0x0532020,
    0x0342020, 0x0842020, 0x00000a0, 0x05000a0, 0x03100a0, 0x08100a0,
    0x00280a0, 0x05280a0, 0x03380a0, 0x08380a0, 0x00090a0, 0x05090a0,
    0x03190a0, 0x08190a0, 0x00310a0, 0x05310a0, 0x03410a0, 0x08410a0,
    0x00010c0, 0x05010c0, 0x03110c0, 0x08110c0, 0x00290c0, 0x05290c0,
    0x03390c0, 0x08390c0, 0x000a0c0, 0x050a0c0, 0x031a0c0, 0x081a0c0,
    0x00320c0, 0x05320c0, 0x03420c0, 0x08420c0, 0x0000043, 0x0500043,
    0x0310043, 0x0810043, 0x0028043, 0x0528043, 0x0338043, 0x0838043,
    0x0009043, 0x0509043, 0x0319043, 0x0819043, 0x0031043, 0x0531043,
    0x0341043, 0x0841043, 0x0001063, 0x0501063, 0x0311063, 0x0811063,
    0x0029063, 0x0529063, 0x0339063, 0x0839063, 0x000a063, 0x050a063,
    0x031a063, 0x081a063, 0x0032063, 0x0532063, 0x0342063, 0x0842063,
    0x00000e3, 0x05000e3, 0x03100e3, 0x08100e3, 0x00280e3, 0x05280e3,
    0x03380e3, 0x08380e3, 0x00090e3, 0x05090e3, 0x03190e3, 0x08190e3,
    0x00310e3, 0x05310e3, 0x03410e3, 0x08410e3, 0x0001103, 0x0501103,
    0x0311103, 0x0811103, 0x0029103, 0x0529103, 0x0339103, 0x0839103,
    0x000a103, 0x050a103, 0x031a103, 0x081a103, 0x0032103, 0x0532103,
    0x0342103, 0x0842103, 0x0000005, 0x0500005, 0x0310005, 0x0810005,
    0x0028005, 0x0528005, 0x0338005, 0x0838005, 0x0009005, 0x0509005,
    0x0319005, 0x0819005, 0x0031005, 0x0531005, 0x0341005, 0x0841005,
    0x0001025, 0x0501025, 0x0311025, 0x0811025, 0x0029025, 0x0529025,
    0x0339025, 0x0839025, 0x000a025, 0x050a025, 0x031a025, 0x081a025,
    0x0032025, 0x0532025, 0x0342025, 0x0842025, 0x00000a5, 0x05000a5,
    0x03100a5, 0x08100a5, 0x00280a5, 0x05280a5, 0x03380a5, 0x08380a5,
    0x00090a5, 0x05090a5, 0x03190a5, 0x08190a5, 0x00310a5, 0x05310a5,
    0x03410a5, 0x08410a5, 0x00010c5, 0x05010c5, 0x03110c5, 0x08110c5,
    0x00290c5, 0x05290c5, 0x03390c5, 0x08390c5, 0x000a0c5, 0x050a0c5,
    0x031a0c5, 0x081a0c5, 0x00320c5, 0x05320c5, 0x03420c5, 0x08420c5,
    0x0000048, 0x0500048, 0x0310048, 0x0810048, 0x0028048, 0x0528048,
    0x0338048, 0x0838048, 0x0009048, 0x0509048, 0x0319048, 0x0819048,
    0x0031048, 0x0531048, 0x0341048, 0x0841048, 0x0001068, 0x0501068,
    0x0311068, 0x0811068, 0x0029068, 0x0529068, 0x0339068, 0x0839068,
    0x000a068, 0x050a068, 0x031a068, 0x081a068, 0x0032068, 0x0532068,
    0x0342068, 0x0842068, 0x00000e8, 0x05000e8, 0x03100e8, 0x08100e8,
    0x00280e8, 0x05280e8, 0x03380e8, 0x08380e8, 0x00090e8, 0x05090e8,
    0x03190e8, 0x08190e8, 0x00310e8, 0x05310e8, 0x03410e8, 0x08410e8,
    0x0001108, 0x0501108, 0x0311108, 0x0811108, 0x0029108, 0x0529108,
    0x0339108, 0x0839108, 0x000a108, 0x050a108, 0x031a108, 0x081a108,
    0x0032108, 0x0532108, 0x0342108, 0x0842108,
};
/* mix_row(c): the 9 blends of colour byte c (byte-addressed: entry w at
 * offset 2w); mix_prepare first, once per line.  A full table (4.5 KiB) or,
 * with cartridge RAM (SG_LOW_RAM), a 16-entry cache filled on demand. */
static M0_TL uint16_t mix_backdrop;
static M0_TL uint8_t mix_ready;
static inline void mix_fill(uint16_t *row, unsigned c, const uint16_t *pal, uint16_t bd) {
    uint32_t f = m0_spread565((c >> 4) ? pal[c >> 4] : bd), b = m0_spread565((c & 15) ? pal[c & 15] : bd);
    for (unsigned w = 0; w <= 8; w++) row[w] = m0_panel(m0_fold565(w * f + (8 - w) * b));
}
#if SG_MIX_SLOTS
/* A pool of SG_MIX_SLOTS rows filled on demand and an index of 256 byte
 * offsets (0: the colour byte has no row yet), the asm loops' layout: the
 * row of colour byte c is at (uint8_t *)&sg_mix + off[c].  A missing row
 * (sg_mix_miss) takes the slot after the one filled last, first-in first-out;
 * keep: the offset of a row the caller still uses, never taken. */
typedef struct {
    uint16_t off[256];
    uint16_t row[SG_MIX_SLOTS][9];
} sg_mix_t;
sg_mix_t sg_mix __attribute__((aligned(4)));
static M0_TL uint16_t mix_owner[SG_MIX_SLOTS]; /* colour byte of each slot, 0xffff: free */
static M0_TL uint8_t mix_hand;
static void mix_reset(const uint16_t *pal, uint16_t bd) {
    (void)pal; (void)bd;
    memset(sg_mix.off, 0, sizeof sg_mix.off);
    memset(mix_owner, 0xff, sizeof mix_owner);
    mix_hand = 0;
}
uint32_t sg_mix_miss(unsigned c, unsigned keep) {
    unsigned s = mix_hand;
    if (offsetof(sg_mix_t, row[s]) == keep) s = s + 1 == SG_MIX_SLOTS ? 0 : s + 1;
    if (mix_owner[s] < 256) sg_mix.off[mix_owner[s]] = 0;
    mix_fill(sg_mix.row[s], c, machine.palette, machine.palette[machine.vdp[7] & 15]);
    mix_owner[s] = (uint16_t)c;
    mix_hand = (uint8_t)(s + 1 == SG_MIX_SLOTS ? 0 : s + 1);
    return sg_mix.off[c] = (uint16_t)offsetof(sg_mix_t, row[s]);
}
static inline const uint8_t *mix_row_keep(unsigned c, const uint8_t *keep) {
    unsigned o = sg_mix.off[c];
    if (!o) o = sg_mix_miss(c, keep ? (unsigned)(keep - (const uint8_t *)&sg_mix) : 0);
    return (const uint8_t *)&sg_mix + o;
}
static inline const uint8_t *mix_row(unsigned c) { return mix_row_keep(c, 0); }
#define SG_MIX_BASE (&sg_mix)
#elif !SG_LOW_RAM
static M0_TL uint16_t mix[256][9];
static void mix_reset(const uint16_t *pal, uint16_t bd) {
    for (unsigned c = 0; c < 256; c++) mix_fill(mix[c], c, pal, bd);
}
static inline const uint8_t *mix_row(unsigned c) { return (const uint8_t *)mix[c]; }
#define SG_MIX_BASE mix
#else
#define SG_MIX_CACHE 16
static M0_TL uint16_t mix[SG_MIX_CACHE][9];
static M0_TL int16_t mix_tag[SG_MIX_CACHE];
static void mix_reset(const uint16_t *pal, uint16_t bd) {
    (void)pal; (void)bd;
    for (unsigned i = 0; i < SG_MIX_CACHE; i++) mix_tag[i] = -1;
}
static inline const uint8_t *mix_row(unsigned c) {
    unsigned slot = (c ^ (c >> 4)) & (SG_MIX_CACHE - 1);
    if (mix_tag[slot] != (int16_t)c) {
        mix_fill(mix[slot], c, machine.palette, machine.palette[machine.vdp[7] & 15]);
        mix_tag[slot] = (int16_t)c;
    }
    return (const uint8_t *)mix[slot];
}
#endif
/* machine.palette is set by sg1000_init only, which calls this */
static M0_TL uint32_t spal[16];
static M0_TL unsigned spal_backdrop = ~0u;
void sg1000_palette_changed(void) {
    mix_ready = 0;
    spal_backdrop = ~0u;
}
static void mix_prepare(const uint16_t *pal, uint16_t bd) {
    if (mix_ready && mix_backdrop == bd) return;
    mix_reset(pal, bd);
    mix_backdrop = bd;
    mix_ready = 1;
}

#define tile_w sg_tile_w
#define tile_wv sg_tile_wv

/* Thumb-1 tile loops (sg_render_asm.S), with the full blend table only. */
#if defined(__ARM_ARCH_6M__) && (!SG_LOW_RAM || SG_MIX_SLOTS) && !defined(SG_NO_RENDER_ASM)
#define SG_RENDER_ASM 1
typedef struct {
    uint16_t *out;
    const uint8_t *names, *pt, *ct;
    const void *mix;
    uint32_t third, pmask, cmask;
} sg_tile_args;
void sg_tiles_g1(const sg_tile_args *);
void sg_tiles_g2(const sg_tile_args *);
void sg_pair_g1(const sg_tile_args *);
void sg_pair_g2(const sg_tile_args *);
#else
#define SG_RENDER_ASM 0
#endif
#if SG_RENDER_ASM
/* sg_tile_w as bytes, five offsets 2w per pattern byte at a stride of 8 (the pair loop of
 * sg_render_asm.S: one load a pixel instead of two shifts) */
const uint8_t sg_tile_w8[256][8] = {
    {0,0,0,0,0}, {0,0,0,0,10}, {0,0,0,4,6}, {0,0,0,4,16},
    {0,0,0,10,0}, {0,0,0,10,10}, {0,0,0,14,6}, {0,0,0,14,16},
    {0,0,8,2,0}, {0,0,8,2,10}, {0,0,8,6,6}, {0,0,8,6,16},
    {0,0,8,12,0}, {0,0,8,12,10}, {0,0,8,16,6}, {0,0,8,16,16},
    {0,2,8,0,0}, {0,2,8,0,10}, {0,2,8,4,6}, {0,2,8,4,16},
    {0,2,8,10,0}, {0,2,8,10,10}, {0,2,8,14,6}, {0,2,8,14,16},
    {0,2,16,2,0}, {0,2,16,2,10}, {0,2,16,6,6}, {0,2,16,6,16},
    {0,2,16,12,0}, {0,2,16,12,10}, {0,2,16,16,6}, {0,2,16,16,16},
    {0,10,0,0,0}, {0,10,0,0,10}, {0,10,0,4,6}, {0,10,0,4,16},
    {0,10,0,10,0}, {0,10,0,10,10}, {0,10,0,14,6}, {0,10,0,14,16},
    {0,10,8,2,0}, {0,10,8,2,10}, {0,10,8,6,6}, {0,10,8,6,16},
    {0,10,8,12,0}, {0,10,8,12,10}, {0,10,8,16,6}, {0,10,8,16,16},
    {0,12,8,0,0}, {0,12,8,0,10}, {0,12,8,4,6}, {0,12,8,4,16},
    {0,12,8,10,0}, {0,12,8,10,10}, {0,12,8,14,6}, {0,12,8,14,16},
    {0,12,16,2,0}, {0,12,16,2,10}, {0,12,16,6,6}, {0,12,16,6,16},
    {0,12,16,12,0}, {0,12,16,12,10}, {0,12,16,16,6}, {0,12,16,16,16},
    {6,4,0,0,0}, {6,4,0,0,10}, {6,4,0,4,6}, {6,4,0,4,16},
    {6,4,0,10,0}, {6,4,0,10,10}, {6,4,0,14,6}, {6,4,0,14,16},
    {6,4,8,2,0}, {6,4,8,2,10}, {6,4,8,6,6}, {6,4,8,6,16},
    {6,4,8,12,0}, {6,4,8,12,10}, {6,4,8,16,6}, {6,4,8,16,16},
    {6,6,8,0,0}, {6,6,8,0,10}, {6,6,8,4,6}, {6,6,8,4,16},
    {6,6,8,10,0}, {6,6,8,10,10}, {6,6,8,14,6}, {6,6,8,14,16},
    {6,6,16,2,0}, {6,6,16,2,10}, {6,6,16,6,6}, {6,6,16,6,16},
    {6,6,16,12,0}, {6,6,16,12,10}, {6,6,16,16,6}, {6,6,16,16,16},
    {6,14,0,0,0}, {6,14,0,0,10}, {6,14,0,4,6}, {6,14,0,4,16},
    {6,14,0,10,0}, {6,14,0,10,10}, {6,14,0,14,6}, {6,14,0,14,16},
    {6,14,8,2,0}, {6,14,8,2,10}, {6,14,8,6,6}, {6,14,8,6,16},
    {6,14,8,12,0}, {6,14,8,12,10}, {6,14,8,16,6}, {6,14,8,16,16},
    {6,16,8,0,0}, {6,16,8,0,10}, {6,16,8,4,6}, {6,16,8,4,16},
    {6,16,8,10,0}, {6,16,8,10,10}, {6,16,8,14,6}, {6,16,8,14,16},
    {6,16,16,2,0}, {6,16,16,2,10}, {6,16,16,6,6}, {6,16,16,6,16},
    {6,16,16,12,0}, {6,16,16,12,10}, {6,16,16,16,6}, {6,16,16,16,16},
    {10,0,0,0,0}, {10,0,0,0,10}, {10,0,0,4,6}, {10,0,0,4,16},
    {10,0,0,10,0}, {10,0,0,10,10}, {10,0,0,14,6}, {10,0,0,14,16},
    {10,0,8,2,0}, {10,0,8,2,10}, {10,0,8,6,6}, {10,0,8,6,16},
    {10,0,8,12,0}, {10,0,8,12,10}, {10,0,8,16,6}, {10,0,8,16,16},
    {10,2,8,0,0}, {10,2,8,0,10}, {10,2,8,4,6}, {10,2,8,4,16},
    {10,2,8,10,0}, {10,2,8,10,10}, {10,2,8,14,6}, {10,2,8,14,16},
    {10,2,16,2,0}, {10,2,16,2,10}, {10,2,16,6,6}, {10,2,16,6,16},
    {10,2,16,12,0}, {10,2,16,12,10}, {10,2,16,16,6}, {10,2,16,16,16},
    {10,10,0,0,0}, {10,10,0,0,10}, {10,10,0,4,6}, {10,10,0,4,16},
    {10,10,0,10,0}, {10,10,0,10,10}, {10,10,0,14,6}, {10,10,0,14,16},
    {10,10,8,2,0}, {10,10,8,2,10}, {10,10,8,6,6}, {10,10,8,6,16},
    {10,10,8,12,0}, {10,10,8,12,10}, {10,10,8,16,6}, {10,10,8,16,16},
    {10,12,8,0,0}, {10,12,8,0,10}, {10,12,8,4,6}, {10,12,8,4,16},
    {10,12,8,10,0}, {10,12,8,10,10}, {10,12,8,14,6}, {10,12,8,14,16},
    {10,12,16,2,0}, {10,12,16,2,10}, {10,12,16,6,6}, {10,12,16,6,16},
    {10,12,16,12,0}, {10,12,16,12,10}, {10,12,16,16,6}, {10,12,16,16,16},
    {16,4,0,0,0}, {16,4,0,0,10}, {16,4,0,4,6}, {16,4,0,4,16},
    {16,4,0,10,0}, {16,4,0,10,10}, {16,4,0,14,6}, {16,4,0,14,16},
    {16,4,8,2,0}, {16,4,8,2,10}, {16,4,8,6,6}, {16,4,8,6,16},
    {16,4,8,12,0}, {16,4,8,12,10}, {16,4,8,16,6}, {16,4,8,16,16},
    {16,6,8,0,0}, {16,6,8,0,10}, {16,6,8,4,6}, {16,6,8,4,16},
    {16,6,8,10,0}, {16,6,8,10,10}, {16,6,8,14,6}, {16,6,8,14,16},
    {16,6,16,2,0}, {16,6,16,2,10}, {16,6,16,6,6}, {16,6,16,6,16},
    {16,6,16,12,0}, {16,6,16,12,10}, {16,6,16,16,6}, {16,6,16,16,16},
    {16,14,0,0,0}, {16,14,0,0,10}, {16,14,0,4,6}, {16,14,0,4,16},
    {16,14,0,10,0}, {16,14,0,10,10}, {16,14,0,14,6}, {16,14,0,14,16},
    {16,14,8,2,0}, {16,14,8,2,10}, {16,14,8,6,6}, {16,14,8,6,16},
    {16,14,8,12,0}, {16,14,8,12,10}, {16,14,8,16,6}, {16,14,8,16,16},
    {16,16,8,0,0}, {16,16,8,0,10}, {16,16,8,4,6}, {16,16,8,4,16},
    {16,16,8,10,0}, {16,16,8,10,10}, {16,16,8,14,6}, {16,16,8,14,16},
    {16,16,16,2,0}, {16,16,16,2,10}, {16,16,16,6,6}, {16,16,16,6,16},
    {16,16,16,12,0}, {16,16,16,12,10}, {16,16,16,16,6}, {16,16,16,16,16},
};
#endif
/* SG_SELFTEST: both paths built, chosen at run time (render selftest) */
#ifdef SG_SELFTEST
static M0_TL int sg_render_c;
#define SG_USE_C sg_render_c
#else
#define SG_USE_C 0
#endif

/* Sprites covering each line, as a mask of sprite numbers, rebuilt when a
 * sprite Y, the terminator, the size or the table moves (sprites_dirty). */
#if SG_LOW_RAM
/* no room for the per-line table: masks per band of 8 lines, filtered per
 * line on the sprites' Y */
#define SG_MASK_ROWS (SG_HEIGHT / 8)
#define SG_MASK_SHIFT 3
#else
#define SG_MASK_ROWS SG_HEIGHT
#define SG_MASK_SHIFT 0
#endif
static M0_TL uint32_t line_mask[SG_MASK_ROWS];
static M0_TL unsigned sat_end; /* sprites before the terminator */

static inline int sprite_top(unsigned sy) { return sy > 208 ? (int)sy - 255 : (int)sy + 1; }
static void span_bits(unsigned sy, unsigned span, uint32_t bit, int set) {
    const int top = sprite_top(sy);
    int first = top < 0 ? 0 : top, last = top + (int)span;
    if (last > SG_HEIGHT) last = SG_HEIGHT;
    if (first >= last) return;
    first >>= SG_MASK_SHIFT;
    last = ((last - 1) >> SG_MASK_SHIFT) + 1;
    if (set) for (int r = first; r < last; r++) line_mask[r] |= bit;
    else for (int r = first; r < last; r++) line_mask[r] &= ~bit;
}
static void build_masks(void) {
    sg1000_t *m = &machine;
    for (unsigned r = 0; r < SG_MASK_ROWS; r++) line_mask[r] = 0;
    const uint8_t *table = m->vram + m->sat_base;
    unsigned span = ((m->vdp[1] & 2) ? 16u : 8u) << (m->vdp[1] & 1);
    unsigned i = 0;
    for (; i < 32 && table[i * 4] != 208; i++) span_bits(table[i * 4], span, 1u << i, 1);
    sat_end = i;
    m->sprites_dirty = 0;
}

/* Sprite i's Y goes from old to value (before the VRAM write): its lines
 * move; the terminator (208) appearing or going rebuilds everything.  With
 * bands, a sprite keeps the bit of a band its other lines still cover:
 * clearing first and setting after is exact as each sprite has its bit. */
void sg1000_sprite_y(unsigned i, unsigned old, unsigned value) {
    sg1000_t *m = &machine;
    if (m->sprites_dirty || old == value) return;
    if (old == 208 || value == 208) { m->sprites_dirty = 1; return; }
    if (i >= sat_end) return;
    const unsigned span = ((m->vdp[1] & 2) ? 16u : 8u) << (m->vdp[1] & 1);
    span_bits(old, span, 1u << i, 0);
    span_bits(value, span, 1u << i, 1);
}

/* number of the single bit set in low */
static inline unsigned sg_bit_index(uint32_t low) {
    static const uint8_t debruijn[32] = { 0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
                                          31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9 };
    return debruijn[(low * 0x077cb531u) >> 27];
}

/* The first four sprites on line y (colour 0 ones count but are dropped);
 * with status, also the fifth-sprite flag and number. */
static unsigned line_sprites(unsigned y, sprite_t *out, int status) {
    sg1000_t *m = &machine;
    if (m->sprites_dirty) build_masks();
    uint32_t mask = line_mask[y >> SG_MASK_SHIFT];
    if (!mask) return 0;
    const uint8_t *vram = m->vram;
    unsigned base = (m->vdp[6] & 7u) << 11;
    unsigned size = (m->vdp[1] & 2) ? 16 : 8, shift = m->vdp[1] & 1;
#if SG_LOW_RAM
    { /* the band's sprites that cover line y */
        const unsigned span = size << shift;
        uint32_t exact = 0;
        for (uint32_t b = mask; b;) {
            const uint32_t low = b & (0u - b);
            b ^= low;
            const unsigned i = sg_bit_index(low);
            if ((unsigned)((int)y - sprite_top(vram[m->sat_base + i * 4])) < span) exact |= low;
        }
        if (!(mask = exact)) return 0;
    }
#endif
    unsigned count = 0, n = 0;
    while (mask) { /* sprite numbers in order: lowest set bit first */
        const uint32_t low = mask & (0u - mask);
        const unsigned i = sg_bit_index(low);
        mask ^= low;
        const uint8_t *e = vram + m->sat_base + i * 4;
        if (++count == 5) {
            if (status && !(m->vdp_status & 64)) m->vdp_status = (uint8_t)((m->vdp_status & 0xa0) | 64 | i);
            break;
        }
        unsigned attr = e[3], color = attr & 15;
        if (!color) continue;
        const int top = sprite_top(e[0]);
        unsigned prow = (unsigned)((int)y - top) >> shift;
        unsigned tile = e[2];
        if (size == 16) tile &= ~3u;
        unsigned a = base + tile * 8 + (prow & 7) + (prow >= 8 ? 8 : 0);
        sprite_t *s = &out[n++];
        s->left = (int16_t)((int)e[1] - ((attr & 128) ? 32 : 0));
        s->bits = (uint16_t)((vram[a] << 8) | (size == 16 ? vram[a + 16] : 0));
        s->color = (uint8_t)color;
        s->width = (uint8_t)size;
        s->shift = (uint8_t)shift;
    }
    return n;
}

/* Coincidence: two non-transparent sprite pixels on the same screen pixel.
 * Rows as 32-bit masks (bit 31 = leftmost pixel), 256 pixels in 8 words. */
static uint32_t sprite_row_mask(const sprite_t *s) {
    uint32_t m = (uint32_t)s->bits << 16;
    if (s->shift) { /* magnified: each pattern pixel twice */
        uint32_t w = 0;
        for (unsigned b = 0; b < s->width; b++)
            if (s->bits & (0x8000u >> b)) w |= 0xc0000000u >> (2 * b);
        m = w;
    }
    return m;
}
static void collisions(const sprite_t *s, unsigned n) {
    if (n < 2) return;
    uint32_t used[9]; /* used[8]: pixels past the right edge */
    for (unsigned i = 0; i < 9; i++) ((volatile uint32_t *)used)[i] = 0; /* no memset call */
    for (unsigned k = 0; k < n; k++) {
        uint32_t m = sprite_row_mask(&s[k]);
        int x = s[k].left;
        if (x < 0) { if (x <= -32) continue; m <<= -x; x = 0; }
        unsigned w = (unsigned)x >> 5, off = (unsigned)x & 31;
        uint32_t a = m >> off, b = off ? m << (32 - off) : 0;
        if ((used[w] & a) || (w < 7 && (used[w + 1] & b))) { machine.vdp_status |= 32; return; }
        used[w] |= a;
        if (w < 7) used[w + 1] |= b;
    }
}

/* Reference renderer: 256 colour indices, sprites with first-wins priority. */
static void ref_line(unsigned y, uint8_t *pixels, const sprite_t *spr, unsigned n) {
    sg1000_t *m = &machine;
    const uint8_t *vram = m->vram;
    unsigned backdrop = m->vdp[7] & 15;
    if (!(m->vdp[1] & 64)) { memset(pixels, (int)backdrop, SG_WIDTH); return; }
    unsigned text = !!(m->vdp[1] & 16), multi = !!(m->vdp[1] & 8), graphics2 = !!(m->vdp[0] & 2);
    unsigned name = (m->vdp[2] & 15u) << 10, pattern = (m->vdp[4] & 7u) << 11, color = (unsigned)m->vdp[3] << 6;
    if (text) {
        memset(pixels, (int)backdrop, 8);
        memset(pixels + 248, (int)backdrop, 8);
    }
    unsigned columns = text ? 40 : 32, width = text ? 6 : 8;
    unsigned rowname = name + (y / 8) * columns;
    uint8_t *dest = pixels + (text ? 8 : 0);
    for (unsigned tx = 0; tx < columns; tx++) {
        unsigned tile = vram[(rowname + tx) & 0x3fff], bits = 0, colors;
        if (text) {
            bits = vram[(pattern + tile * 8 + (y & 7)) & 0x3fff];
            colors = m->vdp[7];
        } else if (multi) {
            colors = vram[(pattern + tile * 8 + ((y & 31) >> 2)) & 0x3fff];
            unsigned hi = colors >> 4, lo = colors & 15;
            memset(dest, (int)(hi ? hi : backdrop), 4);
            memset(dest + 4, (int)(lo ? lo : backdrop), 4);
            dest += 8;
            continue;
        } else {
            unsigned offset = tile * 8 + (y & 7);
            if (graphics2) {
                offset |= (y & 0xc0) << 5;
                unsigned pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff, cmask = ((m->vdp[3] & 127u) << 6) | 63;
                bits = vram[((m->vdp[4] & 4u) << 11) | (offset & pmask)];
                colors = vram[((m->vdp[3] & 128u) << 6) | (offset & cmask)];
            } else {
                bits = vram[(pattern + offset) & 0x3fff];
                colors = vram[(color + (tile >> 3)) & 0x3fff];
            }
        }
        unsigned hi = colors >> 4, lo = text ? backdrop : colors & 15;
        if (!hi && !text) hi = backdrop;
        if (!lo) lo = backdrop;
        for (unsigned xx = 0; xx < width; xx++) { *dest++ = (uint8_t)((bits & 128) ? hi : lo); bits <<= 1; }
    }
    if (text) return;
    for (unsigned k = n; k-- > 0;) /* lower numbers drawn last: they win */
        for (unsigned b = 0; b < spr[k].width; b++) {
            if (!(spr[k].bits & (0x8000u >> b))) continue;
            for (unsigned z = 0; z <= spr[k].shift; z++) {
                int x = spr[k].left + (int)((b << spr[k].shift) + z);
                if (x >= 0 && x < SG_WIDTH) pixels[x] = spr[k].color;
            }
        }
}

/* Pattern and colour bytes of background tile tx on line y (Graphics I/II). */
static inline void tile_bytes(unsigned y, unsigned tx, unsigned *pattern, unsigned *colors) {
    const sg1000_t *m = &machine;
    const uint8_t *vram = m->vram;
    unsigned tile = vram[((m->vdp[2] & 15u) << 10) + (y >> 3) * 32 + tx];
    if (m->vdp[0] & 2) {
        unsigned offset = tile * 8u | ((y & 0xc0) << 5) | (y & 7);
        *pattern = vram[((m->vdp[4] & 4u) << 11) | (offset & (((m->vdp[4] & 3u) << 11) | 0x7ff))];
        *colors = vram[((m->vdp[3] & 128u) << 6) | (offset & (((m->vdp[3] & 127u) << 6) | 63))];
    } else {
        *pattern = vram[((m->vdp[4] & 7u) << 11) + tile * 8 + (y & 7)];
        *colors = vram[((unsigned)m->vdp[3] << 6) + (tile >> 3)];
    }
}

/* Sprite pixels of a line in over[] (0: none; lower numbers win) for the
 * tiles they touch; returns the mask of those tiles. */
static uint32_t sprite_overlay(const sprite_t *spr, unsigned n, uint8_t *over) {
    uint32_t touched = 0;
    for (unsigned k = 0; k < n; k++) {
        int x0 = spr[k].left, x1 = x0 + (spr[k].width << spr[k].shift) - 1;
        if (x1 < 0 || x0 >= SG_WIDTH) continue;
        if (x0 < 0) x0 = 0;
        if (x1 >= SG_WIDTH) x1 = SG_WIDTH - 1;
        for (int t = x0 >> 3; t <= x1 >> 3; t++)
            if (!(touched & (1u << t))) { /* over is word-aligned: two stores, no memset call */
                touched |= 1u << t;
                ((volatile uint32_t *)over)[t * 2] = 0;
                ((volatile uint32_t *)over)[t * 2 + 1] = 0;
            }
    }
    for (unsigned k = n; k-- > 0;)
        for (unsigned b = 0; b < spr[k].width; b++) {
            if (!(spr[k].bits & (0x8000u >> b))) continue;
            for (unsigned z = 0; z <= spr[k].shift; z++) {
                int x = spr[k].left + (int)((b << spr[k].shift) + z);
                if (x >= 0 && x < SG_WIDTH) over[x] = spr[k].color;
            }
        }
    return touched;
}
/* Tiles under sprites (and pairs of differing colour bytes): the source
 * pixels as spread colours (spal: palette with the backdrop as colour 0),
 * filtered; a pair sums its two lines first, the exact 8 x 2 box. */
static const uint32_t *spread_palette(void) {
    const unsigned bd = machine.vdp[7] & 15;
    if (bd != spal_backdrop) {
        spal_backdrop = bd;
        for (unsigned c = 0; c < 16; c++) spal[c] = m0_spread565(machine.palette[c ? c : bd]);
    }
    return spal;
}
/* the 8 pixels of tile tx of line y, stored in src (add: added to it) */
static inline void tile_spread(unsigned y, unsigned tx, const uint8_t *over, const uint32_t *sp, uint32_t *src, int add) {
    unsigned pattern, colors;
    tile_bytes(y, tx, &pattern, &colors);
    const uint32_t f = sp[colors >> 4], b = sp[colors & 15];
    if (over) {
        over += tx * 8;
        for (unsigned i = 0; i < 8; i++, pattern <<= 1) {
            const unsigned c = over[i];
            const uint32_t v = c ? sp[c] : (pattern & 0x80) ? f : b;
            src[i] = add ? src[i] + v : v;
        }
    } else
        for (unsigned i = 0; i < 8; i++, pattern <<= 1) {
            const uint32_t v = (pattern & 0x80) ? f : b;
            src[i] = add ? src[i] + v : v;
        }
}
/* tile tx of line y with sprites (over, or none) */
static void tile_filtered(unsigned y, unsigned tx, const uint8_t *over, uint16_t *o) {
    uint32_t src[8];
    tile_spread(y, tx, over, spread_palette(), src, 0);
    m0_box58_exact(src, o);
}
/* tile tx of lines ya and ya + 1, sprites per line (or none) */
static void pair_filtered(unsigned ya, unsigned tx, const uint8_t *over_a, const uint8_t *over_b, uint16_t *o) {
    const uint32_t *sp = spread_palette();
    uint32_t s[8];
    tile_spread(ya, tx, over_a, sp, s, 0);
    tile_spread(ya + 1, tx, over_b, sp, s, 1);
    m0_box58_pair(s, o);
}

/* Device renderer: SG_OUT_WIDTH RGB565 pixels, box-filtered. */
static void out_line(unsigned y, uint16_t *out, const sprite_t *spr, unsigned n) {
#if SG_OUT_WIDTH == SG_WIDTH /* the PC keeps the TMS9918A's own 256 columns:
                              * the reference line, palette applied 1:1 — no
                              * filter, nothing cropped */
    uint8_t px[SG_WIDTH];
    ref_line(y, px, spr, n);
    const uint16_t *pal = machine.palette;
    for (unsigned i = 0; i < SG_OUT_WIDTH; i++) out[i] = m0_panel(pal[px[i] & 15]);
#else
    sg1000_t *m = &machine;
    const uint16_t *pal = m->palette;
    const uint16_t bd = pal[m->vdp[7] & 15];
    if (!(m->vdp[1] & 64)) {
        for (unsigned i = 0; i < SG_OUT_WIDTH; i++) out[i] = m0_panel(bd);
        return;
    }
    if (m->vdp[1] & 24) { /* text or multicolour: filter the reference line */
        uint8_t px[SG_WIDTH];
        ref_line(y, px, spr, n);
        sg1000_filter_line(px, out);
        return;
    }
    mix_prepare(pal, bd);
    /* everything the loop reads is hoisted: stores to out may alias VDP bytes */
    const uint8_t *vram = m->vram;
    const uint8_t *names = vram + (((m->vdp[2] & 15u) << 10) + (y >> 3) * 32), *pt, *ct;
    unsigned pmask, cmask, third;
    if (m->vdp[0] & 2) { /* Graphics II: three pattern/colour thirds, masked */
        pt = vram + ((m->vdp[4] & 4u) << 11);
        ct = vram + ((m->vdp[3] & 128u) << 6);
        pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff;
        cmask = ((m->vdp[3] & 127u) << 6) | 63;
        third = ((y & 0xc0) << 5) | (y & 7);
    } else { /* Graphics I: as Graphics II with colour = tile / 8 */
        pt = vram + ((m->vdp[4] & 7u) << 11) + (y & 7);
        ct = vram + ((unsigned)m->vdp[3] << 6);
        pmask = cmask = third = 0;
    }
    const int g2 = (m->vdp[0] & 2) != 0;
#if SG_RENDER_ASM
    if (!SG_USE_C) {
        const sg_tile_args args = { out, names, pt, ct, SG_MIX_BASE, third, pmask, cmask };
        if (g2) sg_tiles_g2(&args); else sg_tiles_g1(&args);
    } else
#endif
    {
    uint16_t *o = out;
    for (unsigned tx = 0; tx < 32; tx++, o += 5) {
        unsigned tile = names[tx], pattern, colors;
        if (g2) { unsigned off = tile * 8u | third; pattern = pt[off & pmask]; colors = ct[off & cmask]; }
        else { pattern = pt[tile * 8]; colors = ct[tile >> 3]; }
        const uint32_t w = tile_w[pattern];
        const uint8_t *mc = mix_row(colors);
#define SG_MIX(off) (*(const uint16_t *)(mc + (off)))
        o[0] = SG_MIX(w & 31);
        o[1] = SG_MIX((w >> 5) & 31);
        o[2] = SG_MIX((w >> 10) & 31);
        o[3] = SG_MIX((w >> 15) & 31);
        o[4] = SG_MIX(w >> 20);
#undef SG_MIX
    }
    }
    if (n) {
        uint8_t over[SG_WIDTH] __attribute__((aligned(4)));
        uint32_t touched = sprite_overlay(spr, n, over);
        for (unsigned tx = 0; touched; tx++, touched >>= 1)
            if (touched & 1) tile_filtered(y, tx, over, out + tx * 5);
    }
#endif
}

/* Reference line (256 colour indices) -> device line, the same filter. */
void sg1000_filter_line(const uint8_t *pixels, uint16_t *out) {
    const uint16_t *pal = machine.palette;
    for (unsigned t = 0; t < 32; t++) {
        uint32_t src[8];
        for (unsigned i = 0; i < 8; i++) src[i] = m0_spread565(pal[pixels[t * 8 + i] & 15]);
        m0_box58_exact(src, out + t * 5);
    }
}

/* the sprites of the previous line, for the pair drawn with it */
static M0_TL sprite_t prev_spr[4];
static unsigned prev_n, prev_y = ~0u;

/* Device row from two source lines (ya, ya + 1) of one tile row: tiles
 * whose two lines share their colour byte are filtered once with the mean
 * weight, (w_a + w_b + 1) / 2 eighths; other tiles and sprite tiles take
 * the exact 8 x 2 box (pair_filtered); the text/multicolour modes average
 * the two filtered lines. */
static void out_pair(unsigned ya, uint16_t *out, const sprite_t *spr_b, unsigned n_b) {
    sg1000_t *m = &machine;
    const unsigned yb = ya + 1;
    sprite_t spr_a[4];
    unsigned n_a = 0;
    if (prev_y == ya) { /* rendered just before: same sprite list */
        n_a = prev_n;
        for (unsigned i = 0; i < n_a; i++) spr_a[i] = prev_spr[i];
    } else if ((m->vdp[1] & 64) && !(m->vdp[1] & 16)) n_a = line_sprites(ya, spr_a, 0);
    const int fast = (m->vdp[1] & 64) && !(m->vdp[1] & 24) && (ya >> 3) == (yb >> 3);
    if (!fast) {
        uint16_t second[SG_OUT_WIDTH];
        out_line(ya, out, spr_a, n_a);
        out_line(yb, second, spr_b, n_b);
        for (unsigned i = 0; i < SG_OUT_WIDTH; i++) out[i] = m0_avg2_panel(out[i], second[i]);
        return;
    }
    const uint16_t *pal = m->palette;
    mix_prepare(pal, pal[m->vdp[7] & 15]);
    const uint8_t *vram = m->vram;
    const uint8_t *names = vram + (((m->vdp[2] & 15u) << 10) + (ya >> 3) * 32), *pt, *ct;
    unsigned pmask = 0, cmask = 0, third = 0;
    const int g2 = (m->vdp[0] & 2) != 0;
    if (g2) {
        pt = vram + ((m->vdp[4] & 4u) << 11);
        ct = vram + ((m->vdp[3] & 128u) << 6);
        pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff;
        cmask = ((m->vdp[3] & 127u) << 6) | 63;
        third = ((ya & 0xc0) << 5) | (ya & 7);
    } else {
        pt = vram + ((m->vdp[4] & 7u) << 11) + (ya & 7);
        ct = vram + ((unsigned)m->vdp[3] << 6);
    }
    uint16_t *o = out;
    /* mean of two weights rounded up: (w_a + w_b + 1) / 2 as the byte offset
     * 2 * that, i.e. (w_a + w_b + 1) with its low bit cleared */
#if SG_MIX_SLOTS /* cb never takes ca's slot */
#define SG_ROWS_AB(ca, cb) const uint8_t *ma = mix_row(ca), *mb = mix_row_keep(cb, ma)
#elif SG_LOW_RAM /* ca's blends copied: with the cache, cb may evict them */
#define SG_ROWS_AB(ca, cb) uint16_t ra_[9]; const uint8_t *row_ = mix_row(ca); \
    for (unsigned i_ = 0; i_ < 9; i_++) ra_[i_] = ((const uint16_t *)row_)[i_]; \
    const uint8_t *ma = (const uint8_t *)ra_, *mb = mix_row(cb)
#else
#define SG_ROWS_AB(ca, cb) const uint8_t *ma = mix_row(ca), *mb = mix_row(cb)
#endif
#define SG_PAIR_TILE(pa, pb, ca, cb) do { \
        if (ca == cb) { \
            const uint32_t w = tile_wv[pa] + tile_wv[pb] + 0x0108421u; \
            const uint8_t *mc = mix_row(ca); \
            o[0] = *(const uint16_t *)(mc + (w & 30)); \
            o[1] = *(const uint16_t *)(mc + ((w >> 5) & 30)); \
            o[2] = *(const uint16_t *)(mc + ((w >> 10) & 30)); \
            o[3] = *(const uint16_t *)(mc + ((w >> 15) & 30)); \
            o[4] = *(const uint16_t *)(mc + ((w >> 20) & 30)); \
        } else { \
            const uint32_t wa = tile_w[pa], wb = tile_w[pb]; \
            SG_ROWS_AB(ca, cb); \
            for (unsigned k_ = 0; k_ < 5; k_++) \
                o[k_] = m0_avg2_panel(*(const uint16_t *)(ma + ((wa >> (5 * k_)) & 31)), *(const uint16_t *)(mb + ((wb >> (5 * k_)) & 31))); \
        } \
    } while (0)
#if SG_RENDER_ASM
    if (!SG_USE_C) {
        const sg_tile_args args = { out, names, pt, ct, SG_MIX_BASE, third, pmask, cmask };
        if (g2) sg_pair_g2(&args);
        else sg_pair_g1(&args);
    } else
#endif
    if (g2) {
        for (unsigned tx = 0; tx < 32; tx++, o += 5) {
            const unsigned off = names[tx] * 8u | third;
            const unsigned pa = pt[off & pmask], pb = pt[(off + 1) & pmask];
            const unsigned ca = ct[off & cmask], cb = ct[(off + 1) & cmask];
            SG_PAIR_TILE(pa, pb, ca, cb);
        }
    } else {
        for (unsigned tx = 0; tx < 32; tx++, o += 5) {
            const unsigned tile = names[tx];
            const unsigned pa = pt[tile * 8], pb = pt[tile * 8 + 1], c = ct[tile >> 3];
            SG_PAIR_TILE(pa, pb, c, c);
        }
    }
#undef SG_PAIR_TILE
#undef SG_ROWS_AB
    if (n_a || n_b) { /* tiles under sprites on either line: both lines composed */
        uint8_t over_a[SG_WIDTH] __attribute__((aligned(4))), over_b[SG_WIDTH] __attribute__((aligned(4)));
        uint32_t ta = n_a ? sprite_overlay(spr_a, n_a, over_a) : 0, tb = n_b ? sprite_overlay(spr_b, n_b, over_b) : 0;
        uint32_t touched = ta | tb;
        for (unsigned tx = 0; touched; tx++, touched >>= 1) {
            if (!(touched & 1)) continue;
            pair_filtered(ya, tx, (ta >> tx) & 1 ? over_a : 0, (tb >> tx) & 1 ? over_b : 0, out + tx * 5);
        }
    }
}

static void vdp_line(unsigned y) {
    sg1000_t *m = &machine;
    sprite_t spr[4];
    unsigned n = 0;
    if ((m->vdp[1] & 64) && !(m->vdp[1] & 16)) { /* sprites: display on, not text */
        n = line_sprites(y, spr, 1);
        collisions(spr, n);
        if (!(m->vdp_status & 64)) m->vdp_status = (uint8_t)((m->vdp_status & 0xe0) | 31);
    }
    if (m->row && m->row_out) {
        int draw = m->draw_line ? m->draw_line(m->platform, y) : 1;
        if (draw == 2 && y) out_pair(y - 1, m->row_out, spr, n); /* this line and the one before */
        else if (draw) out_line(y, m->row_out, spr, n);
        if (draw) m->row(m->platform, y, m->row_out);
    }
    if (m->ref_row) {
        uint8_t px[SG_WIDTH];
        ref_line(y, px, spr, n);
        m->ref_row(m->platform, y, px);
    }
    for (unsigned i = 0; i < n; i++) prev_spr[i] = spr[i];
    prev_n = n;
    prev_y = y;
}

void sg1000_vdp_catchup(void) {
    sg1000_t *m = &machine;
    while (m->render_line < SG_HEIGHT && (int32_t)(m->cyc - m->render_at) >= 0) {
        unsigned y = m->render_line++;
        m->render_at += SG_LINE_CYCLES;
        vdp_line(y);
    }
}

void sg1000_render_line(unsigned y, uint8_t *pixels) {
    sprite_t spr[4];
    unsigned n = (machine.vdp[1] & 64) && !(machine.vdp[1] & 16) ? line_sprites(y, spr, 0) : 0;
    ref_line(y, pixels, spr, n);
}

void sg1000_render_out(unsigned y, uint16_t *out) {
    sprite_t spr[4];
    unsigned n = (machine.vdp[1] & 64) && !(machine.vdp[1] & 16) ? line_sprites(y, spr, 0) : 0;
    out_line(y, out, spr, n);
}

void sg1000_render_pair(unsigned ya, uint16_t *out) {
    sprite_t spr[4];
    prev_y = ~0u;
    unsigned n = (machine.vdp[1] & 64) && !(machine.vdp[1] & 16) ? line_sprites(ya + 1, spr, 0) : 0;
    out_pair(ya, out, spr, n);
}

#ifdef SG_SELFTEST
/* Device renderer, Thumb-1 against C, on random VRAM (both graphics modes,
 * sprite sizes, backdrops): returns the number of differing lines. */
unsigned sg1000_render_selftest(void) {
    sg1000_t *m = &machine;
    uint32_t r = 12345;
    unsigned bad = 0;
    uint16_t a[SG_OUT_WIDTH], b[SG_OUT_WIDTH];
    for (unsigned round = 0; round < 8; round++) {
        for (unsigned i = 0; i < 16384; i++) { r = r * 1103515245u + 12345u; m->vram[i] = (uint8_t)(r >> 16); }
        if (round == 7) { /* blank patterns, one colour: the asm's fast tile and pair copies */
            memset(m->vram, 0, 0x2000);
            memset(m->vram + 0x2000, 0xe4, 0x1800);
        }
        for (unsigned i = 0; i < 32; i++) m->vram[0x3b00 + i * 4] = (uint8_t)(m->vram[0x3b00 + i * 4] % 200);
        if (round & 4) m->vram[0x3b00 + 6 * 4] = 208;
        m->vdp[0] = (round & 1) ? 2 : 0;
        m->vdp[1] = (uint8_t)(0x60 | ((round >> 1) & 3));
        m->vdp[2] = 0x0e; m->vdp[3] = (round & 1) ? 0xff : 0x80; m->vdp[4] = (round & 1) ? 3 : 0;
        m->vdp[5] = 0x76; m->vdp[6] = 0; m->vdp[7] = (uint8_t)(round * 3 + 1);
        m->sat_base = 0x3b00; m->sprites_dirty = 1;
        for (unsigned y = 0; y < SG_HEIGHT; y++) {
            sg_render_c = 0; sg1000_render_out(y, a);
            sg_render_c = 1; sg1000_render_out(y, b);
            bad += !!memcmp(a, b, sizeof a);
            if ((y & 7) != 7) {
                sg_render_c = 0; sg1000_render_pair(y, a);
                sg_render_c = 1; sg1000_render_pair(y, b);
                bad += !!memcmp(a, b, sizeof a);
            }
        }
    }
    sg_render_c = 0;
    return bad;
}
#endif
