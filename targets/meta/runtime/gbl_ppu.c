/*
 * gbl PPU: lazy DMG scanline renderer.
 *
 * The PPU is advanced only when an event is due or a PPU register is touched.
 * A visible line is rasterized when the PPU is synchronized past the end of
 * its mode 3, so register writes made before HBlank (including late writes
 * from STAT/LYC handlers, which real hardware picks up mid-fetch) apply to the
 * line, and HBlank writes apply to the next one.  Mode 3 has a fixed 172-dot
 * length.
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_internal.h"
#include "gbl_timing_layout.h"

#include <stddef.h>
#include <string.h>

#define LCDC gbl_io[0x40]
#define STAT gbl_io[0x41]
#define SCY gbl_io[0x42]
#define SCX gbl_io[0x43]
#define LYC gbl_io[0x45]
#define BGP gbl_io[0x47]
#define OBP0 gbl_io[0x48]
#define OBP1 gbl_io[0x49]
#define WY gbl_io[0x4A]
#define WX gbl_io[0x4B]
#define IF gbl_io[0x0F]

#define LINE_CYCLES 456u
#define MODE3_START 80u
#define MODE0_START 252u
#define LY153_CMP 8u /* line 153: LYC sees LY=0 from here (DMG, SameBoy reference) */
#define FRAME_CYCLES 70224u

uint8_t gbl_skip_render;
uint16_t gbl_lcd_palette[4] = {0, 1, 2, 3};

/* PPU timing state in one structure: Thumb-1 reaches every field from one
 * base register instead of loading a literal address per variable.  The
 * assembly state machine (gbl_timing_asm.S) uses the offsets of
 * gbl_timing_layout.h, checked below. */
typedef struct {
    uint32_t line_start;     /* LCD on: cycle the current line began */
    uint32_t off_start;      /* LCD off: start of the current virtual frame */
    uint32_t next_stage_off; /* offset in the line of the next transition */
    uint8_t ly;
    uint8_t stage;           /* 0: before mode 3, 1: in mode 3, 2: in mode 0 */
    uint8_t mode;
    uint8_t stat_line;
    uint8_t win_line;
    uint8_t lcd_on;
    uint8_t moved;           /* set by any transition (gbl_ppu_read) */
    uint8_t stat_modes;      /* STAT bits 3-5: bit m = interrupt on mode m */
    uint8_t lyc_line;        /* LYC interrupt enabled and LY == LYC */
    uint8_t spr_ly, spr_lo, spr_hi, spr_h; /* select_sprites window, valid for spr_ly */
    uint8_t evt_dirty;       /* dev_next must be recomputed */
    uint32_t dev_next;       /* earliest device event (gbl_update_evt) */
    uint16_t m0_start;       /* C line state machine: this line's mode 0 offset */
} gbl_ppu_state_t;
gbl_ppu_state_t gbl_ppu_state = {.next_stage_off = 80u, .evt_dirty = 1, .m0_start = 252u};
#define P gbl_ppu_state

#define GBL_CHECK_OFS(type, field, ofs) _Static_assert(offsetof(type, field) == ofs, #field)
GBL_CHECK_OFS(gbl_ppu_state_t, line_start, GBL_P_LINE_START);
GBL_CHECK_OFS(gbl_ppu_state_t, off_start, GBL_P_OFF_START);
GBL_CHECK_OFS(gbl_ppu_state_t, next_stage_off, GBL_P_NEXT_STAGE_OFF);
GBL_CHECK_OFS(gbl_ppu_state_t, ly, GBL_P_LY);
GBL_CHECK_OFS(gbl_ppu_state_t, stage, GBL_P_STAGE);
GBL_CHECK_OFS(gbl_ppu_state_t, mode, GBL_P_MODE);
GBL_CHECK_OFS(gbl_ppu_state_t, stat_line, GBL_P_STAT_LINE);
GBL_CHECK_OFS(gbl_ppu_state_t, win_line, GBL_P_WIN_LINE);
GBL_CHECK_OFS(gbl_ppu_state_t, lcd_on, GBL_P_LCD_ON);
GBL_CHECK_OFS(gbl_ppu_state_t, moved, GBL_P_MOVED);
GBL_CHECK_OFS(gbl_ppu_state_t, stat_modes, GBL_P_STAT_MODES);
GBL_CHECK_OFS(gbl_ppu_state_t, lyc_line, GBL_P_LYC_LINE);
GBL_CHECK_OFS(gbl_ppu_state_t, spr_ly, GBL_P_SPR_LY);
GBL_CHECK_OFS(gbl_ppu_state_t, spr_lo, GBL_P_SPR_LO);
GBL_CHECK_OFS(gbl_ppu_state_t, spr_hi, GBL_P_SPR_HI);
GBL_CHECK_OFS(gbl_ppu_state_t, spr_h, GBL_P_SPR_H);
GBL_CHECK_OFS(gbl_ppu_state_t, evt_dirty, GBL_P_EVT_DIRTY);
GBL_CHECK_OFS(gbl_ppu_state_t, dev_next, GBL_P_DEV_NEXT);
GBL_CHECK_OFS(gbl_cpu_t, ime, GBL_GBR_IME);
GBL_CHECK_OFS(gbl_cpu_t, cyc, GBL_GBR_CYC);
GBL_CHECK_OFS(gbl_cpu_t, next_evt, GBL_GBR_NEXT_EVT);
#define line_start P.line_start
#define off_start P.off_start
#define next_stage_off P.next_stage_off
#define ly P.ly
#define stage P.stage
#define mode P.mode
#define stat_line P.stat_line
#define win_line P.win_line
#define lcd_on P.lcd_on
#define ppu_moved P.moved

uint8_t gbl_oam_dirty = 1;
/* Global for the assembly render_line (gbl_ppu_asm.S). */
uint8_t gbl_ppu_oam_by_y[40];
uint8_t gbl_ppu_y_sorted[41]; /* OAM Y in oam_by_y order, then a 0xFF sentinel */
#define oam_by_y gbl_ppu_oam_by_y
#define y_sorted gbl_ppu_y_sorted
static uint16_t bg[20];      /* packed BG/window colour indices per 8-pixel group */
static uint32_t bg_pairs[16]; /* two BGP-mapped pixels per 4-bit index */
/* Sprite colours per palette (OBP0, OBP1), rebuilt when OBP0/OBP1 change
 * (the key) or the frontend palette does (gbl_ppu_palette_changed voids
 * the key). */
static uint16_t obj_pal[2][4];
static uint32_t obj_pal_key = 0xFFFFFFFFu;
static int16_t bg_pairs_bgp = -1;

/* spread[b] moves bit i of b to bit 2i, so spread[lo] | spread[hi] << 1 packs
 * a tile row as eight 2-bit colour indices, leftmost pixel in bits 15:14. */
