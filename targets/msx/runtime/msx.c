/* MSX1 on the SG-1000 runtime: slots, PPI, AY-3-8910 registers, joystick and
 * keyboard matrix, and a simulated BIOS.
 *
 * No BIOS image is used or shipped.  The 512 bytes at 0000 (msx_bios) are
 * the jump table of the standard entry points, each a stub
 *     OUT (id),A ; RET
 * whose port is the routine's number: sg1000_out sends ports 00-3F here, and
 * the routine runs in C on the registers in machine.cpu, in the interpreter
 * and in recompiled code alike.  The interrupt handler at 0038 is real Z80
 * code (it calls the hooks H.KEYI and H.TIMI in RAM, which games patch).
 * Entry points not listed below are a bare RET.
 *
 * Covered: slot selection (RDSLT, WRSLT, ENASLT, CALSLT, CALLF, RSLREG,
 * WSLREG), VDP access (WRTVDP, RDVRM, WRTVRM, SETRD, SETWRT, FILVRM, LDIRMV,
 * LDIRVM, DISSCR, ENASCR, RDVDP, CLRSPR, CLS), screen modes 0-3 (CHGMOD,
 * INITXT, INIT32, INIGRP, INIMLT, SETTXT/SETT32/SETGRP/SETMLT, CHGCLR),
 * sound (GICINI, WRTPSG, RDPSG), input (GTSTCK, GTTRIG, SNSMAT, CHSNS,
 * CHGET, GTPAD, GTPDL) and the frame counter JIFFY.  Not covered: the BIOS
 * font (text modes show no glyphs), CHPUT and the other text routines, the
 * cassette, printer and disk routines, slot expansion, MSX2. */
#ifndef M0_TL
#define M0_TL   /* deepeel builds a thread-local flavour with -DM0_TL=__thread */
#endif
#include "sg1000.h"
#if SG_MSX
#include "m0_msx_layout.h"
#include "msx_font.h"
#include <string.h>
#ifdef MSX_TRACE
#include <stdio.h>
#endif

/* Where the cartridge's bytes come from.
 *   direct (default)  rd_win points at the ROM in flash: a read is a load.
 *   SG_MSX_PACKED     the ROM is in flash by 512-byte sectors, each raw or LZSS (records: blob offset |
 *                     codec << 30).  A raw sector is read in place; a packed one is decoded into a
 *                     small cache (m0_cache.h's way: the generator keeps the sectors the profile
 *                     reads often raw, the cache serves the cold ones).
 *   SG_MSX_TRACE      host profiling: reads of the cartridge are counted per sector and frame
 *                     (sg_host.c writes "# read" lines, the planner's weights, as for the Game Gear);
 *                     opcode fetches are not counted: the code the recompiler compiles is not read. */
#ifndef SG_MSX_PACKED
#define SG_MSX_PACKED 0
#endif
#ifdef SG_MSX_TRACE
uint32_t *msx_trace_count, *msx_trace_last;
M0_TL int msx_in_fetch;
#endif
#if SG_MSX_PACKED
#include "m0_rom_codec.h"
#ifndef MSX_CACHE_SECTORS
#define MSX_CACHE_SECTORS 3
#endif
extern const uint32_t msx_rom_records[];
extern const uint8_t msx_rom_blob[];
static uint8_t rom_cache[MSX_CACHE_SECTORS][512] __attribute__((aligned(4)));
static M0_TL uint32_t rom_cache_tag[MSX_CACHE_SECTORS];
M0_TL uint32_t msx_rom_decoded;

static uint8_t msx_rom_byte(uint32_t offset) {
    const uint32_t sector = offset >> 9, record = msx_rom_records[sector];
#ifdef SG_MSX_TRACE
    if (msx_trace_last && !msx_in_fetch && msx_trace_last[sector] != machine.frames + 1) { msx_trace_last[sector] = machine.frames + 1; msx_trace_count[sector]++; }
#endif
    if (!(record >> 30)) return msx_rom_blob[(record & 0x3fffffffu) + (offset & 511)];
    const uint32_t way = sector % MSX_CACHE_SECTORS;
    if (rom_cache_tag[way] != sector + 1) {
        m0_rom_unpack512(msx_rom_blob + (record & 0x3fffffffu), rom_cache[way]);
        rom_cache_tag[way] = sector + 1;
        msx_rom_decoded++;
    }
    return rom_cache[way][offset & 511];
}
#elif defined(SG_MSX_TRACE)
static uint8_t msx_rom_byte(uint32_t offset) {
    const uint32_t sector = offset >> 9;
    if (msx_trace_last && !msx_in_fetch && msx_trace_last[sector] != machine.frames + 1) { msx_trace_last[sector] = machine.frames + 1; msx_trace_count[sector]++; }
    return machine.rom[offset];
}
#endif