#define spread gbl_ppu_spread
const uint16_t gbl_ppu_spread[256] = {
    0x0000, 0x0001, 0x0004, 0x0005, 0x0010, 0x0011, 0x0014, 0x0015,
    0x0040, 0x0041, 0x0044, 0x0045, 0x0050, 0x0051, 0x0054, 0x0055,
    0x0100, 0x0101, 0x0104, 0x0105, 0x0110, 0x0111, 0x0114, 0x0115,
    0x0140, 0x0141, 0x0144, 0x0145, 0x0150, 0x0151, 0x0154, 0x0155,
    0x0400, 0x0401, 0x0404, 0x0405, 0x0410, 0x0411, 0x0414, 0x0415,
    0x0440, 0x0441, 0x0444, 0x0445, 0x0450, 0x0451, 0x0454, 0x0455,
    0x0500, 0x0501, 0x0504, 0x0505, 0x0510, 0x0511, 0x0514, 0x0515,
    0x0540, 0x0541, 0x0544, 0x0545, 0x0550, 0x0551, 0x0554, 0x0555,
    0x1000, 0x1001, 0x1004, 0x1005, 0x1010, 0x1011, 0x1014, 0x1015,
    0x1040, 0x1041, 0x1044, 0x1045, 0x1050, 0x1051, 0x1054, 0x1055,
    0x1100, 0x1101, 0x1104, 0x1105, 0x1110, 0x1111, 0x1114, 0x1115,
    0x1140, 0x1141, 0x1144, 0x1145, 0x1150, 0x1151, 0x1154, 0x1155,
    0x1400, 0x1401, 0x1404, 0x1405, 0x1410, 0x1411, 0x1414, 0x1415,
    0x1440, 0x1441, 0x1444, 0x1445, 0x1450, 0x1451, 0x1454, 0x1455,
    0x1500, 0x1501, 0x1504, 0x1505, 0x1510, 0x1511, 0x1514, 0x1515,
    0x1540, 0x1541, 0x1544, 0x1545, 0x1550, 0x1551, 0x1554, 0x1555,
    0x4000, 0x4001, 0x4004, 0x4005, 0x4010, 0x4011, 0x4014, 0x4015,
    0x4040, 0x4041, 0x4044, 0x4045, 0x4050, 0x4051, 0x4054, 0x4055,
    0x4100, 0x4101, 0x4104, 0x4105, 0x4110, 0x4111, 0x4114, 0x4115,
    0x4140, 0x4141, 0x4144, 0x4145, 0x4150, 0x4151, 0x4154, 0x4155,
    0x4400, 0x4401, 0x4404, 0x4405, 0x4410, 0x4411, 0x4414, 0x4415,
    0x4440, 0x4441, 0x4444, 0x4445, 0x4450, 0x4451, 0x4454, 0x4455,
    0x4500, 0x4501, 0x4504, 0x4505, 0x4510, 0x4511, 0x4514, 0x4515,
    0x4540, 0x4541, 0x4544, 0x4545, 0x4550, 0x4551, 0x4554, 0x4555,
    0x5000, 0x5001, 0x5004, 0x5005, 0x5010, 0x5011, 0x5014, 0x5015,
    0x5040, 0x5041, 0x5044, 0x5045, 0x5050, 0x5051, 0x5054, 0x5055,
    0x5100, 0x5101, 0x5104, 0x5105, 0x5110, 0x5111, 0x5114, 0x5115,
    0x5140, 0x5141, 0x5144, 0x5145, 0x5150, 0x5151, 0x5154, 0x5155,
    0x5400, 0x5401, 0x5404, 0x5405, 0x5410, 0x5411, 0x5414, 0x5415,
    0x5440, 0x5441, 0x5444, 0x5445, 0x5450, 0x5451, 0x5454, 0x5455,
    0x5500, 0x5501, 0x5504, 0x5505, 0x5510, 0x5511, 0x5514, 0x5515,
    0x5540, 0x5541, 0x5544, 0x5545, 0x5550, 0x5551, 0x5554, 0x5555
};

static const uint8_t reverse8[256] = {
    0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0,
    0x30, 0xb0, 0x70, 0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
    0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4,
    0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
    0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc,
    0x3c, 0xbc, 0x7c, 0xfc, 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
    0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a, 0x8a, 0x4a, 0xca,
    0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
    0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6,
    0x36, 0xb6, 0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
    0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe, 0x01, 0x81, 0x41, 0xc1,
    0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
    0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9,
    0x39, 0xb9, 0x79, 0xf9, 0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
    0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5, 0x0d, 0x8d, 0x4d, 0xcd,
    0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
    0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3,
    0x33, 0xb3, 0x73, 0xf3, 0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
    0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb, 0x07, 0x87, 0x47, 0xc7,
    0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
    0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf,
    0x3f, 0xbf, 0x7f, 0xff
};

/* STAT interrupt line: the LYC source or the source of the current mode
 * (bits 3-5 for modes 0-2; mode 3 has none).  Rising edges request IF bit 1. */
/* Both inputs are precomputed: the mode sources when STAT is written, the
 * LYC match when LY, LYC or STAT change (stat_refresh).  Mode 3 shifts the
 * three source bits out: no source. */
static inline __attribute__((always_inline)) void stat_update(void) {
    uint8_t line = (uint8_t)(((P.stat_modes >> mode) & 1u) | P.lyc_line);
    if (line > stat_line) IF |= 0x02; /* rising edge; IF is rarely written */
    stat_line = line;
}

/* LY as the CPU and the LYC comparator see it.  On the DMG, line 153 reads
 * 153 only for its first cycles, then 0 for the rest of the line (SameBoy
 * reference): stage 1 of line 153 (C line state machine) is that part. */
static inline __attribute__((always_inline)) uint8_t ly_visible(void) {
    return (uint8_t)(ly == 153 && stage ? 0 : ly);
}

static inline __attribute__((always_inline)) void stat_refresh(void) {
    uint8_t s = STAT;
    P.stat_modes = (uint8_t)((s >> 3) & 7u);
    P.lyc_line = (uint8_t)((s & 0x40) && ly_visible() == LYC);
}

static inline __attribute__((always_inline)) void lyc_refresh(void) {
    P.lyc_line = (uint8_t)((STAT & 0x40) && ly_visible() == LYC);
}

/* Tile data addressing without per-tile branches: in 0x8800 mode the signed
 * index s maps to 0x1000 + 16*s = 0x800 + 16*(idx ^ 0x80). */
#if GBL_PPU_ASM
/* gbl_ppu_asm.S; long calls reach SRAM from flash. */
#if GBL_PPU_RAMFUNC
#define GBL_ASM_CALL __attribute__((long_call))
#else
#define GBL_ASM_CALL
#endif
GBL_ASM_CALL void gbl_ppu_fetch_asm(uint32_t *t, const uint8_t *map, uint32_t n,
                                    const uint8_t *trow, uint32_t idx_xor);
GBL_ASM_CALL void gbl_ppu_convert_asm(uint32_t *out, const uint16_t *bg,
                                      const uint32_t *pairs, uint32_t groups);
GBL_ASM_CALL void gbl_ppu_convert4_asm(uint32_t *out, const uint16_t *bg,
                                       const uint32_t *pairs, uint32_t groups);
GBL_ASM_CALL void gbl_ppu_convert_fine_asm(uint32_t *out, const uint32_t *t,
                                           const uint32_t *pairs, uint32_t shift);
/* The tiles_hash loop: col_xor = first column | tile index xor << 8. */
GBL_ASM_CALL uint32_t gbl_ppu_tiles_hash_asm(const uint8_t *map_row, uint32_t col_xor,
                                             uint32_t count, const uint16_t *ver);
#endif

static inline uint32_t tile_row(const uint8_t *base, uint32_t idx_xor, uint8_t idx) {
    const uint8_t *t = base + ((uint32_t)(idx ^ idx_xor) << 4);
    return (uint32_t)spread[t[0]] | ((uint32_t)spread[t[1]] << 1);
}

/* Eight pixels starting fine pixels into the 16-pixel window a:b. */
static inline uint32_t extract(uint32_t a, uint32_t b, uint32_t fine) {
    return (((a << 16) | b) >> (16 - 2 * fine)) & 0xFFFFu;
}

void gbl_ppu_palette_changed(void) {
    bg_pairs_bgp = -1;
    obj_pal_key = 0xFFFFFFFFu;
}

/* Packed output (gbl_ppu_packed): sprite shades per palette, and BGP
 * applied to four background pixels at a time. */
uint8_t gbl_ppu_packed;
static uint8_t obj_nibble[2][16]; /* two raw sprite pixels -> two shades, per OBP */
static uint8_t bgp_byte[256]; /* four raw pixels -> four BGP shades */
static int16_t bgp_byte_bgp = -1;

static __attribute__((noinline)) void refresh_obj_pal(void) {
    for (int k = 0; k < 2; k++) {
        uint32_t obp = k ? OBP1 : OBP0;
        obj_pal[k][0] = 0;
        obj_pal[k][1] = gbl_lcd_palette[(obp >> 2) & 3];
        obj_pal[k][2] = gbl_lcd_palette[(obp >> 4) & 3];
        obj_pal[k][3] = gbl_lcd_palette[obp >> 6];
        for (unsigned n = 0; n < 16; n++)
            obj_nibble[k][n] = (uint8_t)(((obp >> (2 * (n >> 2))) & 3u) << 2 | ((obp >> (2 * (n & 3))) & 3u));
    }
    obj_pal_key = (uint32_t)(OBP0 | OBP1 << 8);
}

/* ---- sprite selection -------------------------------------------------- */

/* OAM indices sorted by Y, redone only after OAM changes (the assembly
 * render_line calls it too). */
#if GBL_RENDER_LINE_ASM
void gbl_ppu_sort_oam(void);
#define sort_oam gbl_ppu_sort_oam
#else
static inline __attribute__((always_inline))
#endif
void sort_oam(void) {
    /* Insertion sort.
     * (Re-sorting the previous order was tried: on SML the OAM is nearly
     * in Y order already and reloading the 40 values cost more.)
     * y_sorted keeps the Y values alongside (one load per test). */
    for (int i = 0; i < 40; i++) {
        uint8_t y = gbl_oam[i * 4];
        int j = i;
        while (j > 0 && y_sorted[j - 1] > y) {
            oam_by_y[j] = oam_by_y[j - 1];
            y_sorted[j] = y_sorted[j - 1];
            j--;
        }
        oam_by_y[j] = (uint8_t)i;
        y_sorted[j] = y;
    }
    y_sorted[40] = 0xFF; /* sentinel: never <= ymax (at most 159 + 16) */
    gbl_oam_dirty = 0;
    P.spr_ly = 0xFF;
}

/* More than ten sprites on the line: the first ten in OAM order among the
 * candidates ys[lo..hi), then sorted like select_sprites.  Rare; the
 * assembly render_line calls it too. */
#if GBL_RENDER_LINE_ASM
int gbl_ppu_select_many(uint8_t *sel, uint32_t lo, uint32_t hi);
#define select_many gbl_ppu_select_many
#else
static
#endif
__attribute__((noinline)) int select_many(uint8_t *sel, uint32_t lo, uint32_t hi);

/* Sprites on line ly, highest priority first (lower X, then OAM index). */
static int select_sprites(uint8_t *sel, uint32_t h) {
    int n = 0;
    if (gbl_oam_dirty) sort_oam();
    /* Visible sprites have OAM Y in [ly + 17 - h, ly + 16]: one run
     * [lo, hi) of the sorted order.  Lines come in order, so both ends only
     * move forward from the previous line's; otherwise start from 0. */
    uint32_t ymin = ly + 17u - h, ymax = ly + 16u;
    const uint8_t *ys = y_sorted, *lo, *hi;
    if (ly && P.spr_ly == (uint8_t)(ly - 1u) && P.spr_h == h) { /* 0xFF = invalid */
        lo = ys + P.spr_lo;
        hi = ys + P.spr_hi;
    } else {
        lo = hi = ys;
    }
    while (lo < ys + 40 && *lo < ymin) lo++;
    if (hi < lo) hi = lo;
    while (*hi <= ymax) hi++;
    P.spr_ly = ly;
    P.spr_h = (uint8_t)h;
    P.spr_lo = (uint8_t)(lo - ys);
    P.spr_hi = (uint8_t)(hi - ys);
    if (hi - lo > 10) return select_many(sel, (uint32_t)(lo - ys), (uint32_t)(hi - ys));
    int nc = (int)(hi - lo);
    const uint8_t *cand = oam_by_y + (lo - ys);
    for (int k = 0; k < nc; k++) {
        uint8_t i = cand[k], x = gbl_oam[i * 4 + 1];
        int j = n++;
        while (j > 0 && (gbl_oam[sel[j - 1] * 4 + 1] > x ||
                         (gbl_oam[sel[j - 1] * 4 + 1] == x && sel[j - 1] > i))) {
            sel[j] = sel[j - 1];
            j--;
        }
        sel[j] = i;
    }
    return n;
}

int select_many(uint8_t *sel, uint32_t lo, uint32_t hi) {
    uint8_t cand[40];
    int nc = 0, n = 0;
    for (uint32_t k = lo; k < hi; k++) cand[nc++] = oam_by_y[k];
    /* Hardware keeps the first ten in OAM order. */
    for (int a = 1; a < nc; a++) {
        uint8_t v = cand[a];
        int b = a;
        while (b > 0 && cand[b - 1] > v) { cand[b] = cand[b - 1]; b--; }
        cand[b] = v;
    }
    nc = 10;
    for (int k = 0; k < nc; k++) {
        uint8_t i = cand[k], x = gbl_oam[i * 4 + 1];
        int j = n++;
        while (j > 0 && (gbl_oam[sel[j - 1] * 4 + 1] > x ||
                         (gbl_oam[sel[j - 1] * 4 + 1] == x && sel[j - 1] > i))) {
            sel[j] = sel[j - 1];
            j--;
        }
        sel[j] = i;
    }
    return n;
}

/* ---- line signature ------------------------------------------------------ */

/* FNV-1 style mixing.  The multiplier is read from a variable so GCC emits
 * a single-cycle MULS instead of a long shift/add expansion on Cortex-M0+
 * (the frame signature too: a register write paid 45 cycles for it). */
uint32_t gbl_sig_mul = 0x9E3779B1u;

#if GBL_LINE_SIGNATURE
/* Everything render_line reads, summarized: registers, the map row version
 * and the version of every tile the line shows, and the selected sprites.
 * Versions only move when a VRAM write changes a byte. */