/* A byte of the cartridge: window w shows unit page[w]. */
static uint8_t cart_byte(unsigned w, uint16_t a) {
    const uint32_t offset = (uint32_t)machine.page[w] * M0_MSX_PAGE + (a & (M0_MSX_PAGE - 1));
#if SG_MSX_PACKED || defined(SG_MSX_TRACE)
    return msx_rom_byte(offset);
#else
    return machine.rom[offset];
#endif
}

static M0_TL int mapper_override = -1;
void msx_set_mapper(int mapper) { mapper_override = mapper; }
int msx_mapper_forced(void) { return mapper_override; }

void msx_ay_write(unsigned reg, uint8_t value);

#define Z (machine.cpu)

#ifdef SG_MSX_STATE_BYTES /* generated projects: the save-state slots are sized from this */
_Static_assert(sizeof(sg1000_t) <= SG_MSX_STATE_BYTES, "sg1000_t outgrew SG_MSX_STATE_BYTES (generator.cpp: msx_state_bytes)");
#endif

/* ---- the BIOS image ------------------------------------------------------- */

#include "m0_msx_bios.h"
enum {
#define X(name, address, id, direct) T_##name = id,
    M0_MSX_BIOS(X)
#undef X
    T_TICK = M0_MSX_BIOS_TICK
};
#define ENTRY(address, id) [address] = 0xd3, [(address) + 1] = (id), [(address) + 2] = 0xc9

#pragma GCC diagnostic ignored "-Woverride-init"
const uint8_t msx_bios[MSX_BIOS_BYTES] = {
    [0 ... MSX_BIOS_BYTES - 1] = 0xc9, /* anything else the BIOS would hold: RET */
    [0x04] = 0xbf, [0x05] = 0x1b,      /* CGTABL: the character set (msx_font) is at 1BBF, as in the MSX BIOS */
    [0x06] = 0x98, [0x07] = 0x98,      /* VDP read and write ports */
    [0x2b] = 0x00, [0x2c] = 0x00, [0x2d] = 0x00, [0x2e] = 0x00, /* 60 Hz, Japanese set, MSX1 */
#define X(name, address, id, direct) ENTRY(address, id),
    M0_MSX_BIOS(X)
#undef X
    [0x1c] = 0xdd, [0x1d] = 0xe9,      /* CALSLT: JP (IX); the slot is not switched */
    [0x38] = 0xc3, [0x39] = 0x70, [0x3a] = 0x01, /* KEYINT */
    /* KEYINT body, as the BIOS has it: every register saved (IX and IY too: games
     * run their logic in the H.TIMI hook, and keep object pointers in IX), H.KEYI, the
     * VDP status into STATFL, and on a frame interrupt H.TIMI and the JIFFY count */
    [0x170] = 0xe5, 0xd5, 0xc5, 0xf5,          /* PUSH HL, DE, BC, AF */
    0xd9, 0x08,                                 /* EXX; EX AF,AF' */
    0xe5, 0xd5, 0xc5, 0xf5,                     /* PUSH HL, DE, BC, AF (the other set) */
    0xfd, 0xe5, 0xdd, 0xe5,                     /* PUSH IY; PUSH IX */
    0xcd, 0x9a, 0xfd,                           /* CALL H.KEYI */
    0xdb, 0x99, 0xb7,                           /* IN A,(99); OR A */
    0x32, 0xe7, 0xf3,                           /* LD (STATFL),A */
    0xf2, 0x8f, 0x01,                           /* JP P,done */
    0xcd, 0x9f, 0xfd,                           /* CALL H.TIMI */
    0xd3, T_TICK,                               /* OUT: JIFFY++ */
    0xdd, 0xe1, 0xfd, 0xe1,                     /* done: POP IX; POP IY */
    0xf1, 0xc1, 0xd1, 0xe1,                     /* POP AF, BC, DE, HL */
    0x08, 0xd9,                                 /* EX AF,AF'; EXX */
    0xf1, 0xc1, 0xd1, 0xe1,                     /* POP AF, BC, DE, HL */
    0xfb, 0xc9,                                 /* EI; RET */
};
#pragma GCC diagnostic warning "-Woverride-init"

/* ---- work area (the BIOS variables games read) ---------------------------- */

enum {
    RG0SAV = 0xf3df, STATFL = 0xf3e7, FORCLR = 0xf3e9, BAKCLR = 0xf3ea, BDRCLR = 0xf3eb,
    SCRMOD = 0xfcaf, JIFFY = 0xfc9e, EXPTBL = 0xfcc1, RAMAD0 = 0xf341, HIMEM = 0xfc4a,
    LINL40 = 0xf3ae, LINL32 = 0xf3af, LINLEN = 0xf3b0, GETPNT = 0xf3fa, PUTPNT = 0xf3f8, OLDKEY = 0xfbda, NEWKEY = 0xfbe5, CRTCNT = 0xf3b1, CLIKSW = 0xf3db, CSRY = 0xf3dc, CSRX = 0xf3dd, H_KEYI = 0xfd9a,
    NAMBAS = 0xf922, CGPBAS = 0xf924, PATBAS = 0xf926, ATRBAS = 0xf928
};

/* Time the routines take; none while the machine is being set up. */
static M0_TL int booting;
static void spend(unsigned cycles) { if (!booting) machine.cyc += cycles; }

static uint8_t rd(uint16_t a) { return sg1000_read(a); }
static void wr(uint16_t a, uint8_t v) { sg1000_write(a, v); }
static void wr16(uint16_t a, uint16_t v) { wr(a, (uint8_t)v); wr((uint16_t)(a + 1), (uint8_t)(v >> 8)); }
static uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) | rd((uint16_t)(a + 1)) << 8); }

/* ---- slots ---------------------------------------------------------------- */

/* Primary slots: 0 BIOS (page 0), 1 the cartridge, 2 empty, 3 RAM.  A window
 * shows the cartridge when its slot is 1 and the cartridge has something
 * there (cart_mask); with ROM in flash the table points straight at it. */
void msx_remap(void) {
    sg1000_t *m = &machine;
    const unsigned ram_windows = SG_MSX_RAM / 0x2000;
    m->cart_vis = 0;
    for (unsigned w = 0; w < 8; w++) {
        const unsigned slot = (m->ppi_a >> ((w >> 1) * 2)) & 3;
        m->rd_win[w] = 0;
        m->wr_win[w] = 0;
        if (slot == 1 && w >= 2 && w < 6 && (m->cart_mask >> w & 1)) {
            m->cart_vis |= (uint8_t)(1u << w);
#if !SG_MSX_PACKED && !defined(SG_MSX_TRACE)
            m->rd_win[w] = m->rom + (uint32_t)m->page[w] * M0_MSX_PAGE;
#endif
        } else if (slot == 3 && w >= 8 - ram_windows) {
            m->rd_win[w] = m->wr_win[w] = m->ram + (w - (8 - ram_windows)) * 0x2000u;
        }
    }
}

/* A write that may reach a bank register of the cartridge. */
void msx_mapper_write(uint16_t a, uint8_t v) {
    sg1000_t *m = &machine;
    int window[6];
#ifdef MSX_TRACE
    if (a >= 0x4000 && a < 0xc000) fprintf(stderr, "rom write %04x=%02x pc=%04x vis=%02x\n", a, v, m->cpu.pc, m->cart_vis);
#endif
    if (a < 0x4000 || a >= 0xc000 || !(m->cart_vis >> (a >> 13) & 1)) return;
    if (m->msx_mapper == M0_MSX_KONAMI_SCC) {
        if (m->scc_on && a >= 0x9800 && a < 0xa000) { msx_scc_write(a & 0xff, v); return; } /* the SCC replaces the ROM there */
        if (a >= 0x9000 && a < 0x9800) m->scc_on = (v & 0x3f) == 0x3f; /* the bank register's value switches the sound chip on */
    }
    if (!m0_msx_bank_write(m->msx_mapper, a, v, m->rom_units, window)) return;
    for (unsigned w = 2; w < 6; w++) { /* only the windows that move: games write these registers often */
        if (window[w] < 0 || m->page[w] == window[w]) continue;
        m->page[w] = (uint16_t)window[w];
#if !SG_MSX_PACKED && !defined(SG_MSX_TRACE)
        if (m->cart_vis >> w & 1) m->rd_win[w] = m->rom + (uint32_t)window[w] * M0_MSX_PAGE;
#endif
    }
}

/* Reads that have no window: the BIOS (slot 0, page 0), a cartridge in
 * packed flash, and empty space. */
#define FONT_AT 0x1bbfu /* where the BIOS holds its character set; games copy it to VRAM */
static uint8_t bios_byte(uint16_t a) {
    if (a - FONT_AT < sizeof msx_font) return msx_font[a - FONT_AT];
    return a < MSX_BIOS_BYTES ? msx_bios[a] : 0xc9;
}
uint8_t msx_read_slow(uint16_t a) {
    if (a < 0x4000 && !(machine.ppi_a & 3)) return bios_byte(a);
    if (machine.cart_vis >> (a >> 13) & 1) return cart_byte(a >> 13, a);
    return 0xff;
}

static uint8_t slot_read(unsigned slot, uint16_t a) {
    sg1000_t *m = &machine;
    slot &= 3;
    if (slot == 0) return a < 0x4000 ? bios_byte(a) : 0xff;
    if (slot == 1) return a >= 0x4000 && a < 0xc000 && (m->cart_mask >> (a >> 13) & 1) ? cart_byte(a >> 13, a) : 0xff;
    if (slot == 3 && a >= 0x10000u - SG_MSX_RAM) return m->ram[a - (0x10000u - SG_MSX_RAM)];
    return 0xff;
}