uint16_t gbl_ppu_tile_ver[384] __attribute__((aligned(4))); /* global and word-aligned for the assembly render_line (8x16 pairs) */
uint16_t gbl_ppu_map_ver[64];
#define tile_ver gbl_ppu_tile_ver

static uint32_t tile_epoch; /* any tile data change */

static void vram_version_bump(uint16_t off) {
    if (off < 0x1800) {
        tile_ver[off >> 4]++;
        tile_epoch++;
    } else {
        gbl_ppu_map_ver[(off - 0x1800u) >> 5]++;
    }
}

/* Hash of the versions of `count` tiles of map row mrow from column col.
 * Consecutive lines usually show the same tiles, so the last results are
 * cached, keyed by everything the hash depends on. */
typedef struct {
    uint32_t key, epoch, hash;
    uint16_t map_ver;
} tile_cache_t;
static tile_cache_t bg_cache, win_cache;

static uint32_t tiles_hash(tile_cache_t *c, uint32_t mrow, uint32_t col, uint32_t count, uint8_t lcdc) {
    uint32_t key = (mrow << 24) | (col << 16) | (count << 8) | (uint32_t)(lcdc & 0x10) | 1u;
    if (c->key == key && c->map_ver == gbl_ppu_map_ver[mrow] && c->epoch == tile_epoch) return c->hash;
    uint32_t tbase = (lcdc & 0x10) ? 0u : 128u, txor = (lcdc & 0x10) ? 0u : 0x80u;
    const uint8_t *map = gbl_vram + 0x1800u + mrow * 32u;
#if GBL_PPU_ASM
    uint32_t s = gbl_ppu_tiles_hash_asm(map, col | txor << 8, count, tile_ver + tbase);
#else
    uint32_t s = 2166136261u, k = gbl_sig_mul;
    for (uint32_t i = 0; i < count; i++) {
        s = (s ^ tile_ver[tbase + (map[(col + i) & 31u] ^ txor)]) * k;
        s ^= s >> 15;
    }
#endif
    c->key = key;
    c->map_ver = gbl_ppu_map_ver[mrow];
    c->epoch = tile_epoch;
    c->hash = s;
    return s;
}

#if GBL_RENDER_LINE_ASM
/* For the assembly render_line: which = lcdc | 0x100 for the window. */
uint32_t gbl_ppu_tiles_hash(uint32_t mrow, uint32_t col, uint32_t count, uint32_t which) {
    return tiles_hash(which & 0x100 ? &win_cache : &bg_cache, mrow, col, count, (uint8_t)which);
}
#endif

/* Multiply-xorshift: the multiply only carries bits upward, so each step
 * folds the high half back down; otherwise a change confined to the top
 * bits (a register placed at bits 24-31, say) stays confined there and two
 * such changes collide one time in 256. */
#define SIG(v) (s = (s ^ (uint32_t)(v)) * k, s ^= s >> 15)

static uint32_t line_signature(uint8_t lcdc, int window, const uint8_t *sel, int n, uint32_t h) {
    uint32_t s = 2166136261u, k = gbl_sig_mul;
    SIG(lcdc);
    if (lcdc & 0x01) {
        uint32_t y = (uint8_t)(SCY + ly);
        uint32_t mrow = ((lcdc & 0x08) ? 32u : 0u) + (y >> 3);
        /* Only the background left of the window is visible (draw_line
         * fetches the same nbg groups); BGP also colours the window. */
        uint32_t nbg = 20;
        if (window) {
            int wx0 = (int)WX - 7;
            nbg = wx0 <= 0 ? 0u : ((uint32_t)wx0 + 7u) >> 3;
        }
        if (nbg) {
            SIG(((uint32_t)BGP << 24) | ((uint32_t)SCX << 16) | (y << 8));
            SIG(gbl_ppu_map_ver[mrow]);
            SIG(tiles_hash(&bg_cache, mrow, (uint32_t)SCX >> 3, nbg + 1u, lcdc));
        } else {
            SIG((uint32_t)BGP << 24);
        }
        if (window) {
            uint32_t wrow = ((lcdc & 0x40) ? 32u : 0u) + (win_line >> 3);
            uint32_t count = WX >= 7 ? (uint32_t)(167 - WX + 7) / 8u + 1u : 21u;
            if (count > 21) count = 21;
            SIG(((uint32_t)WX << 16) | ((uint32_t)win_line << 8));
            SIG(gbl_ppu_map_ver[wrow]);
            SIG(tiles_hash(&win_cache, wrow, 0u, count, lcdc));
        }
    }
    if (lcdc & 0x02) {
        SIG(((uint32_t)OBP0 << 8) | OBP1);
        for (int j = 0; j < n; j++) {
            const uint8_t *o = gbl_oam + sel[j] * 4;
            SIG((uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24));
            uint8_t t = h == 16 ? (uint8_t)(o[2] & 0xFE) : o[2];
            SIG(tile_ver[t] | (h == 16 ? (uint32_t)tile_ver[t | 1] << 16 : 0u));
        }
    }
    return s;
}
#undef SIG
#endif

/* ---- rendering ----------------------------------------------------------- */

#if GBL_PPU_PAL32
/* Four BGP-mapped pixels per 8-bit index, built for the frontend's primary
 * palette only (it may switch to a temporary one for a few lines). */
static uint32_t pal32[256][2];
static int16_t pal32_bgp = -1;
static uint16_t pal32_key[4];
#endif
uint8_t gbl_lcd_palette_primary = 1;