static void slot_write(unsigned slot, uint16_t a, uint8_t v) {
    if ((slot & 3) == 3 && a >= 0x10000u - SG_MSX_RAM) machine.ram[a - (0x10000u - SG_MSX_RAM)] = v;
}

/* ---- VDP helpers (the data path is the SG-1000 one) ----------------------- */

static void vdp_address(uint16_t a, unsigned write) {
    msx_vdp_out(0x99, (uint8_t)a);
    msx_vdp_out(0x99, (uint8_t)(((a >> 8) & 63) | (write ? 64 : 0)));
}
static void vdp_put(uint8_t v) { msx_vdp_out(0x98, v); }
static void vdp_register(unsigned r, uint8_t v) {
    msx_vdp_out(0x99, v);
    msx_vdp_out(0x99, (uint8_t)(0x80 | r));
    wr((uint16_t)(RG0SAV + r), v);
}
/* A VRAM block, byte by byte through the data port. */
static void vdp_fill(uint16_t a, unsigned n, uint8_t v) {
    vdp_address(a, 1);
    while (n--) { vdp_put(v); spend(24); }
}

/* ---- screen modes --------------------------------------------------------- */

/* The BIOS keeps where each mode's tables are in variables (TXTNAM... MLTPAT, five
 * words a mode: name, colour, patterns, sprite attributes, sprite patterns) and builds
 * the VDP registers from them when a mode is set (INITXT, INIGRP, SETGRP...).  Games
 * relocate the tables by changing the variables before the call; the registers follow. */
typedef struct { uint8_t r[8]; uint16_t name, color, pattern, attr, sprites; } mode_t_;
#define TABLES(mode) ((uint16_t)(0xf3b3 + 10 * (mode)))
static const uint16_t table_defaults[4][5] = {
    { 0x0000, 0x0000, 0x0800, 0x0000, 0x0000 }, /* screen 0 */
    { 0x1800, 0x2000, 0x0000, 0x1b00, 0x3800 }, /* screen 1 */
    { 0x1800, 0x2000, 0x0000, 0x1b00, 0x3800 }, /* screen 2 */
    { 0x0800, 0x0000, 0x0000, 0x1b00, 0x3800 }, /* screen 3 */
};
static const uint8_t mode_bits[4][2] = { { 0x00, 0xf0 }, { 0x00, 0xe0 }, { 0x02, 0xe0 }, { 0x00, 0xe8 } }; /* R0, R1 */

static mode_t_ mode_of(unsigned mode) {
    mode &= 3;
    mode_t_ md = { { mode_bits[mode][0], mode_bits[mode][1], 0, 0, 0, 0, 0, 0 }, 0, 0, 0, 0, 0 };
    md.name = rd16(TABLES(mode)); md.color = rd16((uint16_t)(TABLES(mode) + 2)); md.pattern = rd16((uint16_t)(TABLES(mode) + 4));
    md.attr = rd16((uint16_t)(TABLES(mode) + 6)); md.sprites = rd16((uint16_t)(TABLES(mode) + 8));
    md.r[2] = (uint8_t)(md.name >> 10);
    md.r[3] = mode == 2 ? (uint8_t)(((md.color >> 6) & 0x80) | 0x7f) : (uint8_t)(md.color >> 6);
    md.r[4] = mode == 2 ? (uint8_t)(((md.pattern >> 11) & 4) | 3) : (uint8_t)(md.pattern >> 11);
    md.r[5] = (uint8_t)(md.attr >> 7);
    md.r[6] = (uint8_t)(md.sprites >> 11);
    return md;
}

static void clear_sprites(const mode_t_ *md) {
    vdp_address(md->attr, 1);
    for (unsigned i = 0; i < 32; i++) {
        vdp_put(209); vdp_put(0); vdp_put((uint8_t)i); vdp_put(rd(FORCLR) & 15);
    }
    vdp_fill(md->sprites, 256, 0);
    spend(32 * 40);
}

/* INITXT / INIT32 / INIGRP / INIMLT: the VDP registers, the tables and the
 * variables of the mode.  VRAM is cleared; there is no BIOS font to load. */
static void screen_init(unsigned mode) {
    const mode_t_ mdv = mode_of(mode), *md = &mdv;
    const uint8_t fore = rd(FORCLR) & 15, back = rd(BAKCLR) & 15, border = rd(BDRCLR) & 15;
    vdp_register(1, 0); /* blank while the tables are rebuilt */
    vdp_fill(0, 16384, 0);
    for (unsigned r = 0; r < 8; r++) {
        uint8_t v = md->r[r];
        if (r == 1) v = (uint8_t)((v & 0xfc) | (rd((uint16_t)(RG0SAV + 1)) & 3)); /* sprite size is kept */
        if (r == 7) v = mode == 0 ? (uint8_t)(fore << 4 | back) : border;
        if (r != 1) vdp_register(r, v);
        else wr((uint16_t)(RG0SAV + 1), v);
    }
    switch (mode & 3) {
    case 0: vdp_fill(md->name, 960, 0x20); break;
    case 1:
        vdp_fill(md->name, 768, 0x20);
        vdp_fill(md->color, 32, (uint8_t)(fore << 4 | back));
        break;
    case 2:
        vdp_address(md->name, 1);
        for (unsigned i = 0; i < 768; i++) { vdp_put((uint8_t)i); spend(24); }
        vdp_fill(md->color, 6144, (uint8_t)(fore << 4 | back));
        break;
    default:
        vdp_address(md->name, 1);
        for (unsigned row = 0; row < 24; row++)
            for (unsigned col = 0; col < 32; col++) { vdp_put((uint8_t)((row / 4) * 32 + col)); spend(24); }
        break;
    }
    if (mode != 0) clear_sprites(md); /* the text screen has no sprites */
    if ((mode & 3) < 2) { /* the text modes' glyphs */
        vdp_address(md->pattern, 1);
        for (unsigned i = 0; i < sizeof msx_font; i++) { vdp_put(msx_font[i]); spend(24); }
    }
    wr(CSRX, 1); wr(CSRY, 1);
    wr(LINLEN, rd(mode == 0 ? LINL40 : LINL32));
    vdp_register(1, (uint8_t)(md->r[1] & 0xfc) | (rd((uint16_t)(RG0SAV + 1)) & 3));
    wr(SCRMOD, (uint8_t)mode);
    wr(NAMBAS, (uint8_t)md->name); wr((uint16_t)(NAMBAS + 1), (uint8_t)(md->name >> 8));
    wr(CGPBAS, (uint8_t)md->pattern); wr((uint16_t)(CGPBAS + 1), (uint8_t)(md->pattern >> 8));
    wr(PATBAS, (uint8_t)md->sprites); wr((uint16_t)(PATBAS + 1), (uint8_t)(md->sprites >> 8));
    wr(ATRBAS, (uint8_t)md->attr); wr((uint16_t)(ATRBAS + 1), (uint8_t)(md->attr >> 8));
}

/* SETTXT & co: the registers of the mode only, VRAM untouched. */
static void screen_set(unsigned mode) {
    const mode_t_ mdv = mode_of(mode), *md = &mdv;
    for (unsigned r = 0; r < 8; r++) {
        uint8_t v = md->r[r];
        if (r == 1) v = (uint8_t)((v & 0xfc) | (rd((uint16_t)(RG0SAV + 1)) & 3));
        if (r == 7) v = mode == 0 ? (uint8_t)((rd(FORCLR) & 15) << 4 | (rd(BAKCLR) & 15)) : rd(BDRCLR) & 15;
        vdp_register(r, v);
    }
    wr(SCRMOD, (uint8_t)mode);
}

static void change_colors(void) {
    const unsigned mode = rd(SCRMOD) & 3;
    const uint8_t fore = rd(FORCLR) & 15, back = rd(BAKCLR) & 15, border = rd(BDRCLR) & 15;
    if (mode == 0) { vdp_register(7, (uint8_t)(fore << 4 | back)); return; }
    vdp_register(7, border);
    if (mode == 1) vdp_fill(mode_of(1).color, 32, (uint8_t)(fore << 4 | back));
    else if (mode == 2) vdp_fill(mode_of(2).color, 6144, (uint8_t)(fore << 4 | back));
}

/* ---- text ------------------------------------------------------------------ */

/* CHPUT in the text modes (0 and 1): printable characters at the cursor
 * (CSRX, CSRY, from 1), CR, LF, BS, TAB, CLS (12), scrolling at the bottom. */
static void text_clear(const mode_t_ *md, unsigned cells) { vdp_fill(md->name, cells, 0x20); }

static void text_scroll(const mode_t_ *md, unsigned columns, unsigned stride) {
    uint8_t line[40];
    for (unsigned row = 1; row < 24; row++) {
        vdp_address((uint16_t)(md->name + row * stride), 0);
        for (unsigned x = 0; x < columns; x++) { line[x] = msx_vdp_in(0x98); spend(24); }
        vdp_address((uint16_t)(md->name + (row - 1) * stride), 1);
        for (unsigned x = 0; x < columns; x++) { vdp_put(line[x]); spend(24); }
    }
    vdp_fill((uint16_t)(md->name + 23 * stride), columns, 0x20);
}