/* Draw scanline ly into out: background, window, sprites. */
#if GBL_RENDER_LINE_ASM
void gbl_ppu_draw_line(uint16_t *out, uint8_t lcdc, int window, uint32_t h, const uint8_t *sel, int n);
#define draw_line gbl_ppu_draw_line
#else
static
#endif
__attribute__((noinline)) void draw_line(uint16_t *out, uint8_t lcdc, int window, uint32_t h,
                                         const uint8_t *sel, int n) {
    /* Background groups (eight 2-bit pixels) come either from bg[] (window
     * lines, pal32, BG off) or straight from the fetched tile words t[],
     * shifted by the fine scroll as they are converted. */
    uint32_t t[21];
    uint32_t fine = 0;
    int use_bg = 1;
    if (lcdc & 0x01) {
        uint32_t y = (uint32_t)(uint8_t)(SCY + ly);
        const uint8_t *map = gbl_vram + ((lcdc & 0x08) ? 0x1C00u : 0x1800u) + (y >> 3) * 32u;
        uint32_t tx = SCX >> 3;
        fine = SCX & 7u;
        const uint8_t *tiles = gbl_vram + ((lcdc & 0x10) ? 0u : 0x800u);
        uint32_t idx_xor = (lcdc & 0x10) ? 0u : 0x80u;
        const uint8_t *trow = tiles + (y & 7u) * 2u;
        /* Background groups visible left of the window (all 20 without it):
         * a group partly covered still needs its background (gm0 draws the
         * background only up to the window, bgDrawLimitX). */
        uint32_t nbg = 20;
        if (window) {
            int wx0 = (int)WX - 7;
            nbg = wx0 <= 0 ? 0u : ((uint32_t)wx0 + 7u) >> 3;
        }
        uint32_t ntiles = nbg ? nbg + 1u : 0u; /* group g reads t[g] and t[g + 1] */
#if GBL_PPU_ASM
        if (ntiles) {
            uint32_t first = 32u - tx < ntiles ? 32u - tx : ntiles;
            gbl_ppu_fetch_asm(t, map + tx, first, trow, idx_xor);
            if (first < ntiles) gbl_ppu_fetch_asm(t + first, map, ntiles - first, trow, idx_xor);
        }
#else
        for (uint32_t i = 0; i < ntiles; i++) t[i] = tile_row(trow, idx_xor, map[(tx + i) & 31u]);
#endif
        use_bg = window;
#if GBL_PPU_PAL32
        if (gbl_lcd_palette_primary) use_bg = 1;
#endif
        if (use_bg)
            for (uint32_t g = 0; g < nbg; g++) bg[g] = (uint16_t)extract(t[g], t[g + 1], fine);

        if (window) {
            int wx = (int)WX - 7;
            const uint8_t *wmap = gbl_vram + ((lcdc & 0x40) ? 0x1C00u : 0x1800u) + (uint32_t)(win_line >> 3) * 32u;
            const uint8_t *wtrow = tiles + (win_line & 7u) * 2u;
            int g = wx <= 0 ? 0 : wx >> 3;
            int first = 8 * g - wx; /* window pixel at the start of group g */
            int k0 = first < 0 ? 0 : first >> 3;
            int count = 21 - k0 < 21 ? 21 - k0 : 21;
#if GBL_PPU_ASM
            {
                int wn = count < 32 - k0 ? count : 32 - k0; /* the asm loop needs n > 0 */
                if (wn > 0) gbl_ppu_fetch_asm(t, wmap + k0, (uint32_t)wn, wtrow, idx_xor);
            }
#else
            for (int k = 0; k < count && k0 + k < 32; k++) t[k] = tile_row(wtrow, idx_xor, wmap[k0 + k]);
#endif
            for (; g < 20; g++) {
                int s = 8 * g - wx;
                if (s < 0) {
                    uint32_t lead = (uint32_t)-s * 2u;
                    uint32_t mask = 0xFFFFu >> lead;
                    bg[g] = (uint16_t)((bg[g] & ~mask) | ((t[0] >> lead) & mask));
                } else {
                    int k = (s >> 3) - k0;
                    bg[g] = (uint16_t)extract(t[k], t[k + 1], (uint32_t)s & 7u);
                }
            }
            win_line++;
        }

        uint8_t bgp = BGP;
        if (gbl_ppu_packed) {
            /* Twenty groups of eight BGP-mapped 2-bit shades. */
            if (bgp_byte_bgp != bgp) {
                for (unsigned k = 0; k < 256; k++) {
                    uint32_t m = 0;
                    for (unsigned p = 0; p < 8; p += 2) m |= ((bgp >> (2 * ((k >> p) & 3u))) & 3u) << p;
                    bgp_byte[k] = (uint8_t)m;
                }
                bgp_byte_bgp = bgp;
            }
            if (bgp == 0xE4) {
                /* The identity palette (most games): the raw shades as they are. */
                if (use_bg) memcpy(out, bg, 40);
                else if (!fine) for (int g = 0; g < 20; g++) out[g] = (uint16_t)t[g];
                else {
                    uint32_t sh = 16u - 2u * fine;
                    for (int g = 0; g < 20; g++) out[g] = (uint16_t)((t[g] << 16 | t[g + 1]) >> sh);
                }
            } else if (use_bg) {
                for (int g = 0; g < 20; g++) out[g] = (uint16_t)(bgp_byte[bg[g] >> 8] << 8 | bgp_byte[bg[g] & 255u]);
            } else if (!fine) {
                for (int g = 0; g < 20; g++) out[g] = (uint16_t)(bgp_byte[(t[g] >> 8) & 255u] << 8 | bgp_byte[t[g] & 255u]);
            } else {
                uint32_t sh = 16u - 2u * fine;
                for (int g = 0; g < 20; g++) {
                    uint32_t v = (t[g] << 16 | t[g + 1]) >> sh;
                    out[g] = (uint16_t)(bgp_byte[(v >> 8) & 255u] << 8 | bgp_byte[v & 255u]);
                }
            }
        } else {
        if (bg_pairs_bgp != bgp) {
            uint16_t c[4] = {gbl_lcd_palette[bgp & 3], gbl_lcd_palette[(bgp >> 2) & 3],
                             gbl_lcd_palette[(bgp >> 4) & 3], gbl_lcd_palette[bgp >> 6]};
            for (int k = 0; k < 16; k++) bg_pairs[k] = (uint32_t)c[k >> 2] | ((uint32_t)c[k & 3] << 16);
            bg_pairs_bgp = bgp;
        }
        uint32_t *o = (uint32_t *)out;
#if GBL_PPU_PAL32
        if (gbl_lcd_palette_primary) {
            if (pal32_bgp != bgp || memcmp(pal32_key, gbl_lcd_palette, sizeof(pal32_key))) {
                for (int k = 0; k < 256; k++) {
                    pal32[k][0] = bg_pairs[k >> 4];
                    pal32[k][1] = bg_pairs[k & 15];
                }
                pal32_bgp = bgp;
                memcpy(pal32_key, gbl_lcd_palette, sizeof(pal32_key));
            }
            for (int g = 0; g < 20; g++) {
                uint32_t v = bg[g];
                const uint32_t *a = pal32[v >> 8], *b = pal32[v & 255u];
                o[0] = a[0];
                o[1] = a[1];
                o[2] = b[0];
                o[3] = b[1];
                o += 4;
            }
        } else
#endif
#if GBL_PPU_ASM
        if (use_bg) gbl_ppu_convert_asm(o, bg, bg_pairs, 20);
        else if (!fine) gbl_ppu_convert4_asm(o, (const uint16_t *)t, bg_pairs, 20);
        else gbl_ppu_convert_fine_asm(o, t, bg_pairs, 16u - 2u * fine);
#else
        for (int g = 0; g < 20; g++) {
            uint32_t v = use_bg ? bg[g] : extract(t[g], t[g + 1], fine);
            o[0] = bg_pairs[v >> 12];
            o[1] = bg_pairs[(v >> 8) & 15u];
            o[2] = bg_pairs[(v >> 4) & 15u];
            o[3] = bg_pairs[v & 15u];
            o += 4;
        }
#endif
        }
    } else {
        uint32_t white = gbl_ppu_packed ? 0u : (uint32_t)gbl_lcd_palette[0] * 0x10001u;
        uint32_t *o = (uint32_t *)out;
        for (int i = 0; i < (gbl_ppu_packed ? 10 : 80); i++) o[i] = white;
        memset(bg, 0, sizeof(bg));
    }

    /* Highest priority first: the first opaque sprite pixel claims x, and
     * its BG-priority bit then decides between it and the background.
     * Pixels are handled eight at a time as bit masks (bit 7 = leftmost):
     * opaque = lo | hi, clipped once at the screen edges; "taken" holds one
     * bit per screen pixel x + 8 (so x = -8 .. 167 need no test). */
    if (!n) return;
    /* Cleared word by word: an initializer becomes a memset call. */
    uint32_t taken_w[6];
    taken_w[0] = 0;
    taken_w[1] = 0;
    taken_w[2] = 0;
    __asm__("" ::: "memory"); /* keep GCC from merging the stores into memset */
    taken_w[3] = 0;
    taken_w[4] = 0;
    taken_w[5] = 0;
    uint8_t *taken = (uint8_t *)taken_w;
    if (obj_pal_key != (uint32_t)(OBP0 | OBP1 << 8)) refresh_obj_pal();
    for (int k = 0; k < n; k++) {
        const uint8_t *o = gbl_oam + sel[k] * 4;
        uint32_t attr = o[3];
        uint32_t row = (uint32_t)(ly + 16 - o[0]);
        if (attr & 0x40) row = h - 1 - row;
        uint32_t tile = o[2];
        if (h == 16) tile &= 0xFE;
        const uint8_t *td = gbl_vram + tile * 16u + row * 2u;
        uint32_t lo = td[0], hi = td[1];
        uint32_t opaque = lo | hi;
        if (!opaque) continue;
        if (attr & 0x20) {
            lo = reverse8[lo];
            hi = reverse8[hi];
            opaque = reverse8[opaque];
        }
        int x = (int)o[1] - 8;
        if (x >= 160) continue; /* off screen (and a shift of 32 or more below) */
        if (x < 0) opaque &= 0xFFu >> -x;
        else if (x > 152) opaque &= (0xFFu << (x - 152)) & 0xFFu;
        uint32_t p = (uint32_t)(x + 8), b = p >> 3, sh = p & 7u;
        uint32_t window8 = (((uint32_t)taken[b] << 8 | taken[b + 1]) >> (8u - sh)) & 0xFFu;
        uint32_t draw = opaque & ~window8;
        if (!draw) continue;
        uint32_t claim = draw << (8u - sh);
        taken[b] |= (uint8_t)(claim >> 8);
        taken[b + 1] |= (uint8_t)claim;
        if (attr & 0x80) {
            /* Behind BG colours 1-3: rare, pixel by pixel. */
            for (uint32_t i = 0, m = 0x80; i < 8; i++, m >>= 1) {
                if (!(draw & m)) continue;
                uint32_t px = (uint32_t)x + i, bgv;
                if (use_bg) {
                    bgv = (uint32_t)bg[px >> 3] >> (14 - 2 * (px & 7));
                } else {
                    uint32_t q = px + fine;
                    bgv = t[q >> 3] >> (14 - 2 * (q & 7u));
                }
                if (bgv & 3u) draw &= ~m;
            }
        }
        uint32_t bits = (uint32_t)spread[lo] | ((uint32_t)spread[hi] << 1);
        if (gbl_ppu_packed) {
            /* The eight OBP shades and a 2-bit mask of the drawn pixels, laid
             * over the two groups the sprite spans at once. */
            const uint8_t *nib = obj_nibble[(attr >> 4) & 1];
            uint32_t v = (uint32_t)nib[bits >> 12] << 12 | (uint32_t)nib[(bits >> 8) & 15u] << 8 |
                         (uint32_t)nib[(bits >> 4) & 15u] << 4 | nib[bits & 15u];
            uint32_t m = (uint32_t)spread[draw] * 3u, shift = 16u - 2u * ((uint32_t)x & 7u);
            int gl = x >> 3; /* -1 when the sprite starts left of the screen */
            v <<= shift;
            m <<= shift;
            if (gl >= 0) out[gl] = (uint16_t)((out[gl] & ~(m >> 16)) | (v & m) >> 16);
            if (gl < 19) out[gl + 1] = (uint16_t)((out[gl + 1] & ~m) | (v & m));
            continue;
        }
        const uint16_t *pal = obj_pal[(attr >> 4) & 1];
        uint16_t *d = out + x;
        if (draw & 0x80) d[0] = pal[(bits >> 14) & 3];
        if (draw & 0x40) d[1] = pal[(bits >> 12) & 3];
        if (draw & 0x20) d[2] = pal[(bits >> 10) & 3];
        if (draw & 0x10) d[3] = pal[(bits >> 8) & 3];
        if (draw & 0x08) d[4] = pal[(bits >> 6) & 3];
        if (draw & 0x04) d[5] = pal[(bits >> 4) & 3];
        if (draw & 0x02) d[6] = pal[(bits >> 2) & 3];
        if (draw & 0x01) d[7] = pal[bits & 3];
    }
}