static void put_char(uint8_t c) {
    const unsigned mode = rd(SCRMOD) & 3;
    if (mode > 1) return; /* graphics modes print through GRPPRT, not provided */
    const mode_t_ mdv = mode_of(mode), *md = &mdv;
    const unsigned stride = mode ? 32 : 40, columns = rd(LINLEN);
    unsigned x = rd(CSRX), y = rd(CSRY);
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (c == 12) { text_clear(md, 24 * stride); x = y = 1; }
    else if (c == 13) x = 1;
    else if (c == 10) y++;
    else if (c == 8) { if (x > 1) x--; }
    else if (c == 9) x = ((x + 7) & ~7u) | 1;
    else if (c == 11) x = y = 1;
    else if (c == 28) x++;
    else if (c == 29) { if (x > 1) x--; }
    else if (c == 30) { if (y > 1) y--; }
    else if (c == 31) y++;
    else if (c >= 32 || c == 1) {
        vdp_address((uint16_t)(md->name + (y - 1) * stride + (x - 1)), 1);
        vdp_put(c);
        spend(24);
        x++;
    }
    if (x > columns) { x = 1; y++; }
    if (y > 24) { text_scroll(md, columns, stride); y = 24; }
    wr(CSRX, (uint8_t)x);
    wr(CSRY, (uint8_t)y);
}

/* ---- input ---------------------------------------------------------------- */

/* Pad bits as sg1000_buttons: 1 up, 2 down, 4 left, 8 right, 16 button 1, 32 button 2. */
static uint8_t key_row(unsigned row) {
    sg1000_t *m = &machine;
    uint8_t v = 0xff;
    if (row == 7 && m->pause) v &= (uint8_t)~0x80;                 /* RETURN */
    if (row == 8) {
        const uint8_t b = m->buttons[0];
        if (b & 16) v &= (uint8_t)~0x01;                           /* SPACE */
        if (b & 4) v &= (uint8_t)~0x10;                            /* LEFT */
        if (b & 1) v &= (uint8_t)~0x20;                            /* UP */
        if (b & 2) v &= (uint8_t)~0x40;                            /* DOWN */
        if (b & 8) v &= (uint8_t)~0x80;                            /* RIGHT */
    }
    return v;
}

static unsigned stick_direction(uint8_t b) {
    static const uint8_t table[16] = { 0, 1, 5, 0, 7, 8, 6, 7, 3, 2, 4, 3, 0, 1, 5, 0 };
    return table[b & 15];
}

/* AY register read: R14 is joystick port 1 or 2 (R15 bit 6), active low;
 * bit 6 reads high (no JIS pad), bit 7 the idle cassette input. */
static uint8_t ay_read(unsigned r) {
    sg1000_t *m = &machine;
    if (r == 14) return (uint8_t)(0xc0 | (~m->buttons[(m->ay_reg[15] >> 6) & 1] & 0x3f));
    return m->ay_reg[r & 15];
}

/* ---- the BIOS routines ---------------------------------------------------- */

static void set_flags_a(uint8_t v) { Z.f = (uint8_t)(Z.f & Z80_C) | (v & 0x80) | (v ? 0 : Z80_Z); }

static void trap(unsigned id) {
    sg1000_t *m = &machine;
    m->msx_calls++;
#ifdef MSX_TRACE /* host debugging: every BIOS call with its registers */
    fprintf(stderr, "bios %02x a=%02x bc=%04x de=%04x hl=%04x pc=%04x sp=%04x f=%u\n", id, Z.a, Z.bc, Z.de, Z.hl, Z.pc, Z.sp, machine.frames);
#endif
    spend(60); /* a call into the BIOS is not free */
    switch (id) {
    case T_RDSLT: Z.a = slot_read(Z.a, Z.hl); Z.iff1 = Z.iff2 = 0; break;
    case T_WRSLT: slot_write(Z.a, Z.hl, Z.e); Z.iff1 = Z.iff2 = 0; break;
    case T_ENASLT: {
        const unsigned page = Z.h >> 6, shift = page * 2;
        m->ppi_a = (uint8_t)((m->ppi_a & ~(3u << shift)) | ((Z.a & 3u) << shift));
        msx_remap();
        Z.iff1 = Z.iff2 = 0;
        break;
    }
    case T_CALLF: { /* RST 30h; DB slot; DW address: the slot is not switched */
        const uint16_t data = rd16(Z.sp), target = rd16((uint16_t)(data + 1));
        wr16(Z.sp, (uint16_t)(data + 3));  /* where the callee returns to */
        Z.sp = (uint16_t)(Z.sp - 2);
        wr16(Z.sp, target);                /* the stub's RET goes there */
        break;
    }
    case T_DCOMPR:
        Z.f = (uint8_t)((Z.hl == Z.de ? Z80_Z : 0) | (Z.hl < Z.de ? Z80_C : 0) | Z80_N);
        break;
    case T_DISSCR: vdp_register(1, (uint8_t)(rd(RG0SAV + 1) & ~0x40)); break;
    case T_ENASCR: vdp_register(1, (uint8_t)(rd(RG0SAV + 1) | 0x40)); break;
    case T_WRTVDP: vdp_register(Z.c & 7, Z.b); break;
    case T_RDVRM: vdp_address(Z.hl, 0); Z.a = msx_vdp_in(0x98); break;
    case T_WRTVRM: vdp_address(Z.hl, 1); vdp_put(Z.a); break;
    case T_SETRD: vdp_address(Z.hl, 0); break;
    case T_SETWRT: vdp_address(Z.hl, 1); break;
    case T_FILVRM: {
        unsigned n = Z.bc;
        vdp_address(Z.hl, 1);
        while (n--) { vdp_put(Z.a); spend(24); }
        Z.bc = 0;
        break;
    }
    case T_LDIRMV: { /* VRAM HL -> RAM DE: DE moves on, HL stays, BC ends as 0098 (B 0, C the VDP port) */
        vdp_address(Z.hl, 0);
        while (Z.bc) { wr(Z.de++, msx_vdp_in(0x98)); Z.bc--; spend(40); }
        Z.bc = 0x98;
        break;
    }
    case T_LDIRVM: { /* RAM HL -> VRAM DE: afterwards HL is the VRAM start, DE the end of the source, as the BIOS leaves them */
        const uint16_t vram = Z.de;
        vdp_address(vram, 1);
        while (Z.bc) { vdp_put(rd(Z.hl++)); Z.bc--; spend(40); }
        Z.de = Z.hl;
        Z.hl = vram;
        Z.bc = 0x98;
        Z.a = 0;
        break;
    }
    case T_CHGMOD: if (Z.a < 4) screen_init(Z.a); break;
    case T_CHGCLR: change_colors(); break;
    case T_CLRSPR: { const mode_t_ md = mode_of(rd(SCRMOD)); clear_sprites(&md); break; }
    case T_INITXT: screen_init(0); break;
    case T_INIT32: screen_init(1); break;
    case T_INIGRP: screen_init(2); break;
    case T_INIMLT: screen_init(3); break;
    case T_SETTXT: screen_set(0); break;
    case T_SETT32: screen_set(1); break;
    case T_SETGRP: screen_set(2); break;
    case T_SETMLT: screen_set(3); break;
    case T_GICINI:
        for (unsigned r = 0; r < 14; r++) msx_ay_write(r, r == 7 ? 0xb8 : 0);
        msx_ay_write(15, 0x8f);
        break;
    case T_WRTPSG: msx_ay_write(Z.a & 15, Z.e); break;
    case T_RDPSG: Z.a = ay_read(Z.a & 15); break;
    case T_CLS: {
        const unsigned mode = rd(SCRMOD) & 3;
        if (mode < 2) vdp_fill(mode_of(mode).name, mode ? 768 : 960, 0x20);
        break;
    }
    case T_GTSTCK: Z.a = (uint8_t)stick_direction(m->buttons[Z.a == 2 ? 1 : 0]); break;
    case T_GTTRIG: {
        /* 0 space, 1/2 joystick 1/2 button A, 3/4 button B */
        const unsigned n = Z.a;
        const uint8_t b = n == 0 || n == 1 ? m->buttons[0] : n == 2 ? m->buttons[1] : n == 3 ? m->buttons[0] >> 1 : m->buttons[1] >> 1;
        Z.a = b & 16 ? 0xff : 0;
        set_flags_a(Z.a);
        break;
    }
    case T_GTPAD: Z.a = 0; break;
    case T_RSLREG: Z.a = m->ppi_a; break;
    case T_WSLREG: m->ppi_a = Z.a; msx_remap(); break;
    case T_RDVDP: Z.a = msx_vdp_in(0x99); break;
    case T_SNSMAT: Z.a = key_row(Z.a); break;
    case T_CHSNS: Z.f = (uint8_t)((m->buttons[0] & 0x1f) || m->pause ? 0 : Z80_Z); break;
    case T_CHGET: {
        const uint8_t b = m->buttons[0];
        Z.a = m->pause ? 13 : b & 16 ? 32 : b & 1 ? 30 : b & 2 ? 31 : b & 4 ? 29 : b & 8 ? 28 : 0;
        break;
    }
    case T_CHPUT: put_char(Z.a); break;
    case T_POSIT: wr(CSRX, Z.h); wr(CSRY, Z.l); break;
    case T_TICK:
        wr16(JIFFY, (uint16_t)(rd16(JIFFY) + 1));
        for (unsigned r = 0; r < 11; r++) { /* the interrupt scans the keyboard into NEWKEY (games read it) */
            wr((uint16_t)(OLDKEY + r), rd((uint16_t)(NEWKEY + r)));
            wr((uint16_t)(NEWKEY + r), key_row(r));
        }
        break;
    default: break;
    }
}

/* ---- ports ---------------------------------------------------------------- */

uint8_t sg1000_in(uint8_t port) {
    sg1000_t *m = &machine;
    switch (port) {
    case 0x98: case 0x99: return msx_vdp_in(port);
    case 0xa2: return ay_read(m->ay_latch);
    case 0xa8: return m->ppi_a;
    case 0xa9: return key_row(m->ppi_c & 15);
    case 0xaa: return m->ppi_c;
    default: return 0xff;
    }
}