/* Most lines are skipped (unchanged signature, dropped row, skipped frame):
 * this light function decides, and only drawn lines pay for draw_line's
 * large frame (tile words, registers). */
#if GBL_TIMING_ASM || GBL_RENDER_LINE_ASM
void gbl_ppu_render_line(void); /* gbl_timing_asm.S calls it */
#define render_line gbl_ppu_render_line
#else
static
#endif
#if !GBL_RENDER_LINE_ASM /* else gbl_ppu_asm.S */
__attribute__((noinline)) void render_line(void) {
    uint8_t lcdc = LCDC;
    int window = (lcdc & 0x21) == 0x21 && ly >= WY && WX <= 166;
    uint32_t h = (lcdc & 0x04) ? 16u : 8u;
    uint8_t sel[10];
    int n = 0;

    if (!gbl_skip_render) {
        if (lcdc & 0x02) n = select_sprites(sel, h);
#if GBL_LINE_SIGNATURE
        if (gbl_platform_line_check(ly, line_signature(lcdc, window, sel, n, h)))
#endif
        {
            uint16_t *out = gbl_platform_line_begin(ly);
            if (out) {
                draw_line(out, lcdc, window, h, sel, n); /* advances win_line */
                gbl_platform_line_end(ly);
                return;
            }
        }
    }
    if (window) win_line++;
}
#endif


void gbl_ppu_vram_changed(uint16_t off) {
#if GBL_LINE_SIGNATURE
    vram_version_bump(off);
#else
    (void)off;
#endif
    gbl_ppu_content_changed();
}

#if GBL_FRAME_SIGNATURE
uint32_t gbl_ppu_frame_sig; /* global for the assembly gbl_io_wr */
#define frame_sig gbl_ppu_frame_sig
static uint32_t content_epoch;

static void sig_mix(uint32_t v) {
    frame_sig = (frame_sig ^ v) * gbl_sig_mul;
    frame_sig ^= frame_sig >> 15;
}

#if GBL_TIMING_ASM
void gbl_ppu_frame_start(void); /* gbl_timing_asm.S calls it at line 0 */
#define sig_frame_start gbl_ppu_frame_start
#else
static
#endif
void sig_frame_start(void) {
    frame_sig = 2166136261u;
    sig_mix(0x100u | lcd_on);
    for (uint8_t n = 0x40; n < 0x4C; n++)
        if (n != 0x41 && n != 0x44 && n != 0x45 && n != 0x46) sig_mix(((uint32_t)n << 8) | gbl_io[n]);
}

void gbl_ppu_content_changed(void) { content_epoch++; }
uint32_t gbl_ppu_frame_signature(void) { return gbl_sig_combine(frame_sig, content_epoch); }
#else
void gbl_ppu_content_changed(void) {}
uint32_t gbl_ppu_frame_signature(void) { return 0; }
#endif