/* PPI port C: the keyboard row, and bit 7, the click: the samples up to now keep the old level. */
static void set_ppi_c(uint8_t v) {
    sg1000_t *m = &machine;
    if ((v ^ m->ppi_c) & 0x80) {
        sg1000_audio_catchup();
        m->click = v >> 7;
        m->click_used = 1;
    }
    m->ppi_c = v;
}

void sg1000_out(uint8_t port, uint8_t v) {
    sg1000_t *m = &machine;
    if (port == 0x98) { msx_vdp_data(v); return; } /* the most frequent: pattern, name and colour bytes */
    if (port < 0x40) { trap(port); return; }
    switch (port) {
    case 0x98: case 0x99: msx_vdp_out(port, v); break;
    case 0xa0: m->ay_latch = v & 15; break;
    case 0xa1: msx_ay_write(m->ay_latch, v); break;
    case 0xa8: m->ppi_a = v; msx_remap(); break;
    case 0xaa: set_ppi_c(v); break;
    case 0xab:
        if (!(v & 0x80)) set_ppi_c((uint8_t)((v & 1) ? m->ppi_c | (1u << ((v >> 1) & 7)) : m->ppi_c & ~(1u << ((v >> 1) & 7))));
        break;
    default: break; /* printer, clock, MSX-MUSIC...: not present */
    }
}

/* ---- boot ----------------------------------------------------------------- */

/* The state the BIOS leaves when it hands over to the cartridge's INIT: page
 * 1 (or 2) on the cartridge, RAM above, the hooks as RET, the VDP in
 * screen 1. */
void msx_boot(void) {
    sg1000_t *m = &machine;
    /* the windows the cartridge occupies and the unit each shows at power on */
    m->cart_mask = 0;
    for (unsigned w = 2; w < 6; w++) {
        if (m->msx_mapper) {
            m->page[w] = (uint16_t)(m0_msx_initial_unit(m->msx_mapper, w) % (m->rom_units ? m->rom_units : 1));
            m->cart_mask |= (uint8_t)(1u << w);
        } else if (w * 0x2000u >= m->msx_base && w * 0x2000u - m->msx_base < (uint32_t)m->rom_units * M0_MSX_PAGE) {
            m->page[w] = (uint16_t)((w * 0x2000u - m->msx_base) / M0_MSX_PAGE);
            m->cart_mask |= (uint8_t)(1u << w);
        }
    }
    m->ppi_a = m->msx_base == 0x8000 && !m->msx_mapper ? 0xd0 : 0xf4;
#if SG_MSX_PACKED
    memset(rom_cache_tag, 0, sizeof rom_cache_tag);
#endif
    m->ppi_c = 0;
    m->ay_latch = 0;
    msx_remap();
    for (unsigned a = H_KEYI; a < 0xffc9; a++) wr((uint16_t)a, 0xc9);
    wr(RAMAD0, 0); wr(RAMAD0 + 1, 0); wr(RAMAD0 + 2, 3); wr(RAMAD0 + 3, 3);
    for (unsigned mode = 0; mode < 4; mode++) /* the table variables the BIOS starts with */
        for (unsigned k = 0; k < 5; k++) wr16((uint16_t)(TABLES(mode) + 2 * k), table_defaults[mode][k]);
    wr(LINL40, 39); wr(LINL32, 32); wr(LINLEN, 32);
    for (unsigned r = 0; r < 11; r++) { wr((uint16_t)(OLDKEY + r), 0xff); wr((uint16_t)(NEWKEY + r), 0xff); } /* keyboard: nothing pressed */
    wr16(PUTPNT, 0xfbf0); wr16(GETPNT, 0xfbf0); wr(CRTCNT, 24); wr(CLIKSW, 0);
    wr(FORCLR, 15); wr(BAKCLR, 4); wr(BDRCLR, 4);
    wr(HIMEM, 0x80); wr(HIMEM + 1, 0xf3);
    for (unsigned r = 0; r < 14; r++) m->ay_reg[r] = r == 7 ? 0xb8 : 0;
    m->ay_reg[15] = 0x8f;
    m->ay_lfsr = 1;
    m->scc_on = m->scc_enable = m->click_used = m->click = 0;
    memset(m->scc_vol, 0, sizeof m->scc_vol);
    wr(RG0SAV + 1, 0xe0);
    booting = 1; /* the boot costs no emulated time */
    screen_init(1);
    booting = 0;
    m->vdp_pending = 0;
    m->sprites_dirty = 1;
    m->vdp_status = 31;
    Z.sp = 0xf380;
    Z.im = 1;
    Z.iff1 = Z.iff2 = 1;
    Z.pc = m->msx_init ? m->msx_init : m->msx_base;
}

void msx_state_loaded(void) { msx_remap(); }
#endif