uint32_t gbl_sig_combine(uint32_t a, uint32_t b) {
    uint32_t s = (a ^ 0x85EBCA6Bu) * 0x9E3779B1u;
    s ^= s >> 15;
    s = (s ^ b) * 0xC2B2AE35u;
    return s ^ (s >> 13);
}

static uint32_t shown_sig, prev_sig;
static uint8_t have_shown;

void gbl_frame_invalidate(void) { have_shown = 0; }

int gbl_frame_unchanged_next(int shown) {
#if GBL_FRAME_SIGNATURE
    uint32_t sig = gbl_ppu_frame_signature();
    if (shown) { shown_sig = sig; have_shown = 1; }
    int same = have_shown && sig == shown_sig && sig == prev_sig;
    prev_sig = sig;
    return same;
#else
    (void)shown;
    return 0;
#endif
}


/* Cycle of the next PPU transition (or virtual frame while the LCD is off):
 * gbl_ppu_sync (gbl_internal.h) compares against it inline, since most
 * calls (register polls, VRAM writes) land between two transitions. */
uint32_t gbl_ppu_due;

static void update_due(void) {
    gbl_ppu_due = lcd_on ? line_start + next_stage_off : off_start + FRAME_CYCLES;
}

/* LCD off: a virtual frame clock (the assembly version jumps here too). */
#if GBL_TIMING_ASM
void gbl_ppu_advance_off(uint32_t now);
#else
static
#endif
void gbl_ppu_advance_off(uint32_t now) {
    while (now - off_start >= FRAME_CYCLES) {
        off_start += FRAME_CYCLES;
        ppu_moved = 1;
        P.evt_dirty = 1;
        gbl_frame++;
        gbl_platform_vblank();
    }
    update_due();
}


#if !GBL_TIMING_ASM
/* Mode 3 length of the current line (Pan Docs "Mode 3 length"; SameBoy and
 * Gambatte agree): 172 dots + SCX%8 + 6 when the window starts on the line +
 * per object 6 dots, plus (pixels of its leftmost pixel's tile strictly to
 * the right of that pixel - 2, floored at 0) once per tile, 11 for X=0.
 * Returns the line offset where mode 0 begins. */
static uint16_t mode3_end(void) {
    /* the SCX%8 discard shows on STAT in 2-dot steps (an odd SCX%8 ends mode
     * 3 like the even value below it), and line 0 runs 2 dots longer; both
     * measured over ~11k lines against SameBoy (which matches Gambatte) */
    uint32_t pen = (SCX & 6u) + (ly == 0 ? 2u : 0u);
    int win = (LCDC & 0x20) && WY <= ly && WX <= 166;
    int wx0 = (int)WX - 7;
    if (win) pen += 6;
    if (LCDC & 0x02) {
        uint8_t h = (LCDC & 0x04) ? 16 : 8;
        uint8_t sel[10], n = 0;
        for (uint8_t i = 0; i < 40 && n < 10; i++) {
            int y = (int)gbl_oam[i * 4] - 16;
            if ((int)ly >= y && (int)ly < y + h) sel[n++] = i;
        }
        /* leftmost first, ties by OAM index (selection order is by index) */
        for (uint8_t a = 1; a < n; a++) {
            uint8_t v = sel[a], b = a;
            while (b && gbl_oam[sel[b - 1] * 4 + 1] > gbl_oam[v * 4 + 1]) { sel[b] = sel[b - 1]; b--; }
            sel[b] = v;
        }
        int seen_bg[24], seen_win[24], nb = 0, nw = 0;
        for (uint8_t k = 0; k < n; k++) {
            int ox = gbl_oam[sel[k] * 4 + 1];
            if (ox >= 168) continue; /* off the right edge: never reached by the pixel output */
            if (ox == 0) { pen += 11; continue; }
            int sx = ox - 8; /* screen column of The Pixel */
            int inwin = win && sx >= wx0;
            int px = inwin ? sx - wx0 : sx + (SCX & 7); /* column in the fetched tile grid */
            int tile = px >> 3, right = 7 - (px & 7);
            int *seen = inwin ? seen_win : seen_bg, *ns = inwin ? &nw : &nb, dup = 0;
            for (int q = 0; q < *ns; q++) if (seen[q] == tile) dup = 1;
            if (!dup) {
                seen[(*ns)++] = tile;
                if (right - 2 > 0) pen += (uint32_t)(right - 2);
            }
            pen += 6;
        }
    }
    return (uint16_t)(MODE0_START + pen);
}
#endif

#if !GBL_TIMING_ASM /* else gbl_timing_asm.S */
void gbl_ppu_advance(uint32_t now) {
    if (!lcd_on) {
        gbl_ppu_advance_off(now);
        return;
    }
    for (;;) {
        uint32_t off = now - line_start;
        if (ly < 144) {
            if (stage == 0) {
                if (off < MODE3_START) break;
                ppu_moved = 1;
                P.evt_dirty = 1;
                stage = 1;
                mode = 3;
                P.m0_start = mode3_end() + GBL_PPU_REF; /* DMG: mode 0 starts one dot later than the STAT-read model (mooneye intr_2_*) */
                stat_update();
            }
            if (stage == 1) {
                if (off < P.m0_start) break;
                ppu_moved = 1;
                P.evt_dirty = 1;
                render_line();
                stage = 2;
                mode = 0;
                stat_update();
            }
        } else if (ly == 153 && stage == 0) {
            /* line 153: LY reads 0 after LY153_ZERO cycles; the LYC
             * comparator sees 0 two cycles later (an LYC=0 match fires here,
             * a line before line 0) */
            if (off < LY153_CMP) break;
            ppu_moved = 1;
            P.evt_dirty = 1;
            stage = 1;
            lyc_refresh();
            stat_update();
        }
        if (off < LINE_CYCLES) break;
        ppu_moved = 1;
        P.evt_dirty = 1;
        line_start += LINE_CYCLES;
        stage = 0;
        if (++ly == 154) {
            ly = 0;
            win_line = 0;
#if GBL_FRAME_SIGNATURE
            sig_frame_start();
#endif
        }
        lyc_refresh();
        if (ly == 144) {
#if GBL_PPU_REF
            /* DMG: the mode 2 source also fires when line 144 starts */
            if ((P.stat_modes & 4u) && !stat_line) IF |= 0x02;
#endif
            mode = 1;
            IF |= 0x01;
            stat_update();
            gbl_frame++;
            gbl_platform_vblank();
        } else {
            if (ly < 144) mode = 2;
            stat_update();
        }
    }
    next_stage_off = ly == 153 ? (stage ? LINE_CYCLES : LY153_CMP)
                   : ly >= 144 || stage == 2 ? LINE_CYCLES : stage == 0 ? MODE3_START : P.m0_start;
    update_due();
}
#endif

uint32_t gbl_ppu_stable_until(void) {
    if (!lcd_on) return off_start + FRAME_CYCLES;
    return line_start + next_stage_off;
}

static inline uint32_t ppu_next_event(void) {
    if (!lcd_on) return off_start + FRAME_CYCLES;
    uint8_t s = STAT;
#if GBL_TIMING_ASM
    if (ly < 144 && stage < 2 && (s & 0x08)) return line_start + MODE0_START;
#else
    if (ly < 144 && stage == 0 && (s & 0x08)) return line_start + MODE3_START; /* mode 0 offset known at mode 3 */
    if (ly < 144 && stage == 1 && (s & 0x08)) return line_start + P.m0_start;
#endif
    if (ly == 153 && !stage) return line_start + LY153_CMP;
    if (GBL_PPU_BATCH == 1 || (s & 0x20)) return line_start + LINE_CYCLES;
    /* Nothing observable happens inside a batch: the next line start that
     * matters is the batch end, VBlank, the LYC line, or the next frame. */
    uint32_t target = ly < 144 ? ((uint32_t)ly | (GBL_PPU_BATCH - 1u)) + 1u : 154u;
    if (ly < 144 && target > 144) target = 144;
    if ((s & 0x40) && LYC > ly && LYC < target) target = LYC;
    uint32_t t = line_start + (target - ly) * LINE_CYCLES;
    /* an LYC=0 match fires inside line 153 (see LY153_CMP) */
    if ((s & 0x40) && LYC == 0 && (ly < 153 || !stage)) {
        uint32_t t153 = line_start + (153u - ly) * LINE_CYCLES + LY153_CMP;
        if ((int32_t)(t153 - t) < 0) t = t153;
    }
    return t;
}

void gbl_evt_invalidate(void) { P.evt_dirty = 1; }

/* Next cycle at which the generated code or the interpreter must poll
 * devices: now when an enabled interrupt is pending, otherwise the earliest
 * device event (cached until a device changes state). */
#if !GBL_TIMING_ASM /* else gbl_timing_asm.S, ppu_next_event included */
static inline __attribute__((always_inline)) void gbl_update_evt_dirty(void) {
    if (P.evt_dirty) {
        P.dev_next = gbl_core_next_event(ppu_next_event());
        P.evt_dirty = 0;
    }
    uint32_t best = P.dev_next, now = gbr.cyc;
    /* Bound lazy device state so the u32 differences never wrap. */
    if ((int32_t)(best - now) > 70224) best = now + 70224;
    gbr.next_evt = best;
}

void gbl_update_evt(void) {
    if (gbr.ime && (gbl_ie & IF & 0x1F)) {
        gbr.next_evt = gbr.cyc;
        return;
    }
    gbl_update_evt_dirty();
}
#endif

#if GBL_ACCESS_BLOCK
/* CPU access blocking by mode (GBL_ACCESS_BLOCK): 1 = VRAM, 2 = OAM. */
int gbl_ppu_blocked(int what) {
    ppu_moved = 0;
    gbl_ppu_sync(gbr.cyc);
    if (ppu_moved) gbl_update_evt();
    if (!lcd_on) return 0;
    return what == 1 ? mode == 3 : (mode == 2 || mode == 3);
}
#endif


uint8_t gbl_ppu_read(uint8_t n) {
    if (n == 0x41 || n == 0x44) {
        ppu_moved = 0;
        gbl_ppu_sync(gbr.cyc);
        if (ppu_moved) gbl_update_evt();
        if (n == 0x44) return lcd_on ? ly_visible() : 0;
        return (uint8_t)(0x80 | (STAT & 0x78) | ((lcd_on && ly_visible() == LYC) ? 4 : 0) | (lcd_on ? mode : 0));
    }
    return gbl_io[n];
}

void gbl_ppu_write(uint8_t n, uint8_t v) {
    gbl_ppu_sync(gbr.cyc);
#if GBL_FRAME_SIGNATURE
    if (n != 0x41 && n != 0x44 && n != 0x45 && gbl_io[n] != v)
        sig_mix(((uint32_t)ly << 18) | ((uint32_t)stage << 16) | ((uint32_t)n << 8) | v);
#endif
    switch (n) {
    case 0x40:
        if ((v & 0x80) && !lcd_on) {
            lcd_on = 1;
            /* The frame restarted by an enable runs 2 cycles ahead of the
             * write (SameBoy and Gambatte: the first VBlank after enable is
             * requested 65662 cycles after the write's bus cycle). */
            line_start = gbr.cyc - 2u;
            ly = 0;
            stage = 0;
            next_stage_off = MODE3_START;
            mode = 2;
            win_line = 0;
            stat_line = 0;
            /* LY=0 at enable: a LYC=0 match with the LYC source armed
             * raises the STAT line at once (SameBoy reference: the IRQ is
             * taken right after enable, not at the first mode-3 edge).  The
             * mode sources stay quiet: the first line after enable starts
             * without a mode-2 OAM search. */
            LCDC = v;
            lyc_refresh();
            if (P.lyc_line) IF |= 0x02;
            stat_line = P.lyc_line;
        } else if (!(v & 0x80) && lcd_on) {
            lcd_on = 0;
            ly = 0;
            mode = 0;
            stat_line = 0;
            off_start = gbr.cyc;
        }
        LCDC = v;
        update_due();
        break;
    case 0x41: STAT = (uint8_t)((STAT & 0x07) | (v & 0x78)); break;
    case 0x44: return;
    default: gbl_io[n] = v; break;
    }
    if (n == 0x40 || n == 0x41 || n == 0x45) {
        /* Only LCD on/off, the STAT sources and LYC move PPU events or the
         * STAT line; scroll, palette and window writes cannot. */
        stat_refresh();
        if (lcd_on && n != 0x40) stat_update();
        P.evt_dirty = 1;
        gbl_update_evt();
    }
}

void gbl_ppu_reset(void) {
    LCDC = 0x91;
    STAT = 0x00;
    BGP = 0xFC;
    OBP0 = OBP1 = 0xFF;
    lcd_on = 1;
    line_start = gbr.cyc;
    ly = 0;
    stage = 0;
    next_stage_off = MODE3_START;
    mode = 2;
    win_line = 0;
    stat_line = 0;
    off_start = gbr.cyc;
    update_due();
    stat_refresh();
    P.spr_ly = 0xFF;
    bg_pairs_bgp = -1;
    obj_pal_key = 0xFFFFFFFFu;
#if GBL_FRAME_SIGNATURE
    content_epoch = 0;
    sig_frame_start();
#endif
}

/* Save states (gbl_state_io of gbl_core.c): the line state machine; after
 * a load, everything derived from VRAM, OAM and the registers is voided. */
void gbl_ppu_state_io(gbl_state_fn io, void *ctx, int load) {
    io(&gbl_ppu_state, sizeof(gbl_ppu_state), ctx);
    if (!load) return;
    P.evt_dirty = 1;
    P.moved = 1;
    P.spr_ly = 0xFF;
    gbl_oam_dirty = 1;
    gbl_ppu_palette_changed();
    bgp_byte_bgp = -1;
#if GBL_PPU_PAL32
    pal32_bgp = -1;
#endif
#if GBL_LINE_SIGNATURE
    for (unsigned i = 0; i < 384; i++) tile_ver[i]++;
    for (unsigned i = 0; i < 64; i++) gbl_ppu_map_ver[i]++;
    tile_epoch++;
#endif
#if GBL_FRAME_SIGNATURE
    content_epoch++;
#endif
    update_due();
}
