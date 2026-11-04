/*
 * gbl core: bus, cartridge mapper, timer, joypad, interrupts and the
 * compiled/interpreted execution loop.
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_internal.h"
#include "gbl_timing_layout.h"

#include <stddef.h>
#include <string.h>

gbl_cpu_t gbr;
uint8_t gbl_wram[GBL_WRAM_SIZE];
uint8_t gbl_vram[GBL_VRAM_SIZE];
#if GBL_CGB
uint8_t gbl_cgb, gbl_vbk, gbl_svbk = 1, gbl_key1;
uint8_t gbl_hdma_pend, gbl_hdma_active;
static uint16_t hdma_src, hdma_dst;
static uint8_t hdma_len; /* FF55 low bits: blocks left - 1 */
static void hdma_service(void);
static void hdma_write(uint8_t n, uint8_t v);
/* Banked addresses: VRAM bank 1 sits at +$2000; WRAM bank n (1..7) at
 * n * $1000, so D000 with SVBK=1 is the DMG layout. */
#define VOFF(o) ((uint16_t)((o) + (gbl_vbk ? 0x2000u : 0u)))
static inline uint16_t wram_echo(uint16_t a) {
    return a < 0xF000 ? (uint16_t)(a - 0xE000) : (uint16_t)(a - 0xF000 + gbl_svbk * 0x1000u);
}
#define WRAM_FAST_END 0xCFFFu /* the stack fast path stops before the banked half */
static void map_pages(void);
#else
#define VOFF(o) (o)
#define wram_echo(a) ((a) - 0xE000)
#define WRAM_FAST_END 0xDFFFu
#endif
uint8_t gbl_oam[0xA0] __attribute__((aligned(4))); /* word copies (OAM DMA) */
uint8_t gbl_hram[0x80];
uint8_t gbl_io[0x80];
uint8_t gbl_ie;
#if GBL_ERAM_SIZE > 0 && !GBL_ERAM_ON_SD
uint8_t gbl_eram[GBL_ERAM_SIZE];
#endif

const uint8_t *gbl_rmap[16];
uint8_t *gbl_wmap[16];

uint8_t gbl_interp_only;
uint32_t gbl_frame;
uint32_t gbl_interp_instrs;
uintptr_t gbl_stack_limit;

/* ---- cartridge ---------------------------------------------------------- */

/* Page pointers are biased by the page's CPU address so gbl_rmap[a >> 12][a]
 * addresses the byte directly. */
#define BIAS(p, cpu) ((uint8_t *)((uintptr_t)(p) - (uintptr_t)(cpu)))

static struct {
    uint8_t ram_enable;
    uint8_t lo, hi, mode;
} mbc;

#if GBL_ROM_STREAM
/* ---- ROM streaming ---------------------------------------------------------
 * Bank 0 (gbl_rom) and the resident 4 KiB chunks are in flash and mapped
 * directly; reads from other chunks go through an LRU cache of 512-byte
 * sectors filled by gbl_platform_rom_read. */
#if defined(ESP8266)
#define gbl_rom_resident ((const uint8_t (*)[4096])gbl_rom) /* the map has none */
#else
extern const uint8_t gbl_rom_resident[][4096];
#endif
extern const uint8_t gbl_rom_chunk_map[];

#if GBL_CACHE_UNIFIED
/* The cache is the cartridge RAM's (below): gbl_reset empties both. */
#define stream_reset() ((void)0)
#else
/* Buffers, tags and the last slot are global: the assembly gbl_rd reads a
 * byte of the last sector used without calling gbl_rd_slow.  With
 * GBL_CACHE_DYN the array is the fixed part (.cachepool, last in RAM) and
 * stream_ways - GBL_STREAM_FIXED more sectors follow it, in RAM the
 * frontend takes from the C stack (gbl_cache_grow, gbl_cache_shrink). */
#if GBL_CACHE_DYN && !GBL_CACHE_DYN_INLINE
uint8_t gbl_stream_buf[GBL_STREAM_FIXED][512] __attribute__((aligned(4), section(".cachepool")));
#else
uint8_t gbl_stream_buf[GBL_STREAM_MAX][512] __attribute__((aligned(4)));
#endif
int32_t gbl_stream_tag[GBL_STREAM_MAX];
uint8_t gbl_stream_last;
#define stream_tag gbl_stream_tag
#define stream_last gbl_stream_last
static uint32_t stream_age[GBL_STREAM_MAX], stream_clock;
uint32_t gbl_stream_misses;
#if GBL_CACHE_DYN
static uint8_t stream_ways = GBL_STREAM_FIXED;
/* A sector past the array: its address hides where it comes from. */
static inline uint8_t *stream_buf_of(unsigned i) {
    uint8_t *p = &gbl_stream_buf[0][0];
    __asm__("" : "+r"(p));
    return p + i * 512u;
}
#define stream_buf(i) stream_buf_of(i)
#else
#define stream_ways GBL_STREAM_SECTORS
#define stream_buf(i) gbl_stream_buf[i]
#endif

uint8_t *gbl_stream_scratch(void) { return gbl_stream_buf[0]; }

static void stream_reset(void) {
    for (unsigned i = 0; i < stream_ways; i++) { stream_tag[i] = -1; stream_age[i] = 0; }
    stream_last = 0;
}

void gbl_stream_invalidate(void) { stream_reset(); }

#if GBL_CACHE_DYN
uint32_t gbl_cache_loads;
uintptr_t gbl_cache_top(void) { return (uintptr_t)stream_buf(stream_ways); }
unsigned gbl_cache_ways(void) { return stream_ways; }

int gbl_cache_grow(void) {
    if (stream_ways >= GBL_STREAM_MAX) return 0;
    stream_tag[stream_ways] = -1;
    stream_age[stream_ways] = 0;
    stream_ways++;
    return 1;
}

int gbl_cache_shrink(void) {
    if (stream_ways <= GBL_STREAM_FIXED) return 0;
    stream_ways--;
    stream_tag[stream_ways] = -1; /* the assembly gbl_rd compares the last slot's tag */
    if (stream_last >= stream_ways) stream_last = 0;
    return 1;
}
#endif
#endif

#if GBL_ROM_PACKED
#include "m0_rom_codec.h"
#include "m0_copy.h"
/* Compressed flash source: sector k of the ROM is gbl_rom_packed
 * [off[k], off[k + 1]), LZSS-coded, or stored as is when that is 512
 * bytes.  Only sectors outside bank 0 and the resident chunks are used. */
#if defined(ESP8266) && !defined(GBL_ROM_PACKED_FLASH)
#define GBL_ROM_PACKED_FLASH 1
#endif
#if GBL_ROM_PACKED_FLASH
/* rom.bin (sectors 32 on) is not mapped: sectors are read with
 * spi_flash_read (espboy_flash.cpp). */
int espboy_rom_read(uint32_t offset, uint8_t *dst, uint32_t bytes);
#else
extern const uint8_t gbl_rom_packed[];
#endif
extern const uint32_t gbl_rom_packed_off[];

/* Decode one 512-byte sector.  Bitstream MSB first; per token a flag bit:
 * 1 = literal byte, 0 = back-reference (8-bit offset - 1, 4-bit length -
 * 2) into the bytes already decoded, so no window beyond dst is needed.
 * gbl_unpack512_asm (gbl_core_asm.S) is the same loop in Thumb-1. */
#if GBL_UNPACK_ASM
#if GBL_PPU_RAMFUNC
__attribute__((long_call)) /* in SRAM, out of BL range from flash */
#endif
void gbl_unpack512_asm(const uint8_t *src, uint8_t *dst);
#define unpack512 gbl_unpack512_asm
#else
static void unpack512(const uint8_t *src, uint8_t *dst) {
    m0_rom_unpack512_c(src, dst);
}
#endif

static int rom_unpack(uint32_t sector, uint8_t *dst) {
    uint32_t a = gbl_rom_packed_off[sector], n = gbl_rom_packed_off[sector + 1] - a;
    if (!n) return 0; /* a hybrid build leaves this sector on the card */
#if GBL_ROM_PACKED_FLASH
    /* The decoders may read two bytes ahead, so a few more than n are fetched. */
    static uint8_t packed[512 + 4] __attribute__((aligned(4)));
    if (!espboy_rom_read(a, packed, n + 2u)) return 0;
    if (n == 512) memcpy(dst, packed, 512);
    else unpack512(packed, dst);
#else
    if (n == 512) M0_COPY(512, dst, gbl_rom_packed + a); /* m0_copy.h: any alignment */
    else unpack512(gbl_rom_packed + a, dst);
#endif
    gbl_stream_unpacked++;
    return 1;
}
uint32_t gbl_stream_unpacked;
#endif

#if GBL_CACHE_UNIFIED
/* ROM sector sec into dst: unpacked from flash, else read from the card. */
static int rom_sector(uint32_t sec, uint8_t *dst) {
#if GBL_ROM_PACKED
    if (rom_unpack(sec, dst)) return 1;
#endif
#if GBL_ROM_SD
    return gbl_platform_rom_read(sec, dst);
#else
    (void)dst;
    return 0;
#endif
}
#else
static uint8_t stream_read(uint32_t off) {
    int32_t sec = (int32_t)(off >> 9);
    if (stream_tag[stream_last] == sec) return stream_buf(stream_last)[off & 511u];
    unsigned slot = 0;
    for (unsigned i = 0; i < stream_ways; i++) {
        if (stream_tag[i] == sec) { slot = i; goto hit; }
        if (stream_age[i] < stream_age[slot]) slot = i;
    }
#if GBL_ROM_PACKED
    if (!rom_unpack((uint32_t)sec, stream_buf(slot)))
#endif
#if GBL_ROM_SD
    if (!gbl_platform_rom_read((uint32_t)sec, stream_buf(slot)))
        for (int i = 0; i < 512; i++) stream_buf(slot)[i] = 0xFF;
#elif GBL_ROM_PACKED
        for (int i = 0; i < 512; i++) stream_buf(slot)[i] = 0xFF;
#endif
    stream_tag[slot] = sec;
    gbl_stream_misses++;
#if GBL_CACHE_DYN
    gbl_cache_loads++;
#endif
hit:
    stream_age[slot] = ++stream_clock;
    stream_last = (uint8_t)slot;
    return stream_buf(slot)[off & 511u];
}
#endif
#endif

/* Bank whose pages gbl_rmap[4..7] hold; the pages are only rebuilt when
 * the bank changes (games rewrite the bank register around every banked
 * call).  gbl_reset voids it. */
static uint16_t mapped_bank = 0xFFFF;

/* Bank shown by the lower window ($0000-$3FFF).  Fixed on bank 0 except on
 * a large MBC1 (1 MiB and up) in banking mode 1, where the upper register
 * selects bank 0x00/0x20/0x40/0x60.  Compiled only for that case, so small
 * DMG targets keep the fixed window and pay nothing. */
uint16_t gbl_rom0_bank;
/* A host tool that runs any cartridge on one build (deepeel, memscan) defines
 * GBL_ROM_BANKS_VAR: the bank count is then this variable, set per ROM. */
#ifdef GBL_ROM_BANKS_VAR
uint32_t gbl_rom_banks = GBL_ROM_BANKS;
/* MBC1M multicart wiring (BANK2 shifted by 4, BANK1 on 4 bits): set by the tool. */
uint8_t gbl_mbc1m;
#define MBC1_HI_SHIFT (gbl_mbc1m ? 4 : 5)
#define MBC1_LO_MASK (gbl_mbc1m ? 0x0F : 0x1F)
#define ROM_BANKS gbl_rom_banks
#define ROM_BANKS_BIG 1
#else
#define MBC1_HI_SHIFT 5
#define MBC1_LO_MASK 0x1F
#define ROM_BANKS GBL_ROM_BANKS
#define ROM_BANKS_BIG (GBL_ROM_BANKS > 32)
#endif
#if GBL_MBC == 1 && !GBL_ROM_STREAM && ROM_BANKS_BIG
static void map_rom0(void) {
    uint32_t b = mbc.mode ? ((uint32_t)(mbc.hi & 3) << MBC1_HI_SHIFT) : 0u;
    b &= (uint32_t)(ROM_BANKS - 1);
    gbl_rom0_bank = (uint16_t)b;
    const uint8_t *base = gbl_rom + b * 0x4000u;
    for (int i = 0; i < 4; i++) gbl_rmap[i] = BIAS(base, 0) + 0;
}
#define MAP_ROM0() map_rom0()
#else
#define MAP_ROM0() ((void)0)
#endif

static void map_rom_bank(void) {
    uint32_t bank;
    MAP_ROM0();
#if GBL_MBC == 1
    bank = (uint32_t)(mbc.lo & 0x1F);
    if (bank == 0) bank = 1;
    bank = (bank & MBC1_LO_MASK) | ((uint32_t)(mbc.hi & 3) << MBC1_HI_SHIFT);
#elif GBL_MBC == 2
    bank = mbc.lo & 0x0F;
    if (bank == 0) bank = 1;
#elif GBL_MBC == 3
    bank = mbc.lo & 0x7F;
    if (bank == 0) bank = 1;
#elif GBL_MBC == 5
    bank = (uint32_t)mbc.lo | ((uint32_t)(mbc.mode & 1) << 8); /* MBC5: mode holds bit 8 */
#else
    bank = 1;
#endif
    bank &= (uint32_t)(ROM_BANKS - 1);
    gbr.rom_bank = (uint16_t)bank;
    if (bank == mapped_bank) return;
    mapped_bank = (uint16_t)bank;
#if GBL_ROM_STREAM
    /* Bank 0 is already resident at gbl_rom. MBC5 can also select it in
     * the upper slot: keep those reads out of the SD cache as well. */
    if (bank == 0) {
        const uint8_t *base = BIAS(gbl_rom, 0x4000u);
        gbl_rmap[4] = gbl_rmap[5] = gbl_rmap[6] = gbl_rmap[7] = base;
    } else {
        const uint8_t *m = gbl_rom_chunk_map + bank * 4u;
#define CHUNK(i) gbl_rmap[4 + i] = m[i] ? BIAS(gbl_rom_resident[m[i] - 1u], 0x4000u + i * 0x1000u) : 0
        CHUNK(0); CHUNK(1); CHUNK(2); CHUNK(3);
#undef CHUNK
    }
#else
    const uint8_t *base = BIAS(gbl_rom + bank * 0x4000u, 0x4000u);
    gbl_rmap[4] = gbl_rmap[5] = gbl_rmap[6] = gbl_rmap[7] = base;
#endif
#ifdef GBL_ROM_TRACE
    /* host study of ROM reads: every banked read is observed */
    for (int i = 4; i < 8; i++) gbl_rmap[i] = 0;
#endif
}

/* ---- cartridge RAM -------------------------------------------------------
 * Small RAM lives in gbl_eram.  With a battery (GBL_BATTERY) the platform
 * keeps it in a file: it is loaded at reset, writes go through the slow path
 * to mark 512-byte sectors dirty, and gbl_save_flush writes those back (when
 * the game disables cartridge RAM, as games do after saving, and whenever
 * the frontend decides).  RAM too large for the target (GBL_ERAM_ON_SD)
 * exists only in that file, behind a write-back cache of
 * GBL_ERAM_CACHE_SECTORS sectors (gbl_profile.h). */
#if GBL_ERAM_SIZE > 0 && GBL_MBC != 2
static uint32_t eram_bank(void) {
    uint32_t bank = 0;
#if GBL_MBC == 1
    if (mbc.mode) bank = mbc.hi & 3;
#elif GBL_MBC == 3 || GBL_MBC == 5
    bank = mbc.hi & 0x0F;
#endif
    return bank;
}
#endif

#if GBL_ERAM_SIZE > 0
static uint32_t eram_offset(uint16_t a) {
#if GBL_MBC == 2
    return a & 0x1FFu;
#else
    return (eram_bank() * 0x2000u + (uint32_t)(a - 0xA000u)) % GBL_ERAM_SIZE;
#endif
}

uint32_t gbl_save_last_change; /* gbl_frame of the last change not yet saved */
static uint8_t save_pending;

#if GBL_ERAM_ON_SD
#ifndef GBL_ERAM_CACHE_SECTORS
#define GBL_ERAM_CACHE_SECTORS 2
#endif
#define CBLK GBL_CACHE_BLOCK
#define CSHIFT GBL_CACHE_SHIFT
#define ESLOTS (0x2000u / CBLK) /* table entries for A000-BFFF */
#if GBL_CACHE_UNIFIED
/* One pool for both (GBL_CACHE_UNIFIED): the card cache the consoles share
 * (m0_cache.h), blocks of the streamed ROM and of the cartridge RAM; -2
 * lent to the option menu.  Global for the assembly gbl_rd (the ROM's last
 * block). */
#define M0C_BLOCK CBLK
#define M0C_SHIFT CSHIFT
#define M0C_WAYS (GBL_CACHE_FIXED_BYTES / CBLK)
#define M0C_DYN GBL_CACHE_DYN
#define M0C_MAX_WAYS (GBL_CACHE_MAX_BYTES / CBLK)
#if GBL_CACHE_DYN_INLINE
#define M0C_DYN_INLINE 1
#elif GBL_CACHE_DYN
#define M0C_BUF_ATTR __attribute__((section(".cachepool"))) /* last in RAM, under the stack */
#endif
#define M0C_BUF gbl_cache_buf
#define M0C_TAG gbl_cache_tag
#define M0C_ROM_LAST gbl_cache_rom_last
#define M0C_MISSES gbl_stream_misses
#define M0C_LOADS gbl_cache_loads
#define M0C_SAVE 1
#define M0C_LEND 1
#include "m0_cache.h"
#define ERAM_WAYS CACHE_N
#define eram_buf cache_buf
#define eram_way(i) CACHE_BLK(i)
#define eram_tag cache_tag
#define eram_ref cache_ref
#define eram_hand cache_hand
#define eram_dirty cache_dirty
#define eram_mru cache_mru
#define eram_load cache_ram_load
#define eram_store cache_ram_store
#define eram_reset cache_reset
#define stream_read cache_rom_byte
#define ETAG CACHE_ETAG
#define IS_ETAG CACHE_IS_ETAG
#define ETAG_BLK CACHE_ETAG_BLK
#else
#define ERAM_WAYS GBL_ERAM_CACHE_SECTORS
#if ERAM_WAYS < 1 || ERAM_WAYS > 256
#error "GBL_ERAM_CACHE_SECTORS must be between 1 and 256"
#endif
static uint8_t eram_buf[ERAM_WAYS][512] __attribute__((aligned(4)));
#define eram_way(i) eram_buf[i]
static int32_t eram_tag[ERAM_WAYS];
#define ETAG(b) ((int32_t)(b))
#define IS_ETAG(t) ((t) >= 0)
#define ETAG_BLK(t) ((uint32_t)(t))
/* CLOCK replacement: the hand evicts the first way not referenced since it
 * last passed, never the newest (eram_mru).  Only referenced ways are in
 * the tables below, so clearing a way's referenced byte takes it out of
 * them: its next access goes through eram_sector, which marks it again. */
static uint8_t eram_ref[ERAM_WAYS], eram_hand;
static uint8_t eram_dirty[ERAM_WAYS], eram_mru;
#endif

/* Tables for gbl_rd / gbl_wr (C and assembly), so that most accesses skip
 * the slow path and eram_sector: for each 512-byte slot of A000-BFFF, the
 * buffer of the sector it shows with the current bank and RAM enable,
 * indexed by the Game Boy address, or 0 (slow path).  Reads: every
 * referenced way.  Writes: those already dirty, so that a write through the
 * table only has to note the change (gbl_save_last_change).  eram_window
 * rebuilds both after anything that moves a sector, the bank, the RAM
 * enable, a referenced byte or a dirty bit. */
uint8_t *gbl_eram_rmap[ESLOTS], *gbl_eram_wmap[ESLOTS];

static void eram_tables(uint8_t **rmap, uint8_t **wmap) {
    for (unsigned s = 0; s < ESLOTS; s++) rmap[s] = wmap[s] = 0;
#if GBL_ERAM_SIZE % 0x2000 == 0 && GBL_ERAM_TABLES
    if (!mbc.ram_enable) return;
    uint32_t base = (eram_bank() * 0x2000u) % GBL_ERAM_SIZE; /* as eram_offset */
    for (unsigned i = 0; i < ERAM_WAYS; i++) {
        uint32_t off = ETAG_BLK(eram_tag[i]) * CBLK - base;
        if (!IS_ETAG(eram_tag[i]) || off >= 0x2000u || !eram_ref[i]) continue;
        uint8_t *p = BIAS(eram_way(i), 0xA000u + off);
        rmap[off >> CSHIFT] = p;
        if (eram_dirty[i]) wmap[off >> CSHIFT] = p;
    }
#endif
}

static void eram_window(void) {
#if GBL_DEBUG_ERAM_MAPS
    gbl_dbg_eram.rebuilds++;
#endif
    eram_tables(gbl_eram_rmap, gbl_eram_wmap);
}

#if GBL_DEBUG_RESTART
/* Kept across the restart that follows a fault (.noinit on the META). */
#if defined(__arm__)
__attribute__((section(".noinit")))
#endif
gbl_dbg_tables_t gbl_dbg_tables;

void gbl_dbg_tables_check(uint32_t where) {
    if (gbl_dbg_tables.magic != 0x54424c31u) {
        memset(&gbl_dbg_tables, 0, sizeof gbl_dbg_tables);
        gbl_dbg_tables.magic = 0x54424c31u;
    }
    uint8_t *r[ESLOTS], *w[ESLOTS];
    eram_tables(r, w);
    gbl_dbg_tables.checks++;
    for (unsigned k = 0; k < 2 * ESLOTS; k++) {
        uint8_t **slot = k < ESLOTS ? &gbl_eram_rmap[k] : &gbl_eram_wmap[k - ESLOTS];
        uint8_t *want = k < ESLOTS ? r[k] : w[k - ESLOTS];
        if (*slot == want) continue;
        gbl_dbg_tables_rec_t *n = &gbl_dbg_tables.rec[gbl_dbg_tables.bad++ ? 1 : 0];
        n->frame = gbl_frame;
        n->where = where;
        n->slot = k;
        n->addr = (uint32_t)(uintptr_t)slot;
        n->bad = (uint32_t)(uintptr_t)*slot;
        n->want = (uint32_t)(uintptr_t)want;
        for (int j = 0; j < 17; j++) n->around[j] = ((uint32_t *)slot)[j - 8];
        *slot = want;
    }
}
#endif

static uint8_t eram_load(uint32_t off);
static void eram_store(uint32_t off, uint8_t v);

#if GBL_DEBUG_ERAM_MAPS
/* Every access through gbl_eram_rmap / gbl_eram_wmap (C or assembly) is
 * checked against the sector search, without its side effects: the table's
 * buffer must be the way eram_sector would find, referenced (and dirty for
 * a write, or the change would never reach the card), and the byte read
 * the one gbl_rd_slow would return.  The first and the last divergences
 * are kept for meta-usb.py eram; the tables are rebuilt after one and the
 * access goes through the slow path, so the game goes on and the next
 * ones are found too. */
gbl_dbg_eram_t gbl_dbg_eram;
_Static_assert(sizeof(gbl_dbg_eram_rec_t) == 96, "meta-usb.py eram reads this layout");

static int8_t dbg_way_of(uintptr_t first) { /* buffer starting at first */
    for (unsigned w = 0; w < ERAM_WAYS; w++)
        if (first == (uintptr_t)eram_buf[w]) return (int8_t)w;
    return -2;
}

static void dbg_eram_note(gbl_dbg_eram_rec_t *r, uint8_t kind, uint16_t a, uint8_t v, int got_way,
                          int want_way, uint8_t want_v) {
    uint8_t *const *map = kind ? gbl_eram_wmap : gbl_eram_rmap;
    r->frame = gbl_frame;
    r->addr = a;
    r->pc = gbr.pc;
    r->got_v = v;
    r->want_v = want_v;
    r->bank = (uint8_t)eram_bank();
    r->ram_enable = mbc.ram_enable;
    r->got_way = (int8_t)got_way;
    r->want_way = (int8_t)want_way;
    r->mru = eram_mru;
    r->ways = (uint8_t)ERAM_WAYS;
    r->got_ptr = (uint32_t)(uintptr_t)(map[(a - 0xA000u) >> 9] + a);
    r->want_ptr = want_way >= 0 ? (uint32_t)(uintptr_t)&eram_buf[want_way][eram_offset(a) & 511u] : 0;
    for (unsigned i = 0; i < 16; i++) {
        r->tag[i] = i < ERAM_WAYS ? (int16_t)eram_tag[i] : -2;
        uintptr_t first = 0xA000u + i * 512u; /* the slot's first byte */
        r->rmap_way[i] = gbl_eram_rmap[i] ? dbg_way_of((uintptr_t)gbl_eram_rmap[i] + first) : -1;
        r->wmap_way[i] = gbl_eram_wmap[i] ? dbg_way_of((uintptr_t)gbl_eram_wmap[i] + first) : -1;
    }
    r->kind = kind;
    r->flags = 0;
    for (unsigned w = 0; w < ERAM_WAYS && w < 16; w++)
        r->flags |= (uint32_t)(eram_ref[w] | eram_dirty[w] << 1) << (2 * w);
}

/* 0 when the table entry for a is the one the search gives. */
static int dbg_eram_check(uint8_t kind, uint16_t a, uint8_t v) {
    const uint8_t *got = (kind ? gbl_eram_wmap : gbl_eram_rmap)[(a - 0xA000u) >> 9] + a;
    uint32_t off = eram_offset(a);
    int got_way = -1, want_way = -1;
    for (unsigned w = 0; w < ERAM_WAYS; w++) {
        if (eram_tag[w] == (int32_t)(off >> 9)) want_way = (int)w;
        if (got >= eram_buf[w] && got < eram_buf[w] + 512) got_way = (int)w;
    }
    const uint8_t *want = mbc.ram_enable && want_way >= 0 && eram_ref[want_way] &&
                                  (!kind || eram_dirty[want_way])
                              ? &eram_buf[want_way][off & 511u]
                              : 0;
    uint8_t want_v = want ? *want : 0xFF;
    if (got == want && (kind || v == want_v)) return 0;
    if (!gbl_dbg_eram.errors++) dbg_eram_note(&gbl_dbg_eram.first, kind, a, v, got_way, want_way, want_v);
    dbg_eram_note(&gbl_dbg_eram.last, kind, a, v, got_way, want_way, want_v);
    eram_window();
    return 1;
}

uint8_t gbl_dbg_eram_rd(uint16_t a, uint8_t v) {
    gbl_dbg_eram.reads++;
    if (!dbg_eram_check(0, a, v)) return v;
    return mbc.ram_enable ? eram_load(eram_offset(a)) : 0xFF;
}

void gbl_dbg_eram_wr(uint16_t a, uint8_t v) {
    gbl_dbg_eram.writes++;
    if (dbg_eram_check(1, a, v)) {
        if (mbc.ram_enable) eram_store(eram_offset(a), v);
        return;
    }
    uint8_t *p = gbl_eram_wmap[(a - 0xA000u) >> 9];
    if (p[a] != v) {
        p[a] = v;
        gbl_save_last_change = gbl_frame;
    }
}
#endif

#if GBL_DEBUG_ERAM_SUM
/* SD round trip of the cache: the checksum of each sector as last written
 * to the card, checked when it is read back; an evicted sector is also read
 * back at once and compared (write or read at fault).  Read with
 * meta-usb.py read gbl_dbg_eram_sum. */
gbl_dbg_eram_sum_t gbl_dbg_eram_sum;
static uint16_t eram_sum_of[GBL_ERAM_SIZE / 512u];
static uint8_t eram_sum_known[GBL_ERAM_SIZE / 512u];

static uint16_t eram_sum(const uint8_t *p) { /* Fletcher-16 */
    unsigned a = 0, b = 0;
    for (unsigned k = 0; k < 512; k++) {
        a = (a + p[k]) % 255u;
        b = (b + a) % 255u;
    }
    return (uint16_t)(b << 8 | a);
}

static void eram_sum_note(uint32_t kind, int32_t sec, uint16_t want, uint16_t got) {
    gbl_dbg_eram_sum.errors++;
    gbl_dbg_eram_sum.kind = kind;
    gbl_dbg_eram_sum.sec = (uint32_t)sec;
    gbl_dbg_eram_sum.want = want;
    gbl_dbg_eram_sum.got = got;
    gbl_dbg_eram_sum.frame = gbl_frame;
}
#endif

#if GBL_DEBUG_STACK
static int dbg_stack_sector(int32_t tag) { return tag == ETAG(eram_offset(GBL_DBG_STACK_BASE) >> CSHIFT); }
#endif

#if GBL_CACHE_UNIFIED
/* ---- the pool shared with the streamed ROM (GBL_CACHE_UNIFIED) --------------
 * m0_cache.inc; cartridge RAM blocks only appear in the tables while
 * referenced, so the tables are rebuilt when the hand clears one. */
static int rom_sector(uint32_t sec, uint8_t *dst);
#define M0C_ROM_SECTOR(sec, dst) rom_sector((sec), (dst))
#define M0C_SAVE_READ(sec, dst) gbl_platform_save_read((sec), (dst))
#define M0C_SAVE_WRITE(sec, src) gbl_platform_save_write((sec), (src))
#define M0C_PENDING save_pending
#define M0C_NOTE_CHANGE() (gbl_save_last_change = gbl_frame)
#define M0C_SHOWN() eram_window()
#if GBL_DEBUG_STACK
#define M0C_HOOK_WRITEBACK(i) do { \
        if (dbg_stack_sector(eram_tag[i]) && !gbl_dbg_stack.frozen) { \
            gbl_dbg_stack.writebacks++; \
            gbl_dbg_stack.writeback_frame = gbl_frame; \
        } \
    } while (0)
#define M0C_HOOK_LOAD(tag) do { \
        if (dbg_stack_sector(tag) && !gbl_dbg_stack.frozen) { \
            gbl_dbg_stack.loads++; \
            gbl_dbg_stack.load_frame = gbl_frame; \
        } \
    } while (0)
#endif
/* The option menu's stack: the pool's fixed part but 3 blocks.  A state
 * save uses gbl_save_scratch at its bottom. */
#define M0C_LEND_KEEP 3u
#define M0C_FLUSH gbl_save_flush
#define M0C_FLUSH_NEXT gbl_save_flush_next
#include "m0_cache.inc"

/* 512 bytes for the platform before gbl_reset, and for gbl_state_io: the
 * pool's start (below the menu's stack when the menu saves a state). */
uint8_t *gbl_save_scratch(void) { return &eram_buf[0][0]; }
uint8_t *gbl_stream_scratch(void) { return &eram_buf[0][0]; }

uintptr_t gbl_cache_lend(void) { return cache_lend(); }
void gbl_stream_invalidate(void) { cache_unlend(); }
#if GBL_CACHE_DYN
uintptr_t gbl_cache_top(void) { return cache_top(); }
unsigned gbl_cache_ways(void) { return cache_ways; }
int gbl_cache_grow(void) { return cache_grow(); }
int gbl_cache_shrink(void) { return cache_shrink(); }
#endif
#else
static void eram_writeback(unsigned i) {
    if (eram_dirty[i]) {
#if GBL_DEBUG_STACK
        if (dbg_stack_sector(eram_tag[i]) && !gbl_dbg_stack.frozen) {
            gbl_dbg_stack.writebacks++;
            gbl_dbg_stack.writeback_frame = gbl_frame;
        }
#endif
        /* Still dirty when the card refused it: a later flush tries again. */
        if (!gbl_platform_save_write((uint32_t)eram_tag[i], eram_buf[i])) return;
        eram_dirty[i] = 0;
#if GBL_DEBUG_ERAM_SUM
        eram_sum_of[eram_tag[i]] = eram_sum(eram_buf[i]);
        eram_sum_known[eram_tag[i]] = 1;
        gbl_dbg_eram_sum.writes++;
#endif
    }
}

/* Through the slow path: a sector missing from the tables (not cached, or
 * not referenced since the hand passed), or a first write to a clean one.
 * The sector becomes the newest and referenced.  A game using its
 * cartridge RAM as work RAM cycles through several sectors (Super Mario
 * Land 2: about seven), and every eviction of a dirty one is a card
 * write. */
static uint8_t *eram_sector(uint32_t off) {
    int32_t sec = (int32_t)(off >> 9);
    unsigned i = eram_mru;
    if (eram_tag[i] != sec) {
        for (i = 0; i < ERAM_WAYS && eram_tag[i] != sec; i++) {}
        if (i == ERAM_WAYS) {
            for (;;) {
                i = eram_hand;
                eram_hand = (uint8_t)(i + 1u == ERAM_WAYS ? 0 : i + 1u);
                if (ERAM_WAYS > 1 && i == eram_mru) continue;
                if (!eram_ref[i]) break;
                eram_ref[i] = 0;
            }
            eram_writeback(i);
#if GBL_DEBUG_ERAM_SUM
            if (eram_tag[i] >= 0 && eram_sum_known[eram_tag[i]]) { /* read the evicted one back */
                gbl_dbg_eram_sum.verifies++;
                gbl_platform_save_read((uint32_t)eram_tag[i], eram_buf[i]);
                uint16_t got = eram_sum(eram_buf[i]);
                if (got != eram_sum_of[eram_tag[i]]) eram_sum_note(1, eram_tag[i], eram_sum_of[eram_tag[i]], got);
            }
#endif
            if (!gbl_platform_save_read((uint32_t)sec, eram_buf[i]))
                for (int k = 0; k < 512; k++) eram_buf[i][k] = 0xFF;
#if GBL_DEBUG_ERAM_SUM
            gbl_dbg_eram_sum.loads++;
            if (eram_sum_known[sec]) {
                uint16_t got = eram_sum(eram_buf[i]);
                if (got != eram_sum_of[sec]) {
                    eram_sum_note(2, sec, eram_sum_of[sec], got);
                    gbl_platform_save_read((uint32_t)sec, eram_buf[i]); /* once more */
                    if (eram_sum(eram_buf[i]) == eram_sum_of[sec]) gbl_dbg_eram_sum.reread_ok++;
                }
            }
#endif
            eram_tag[i] = sec;
            eram_ref[i] = 0;
            eram_dirty[i] = 0; /* even if the evicted one could not be written */
#if GBL_DEBUG_STACK
            if (dbg_stack_sector(sec) && !gbl_dbg_stack.frozen) {
                gbl_dbg_stack.loads++;
                gbl_dbg_stack.load_frame = gbl_frame;
            }
#endif
        }
    }
    if (i != eram_mru || !eram_ref[i]) {
        eram_ref[i] = 1;
        eram_mru = (uint8_t)i;
        eram_window();
    }
    return eram_buf[i];
}

static uint8_t eram_load(uint32_t off) { return eram_sector(off)[off & 511u]; }

static void eram_store(uint32_t off, uint8_t v) {
    uint8_t *p = eram_sector(off);
    if (p[off & 511u] == v) return;
    p[off & 511u] = v;
    save_pending = 1;
    gbl_save_last_change = gbl_frame;
    if (!eram_dirty[eram_mru]) {
        eram_dirty[eram_mru] = 1;
        eram_window(); /* its write slot opens */
    }
}

void gbl_save_flush(void) {
    unsigned dirty = 0;
    for (unsigned i = 0; i < ERAM_WAYS; i++) {
        eram_writeback(i);
        dirty |= eram_dirty[i];
    }
    save_pending = (uint8_t)(dirty != 0);
    eram_window(); /* clean again: the write slots close */
}

int gbl_save_flush_next(int i) {
    for (; i >= 0 && i < ERAM_WAYS; i++)
        if (eram_dirty[i]) {
            eram_writeback(i);
            eram_window(); /* clean again: its write slot closes */
            return i + 1;
        }
    unsigned dirty = 0;
    for (unsigned w = 0; w < ERAM_WAYS; w++) dirty |= eram_dirty[w];
    save_pending = (uint8_t)(dirty != 0);
    return -1;
}

uint8_t *gbl_save_scratch(void) { return eram_buf[0]; }

static void eram_reset(void) {
    for (unsigned i = 0; i < ERAM_WAYS; i++) {
        eram_tag[i] = -1;
        eram_dirty[i] = 0;
        eram_ref[i] = 0;
    }
    eram_mru = 0;
    eram_hand = 0;
    save_pending = 0;
    eram_window();
}
#endif
#else
static uint32_t eram_dirty_mask; /* bit per 512-byte sector */

static uint8_t eram_load(uint32_t off) { return gbl_eram[off]; }

static void eram_store(uint32_t off, uint8_t v) {
    if (gbl_eram[off] == v) return;
    gbl_eram[off] = v;
#if GBL_BATTERY
    eram_dirty_mask |= 1u << (off >> 9);
    save_pending = 1;
    gbl_save_last_change = gbl_frame;
#endif
}

void gbl_save_flush(void) {
#if GBL_BATTERY && defined(ESP8266)
    /* Flash is erased by 4 KiB blocks: the platform writes the whole RAM as
     * a new copy (espboy_flash.cpp) rather than one sector at a time. */
    if (eram_dirty_mask && gbl_platform_save_image(gbl_eram, GBL_ERAM_SIZE)) eram_dirty_mask = 0;
#elif GBL_BATTERY
    for (uint32_t sec = 0; eram_dirty_mask; sec++) {
        if (eram_dirty_mask & (1u << sec)) {
            gbl_platform_save_write(sec, gbl_eram + sec * 512u);
            eram_dirty_mask &= ~(1u << sec);
        }
    }
#endif
    save_pending = 0;
}

int gbl_save_flush_next(int i) {
#if GBL_BATTERY && defined(ESP8266)
    (void)i;
    gbl_save_flush();
    return -1;
#elif GBL_BATTERY
    for (; i >= 0 && i < 32; i++)
        if (eram_dirty_mask & (1u << i)) {
            gbl_platform_save_write((uint32_t)i, gbl_eram + (uint32_t)i * 512u);
            eram_dirty_mask &= ~(1u << i);
            return i + 1;
        }
    save_pending = (uint8_t)(eram_dirty_mask != 0);
#else
    (void)i;
#endif
    return -1;
}

uint8_t *gbl_save_scratch(void) { return gbl_eram; }

static void eram_reset(void) {
    eram_dirty_mask = 0;
    save_pending = 0;
#if GBL_BATTERY
    /* The saved RAM, or 0xFF (an erased cartridge) where there is none. */
    for (uint32_t sec = 0; sec * 512u < GBL_ERAM_SIZE; sec++)
        if (!gbl_platform_save_read(sec, gbl_eram + sec * 512u))
            memset(gbl_eram + sec * 512u, 0xFF, 512);
#endif
}
#endif

int gbl_save_pending(void) { return save_pending; }

/* RAM enable register: a game that disables cartridge RAM has finished
 * writing it; that is the moment to save. */
static __attribute__((unused)) void set_ram_enable(uint8_t v) {
    uint8_t on = (v & 0x0F) == 0x0A;
    if (mbc.ram_enable && !on && save_pending) gbl_save_flush();
    mbc.ram_enable = on;
}
#else
void gbl_save_flush(void) {}
int gbl_save_flush_next(int i) { (void)i; return -1; }
int gbl_save_pending(void) { return 0; }
uint8_t *gbl_save_scratch(void) { return 0; }
uint32_t gbl_save_last_change;
static __attribute__((unused)) void set_ram_enable(uint8_t v) { mbc.ram_enable = (v & 0x0F) == 0x0A; }
#endif

static void map_eram(void) {
#if GBL_ERAM_ON_SD
    eram_window(); /* bank or RAM enable */
#endif
#if GBL_ERAM_SIZE >= 0x2000 && GBL_MBC != 2 && !GBL_ERAM_ON_SD
    /* Whole 8 KiB banks map directly (reads always; writes only without a
     * battery, whose saves need the dirty tracking of the slow path).
     * Smaller RAM mirrors through the slow path. */
    uint32_t off = (eram_bank() * 0x2000u) % GBL_ERAM_SIZE;
    uint8_t *base = mbc.ram_enable ? BIAS(gbl_eram + off, 0xA000u) : 0;
    gbl_rmap[0xA] = gbl_rmap[0xB] = base;
    gbl_wmap[0xA] = gbl_wmap[0xB] = GBL_BATTERY ? 0 : base;
#endif
}

#if GBL_DEBUG_RESTART
/* The buttons of the last 16 frames, and a copy of them at each write to
 * the RAM enable register (0000-1FFF): a game that enables its RAM once,
 * when it starts (Super Mario Land 2), writes it again only when it
 * restarts.  meta-usb.py restart reads them. */
gbl_dbg_restart_t gbl_dbg_restart;

static void dbg_restart_note(uint16_t a, uint8_t v) {
    gbl_dbg_restart_snap_t *n = &gbl_dbg_restart.snap[gbl_dbg_restart.writes++ & 1u];
    n->frame = gbl_frame;
    n->addr = a;
    n->value = v;
    n->ram_enable = mbc.ram_enable;
    n->sp = gbr.sp;
    n->bank = (uint8_t)gbr.rom_bank;
    n->combo_frames = gbl_dbg_restart.combo_frames;
    for (unsigned i = 0; i < 16; i++) {
        unsigned k = (gbl_dbg_restart.pos + i) & 15u; /* oldest first */
        n->buttons[i] = gbl_dbg_restart.buttons[k];
        n->button_frame[i] = gbl_dbg_restart.button_frame[k];
    }
}
#endif

#if GBL_DEBUG_RESTART
gbl_dbg_off_t gbl_dbg_off;

static __attribute__((noinline)) void dbg_off_note(uint16_t a, uint8_t v, uint32_t lr) {
    uint32_t *sp;
#if defined(__arm__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    sp = (uint32_t *)__builtin_frame_address(0);
#endif
    gbl_dbg_off.frame = gbl_frame | 1u;
    gbl_dbg_off.lr = lr;
    gbl_dbg_off.sp = (uint32_t)(uintptr_t)sp;
    gbl_dbg_off.addr = a;
    gbl_dbg_off.value = v;
    memcpy(gbl_dbg_off.gbr, &gbr, sizeof gbl_dbg_off.gbr);
    for (unsigned i = 0; i < 16; i++) gbl_dbg_off.gb_stack[i] = gbl_rd((uint16_t)(gbr.sp - 4u + i));
    for (unsigned i = 0; i < 32; i++)
        gbl_dbg_off.c_stack[i] = (uint32_t)((uintptr_t)(sp + i) < 0x20008000u ? sp[i] : 0);
    if (!gbl_dbg_restart.lock_frame) gbl_dbg_restart.lock_frame = gbl_frame | 1u;
}
#endif

#if GBL_DEBUG_STACK
gbl_dbg_stack_t gbl_dbg_stack = {.magic = 0x4B415453u, .len = GBL_DBG_STACK_LEN};

static void dbg_stack_freeze(uint32_t why) {
    if (gbl_dbg_stack.frozen) return;
    gbl_dbg_stack.frozen = why;
    gbl_dbg_stack.frozen_frame = gbl_frame;
}

void gbl_dbg_stack_note(uint16_t a, uint8_t v, uint32_t lr) {
    gbl_dbg_stack_t *s = &gbl_dbg_stack;
    unsigned i = (unsigned)(uint16_t)(a - GBL_DBG_STACK_BASE);
    if (i >= GBL_DBG_STACK_LEN || s->frozen) return;
    s->value[i] = v;
    s->valid[i >> 3] |= (uint8_t)(1u << (i & 7u));
    s->pc[i] = gbr.pc;
    s->sp[i] = gbr.sp;
    s->frame[i] = (uint16_t)gbl_frame;
    s->lr[i] = (uint16_t)(lr >> 2);
    s->at_write_loads[i] = (uint8_t)s->loads;
}

/* The assembly gbl_wr's tail for a dirty sector's slot (e from gbl_eram_wmap). */
void gbl_dbg_stack_wr(uint16_t a, uint8_t v, uint8_t *e) {
    if (e[a] != v) {
        e[a] = v;
        gbl_save_last_change = gbl_frame;
    }
    gbl_dbg_stack_note(a, v, (uint32_t)(uintptr_t)__builtin_return_address(0));
}

uint16_t gbl_dbg_pop(uint16_t sp, uint16_t v) {
    gbl_dbg_stack_t *s = &gbl_dbg_stack;
    s->pops++;
    if (s->frozen || !mbc.ram_enable) return v;
    for (unsigned k = 0; k < 2; k++) {
        unsigned i = (unsigned)(uint16_t)(sp + k - GBL_DBG_STACK_BASE);
        if (i >= GBL_DBG_STACK_LEN || !(s->valid[i >> 3] >> (i & 7u) & 1u)) continue;
        s->checked++;
        uint8_t got = (uint8_t)(v >> (8 * k));
        if (got == s->value[i]) continue;
        s->bad++;
        s->bad_frame = gbl_frame;
        s->bad_sp = sp;
        s->bad_addr = (uint16_t)(sp + k);
        s->bad_pc = gbr.pc;
        s->bad_bank = (uint16_t)gbr.rom_bank;
        s->bad_got = got;
        s->bad_want = s->value[i];
#if GBL_ERAM_ON_SD
        uint32_t off = eram_offset(GBL_DBG_STACK_BASE);
        unsigned slot = (GBL_DBG_STACK_BASE - 0xA000u) >> CSHIFT;
        s->rmap_way = s->wmap_way = s->tag_way = -1;
        s->flags = 0;
        for (unsigned w = 0; w < 16; w++) s->tag[w] = w < ERAM_WAYS ? (int16_t)(IS_ETAG(eram_tag[w]) ? ETAG_BLK(eram_tag[w]) : eram_tag[w] | 0x4000) : -2;
        for (unsigned w = 0; w < ERAM_WAYS; w++) {
            const uint8_t *b = eram_way(w);
            if (gbl_eram_rmap[slot] && gbl_eram_rmap[slot] + GBL_DBG_STACK_BASE == b + (off & (CBLK - 1u))) s->rmap_way = (int8_t)w;
            if (gbl_eram_wmap[slot] && gbl_eram_wmap[slot] + GBL_DBG_STACK_BASE == b + (off & (CBLK - 1u))) s->wmap_way = (int8_t)w;
            if (eram_tag[w] == ETAG(off >> CSHIFT)) {
                s->tag_way = (int8_t)w;
                memcpy(s->actual, b + (off & (CBLK - 1u)), GBL_DBG_STACK_LEN);
            }
            if (w < 16) s->flags |= (uint32_t)(eram_ref[w] | eram_dirty[w] << 1) << (2 * w);
        }
        s->mru = (int8_t)eram_mru;
        s->hand = (int8_t)eram_hand;
#endif
        dbg_stack_freeze(2);
        break;
    }
    return v;
}
#endif

/* The assembly gbl_wr calls it directly for 0000-7FFF. */
#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
void gbl_mbc_write(uint16_t a, uint8_t v);
#define mbc_write gbl_mbc_write
#else
static
#endif
void mbc_write(uint16_t a, uint8_t v) {
#if GBL_DEBUG_STACK
    if (a < 0x2000 && mbc.ram_enable && (v & 0x0F) != 0x0A) dbg_stack_freeze(1);
#endif
#if GBL_DEBUG_RESTART
    if (a < 0x2000 && mbc.ram_enable && (v & 0x0F) != 0x0A && !gbl_dbg_off.frame)
        dbg_off_note(a, v, (uint32_t)(uintptr_t)__builtin_return_address(0));
    if (a < 0x2000) dbg_restart_note(a, v);
    if (a >= 0x2000 && a < 0x4000 && !gbl_dbg_restart.lock_frame) {
        unsigned k = gbl_dbg_restart.bank_pos++ & 15u;
        gbl_dbg_restart.bank_val[k] = v;
        gbl_dbg_restart.bank_frame[k] = (uint16_t)gbl_frame;
#if GBL_ERAM_ON_SD
        const uint8_t *sh = gbl_eram_rmap[(0xA24Eu - 0xA000u) >> CSHIFT];
        gbl_dbg_restart.bank_shadow[k] = sh ? sh[0xA24Eu] : 0xEE;
#endif
    }
#endif
#if GBL_MBC == 1
    switch (a >> 13) {
    case 0: set_ram_enable(v); map_eram(); break;
    case 1: mbc.lo = v; map_rom_bank(); break;
    case 2: mbc.hi = v & 3; map_rom_bank(); map_eram(); break;
    case 3: mbc.mode = v & 1; MAP_ROM0(); map_eram(); break;
    }
#elif GBL_MBC == 2
    if (a < 0x4000) {
        if (a & 0x100) { mbc.lo = v; map_rom_bank(); }
        else set_ram_enable(v);
    }
#elif GBL_MBC == 3
    switch (a >> 13) {
    case 0: set_ram_enable(v); map_eram(); break;
    case 1: mbc.lo = v; map_rom_bank(); break;
    case 2: mbc.hi = v; map_eram(); break;
    default: break; /* RTC latch: no RTC on this target */
    }
#elif GBL_MBC == 5
    if (a < 0x2000) { set_ram_enable(v); map_eram(); }
    else if (a < 0x3000) { mbc.lo = v; map_rom_bank(); }
    /* ROM bank bit 8 in mbc.mode, RAM bank in mbc.hi: they shared hi's
     * bit 0, so RAM bank 1 then 0 stayed on 1 and moved the ROM bank. */
    else if (a < 0x4000) { mbc.mode = v & 1; map_rom_bank(); }
    else if (a < 0x6000) { mbc.hi = v & 0x0F; map_eram(); }
#else
    (void)a; (void)v;
#endif
}

#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
/* gbl_core_asm.S: copy 160 bytes into the (word-aligned) OAM, return
 * nonzero when any byte changed. */
uint32_t gbl_oam_dma_asm(uint8_t *oam, const uint8_t *src);
#endif

/* ---- timer ---------------------------------------------------------------- */

/* Timer and serial state in one structure, for the assembly gbl_sync
 * (gbl_timing_asm.S, offsets in gbl_timing_layout.h). */
typedef struct {
    uint32_t div_base;    /* cycle at which the 16-bit divider was zero */
    uint32_t tima_sync;   /* cycle the TIMA counter was last advanced to */
    uint32_t serial_done; /* end of the transfer in progress */
    uint8_t serial_busy;
    uint8_t core_has_next, core_dirty; /* gbl_core_next_event cache */
    uint32_t core_next;
    /* C timing path: a TIMA overflow requests its interrupt one M-cycle
     * later (SameBoy reference); the request waits here until then */
    uint32_t tima_irq_at;
    uint8_t tima_irq_pending;
} gbl_timer_state_t;
gbl_timer_state_t gbl_timer_state = {.core_dirty = 1};
_Static_assert(offsetof(gbl_timer_state_t, div_base) == GBL_T_DIV_BASE, "div_base");
_Static_assert(offsetof(gbl_timer_state_t, tima_sync) == GBL_T_TIMA_SYNC, "tima_sync");
_Static_assert(offsetof(gbl_timer_state_t, serial_busy) == GBL_T_SERIAL_BUSY, "serial_busy");
_Static_assert(offsetof(gbl_timer_state_t, core_has_next) == GBL_T_CORE_HAS_NEXT, "core_has_next");
_Static_assert(offsetof(gbl_timer_state_t, core_dirty) == GBL_T_CORE_DIRTY, "core_dirty");
_Static_assert(offsetof(gbl_timer_state_t, core_next) == GBL_T_CORE_NEXT, "core_next");
#define div_base gbl_timer_state.div_base
#define tima_sync gbl_timer_state.tima_sync
#define serial_done gbl_timer_state.serial_done
#define serial_busy gbl_timer_state.serial_busy
const uint8_t gbl_tac_shift[4] = {10, 4, 6, 8};
#define tac_shift gbl_tac_shift

#define TIMA gbl_io[0x05]
#define TMA gbl_io[0x06]
#define TAC gbl_io[0x07]
#define IF gbl_io[0x0F]

static void core_invalidate(void);

#if GBL_TIMER_EDGE
static uint32_t tima_reload_at; /* cycle of the last TIMA reload from TMA */
#endif
/* Also the slow path of the assembly gbl_sync (a TIMA overflow). */
#if GBL_TIMING_ASM
void gbl_timer_sync(uint32_t now);
#define timer_sync gbl_timer_sync
#else
static
#endif
void timer_sync(uint32_t now) {
#if !GBL_TIMING_ASM
    if (gbl_timer_state.tima_irq_pending && (int32_t)(now - gbl_timer_state.tima_irq_at) >= 0) {
        gbl_timer_state.tima_irq_pending = 0;
        IF |= 0x04;
#if GBL_TIMER_EDGE
        TIMA = TMA;
#endif
        core_invalidate();
    }
#endif
    if (TAC & 4) {
        uint32_t s = tac_shift[TAC & 3];
        uint32_t done = (tima_sync - div_base) >> s;
        uint32_t ticks = ((now - div_base) >> s) - done;
        while (ticks) {
            uint32_t room = 256u - TIMA;
            if (ticks < room) { TIMA = (uint8_t)(TIMA + ticks); break; }
            ticks -= room;
            done += room;
            TIMA = TMA;
            core_invalidate(); /* without a reload the next overflow stays put */
#if !GBL_TIMING_ASM
            uint32_t at = div_base + (done << s) + 4u;
#if GBL_TIMER_EDGE
            tima_reload_at = at;
            if ((int32_t)(now - at) < 0) TIMA = 0; /* reads 0 until the reload M-cycle */
#endif
            if ((int32_t)(now - at) >= 0) IF |= 0x04;
            else {
                gbl_timer_state.tima_irq_at = at;
                gbl_timer_state.tima_irq_pending = 1;
            }
#else
            IF |= 0x04;
#endif
        }
    }
    tima_sync = now;
}

#if GBL_TIMER_EDGE
/* A clock edge forced by a DIV or TAC write: TIMA advances by one. */
static void tima_edge_tick(void) {
    if (TIMA == 0xFF) { TIMA = TMA; IF |= 0x04; }
    else TIMA++;
}
#endif

static uint32_t timer_next_event(void) {
    uint32_t s = tac_shift[TAC & 3];
    uint32_t done = (tima_sync - div_base) >> s;
    uint32_t t = div_base + ((done + (256u - TIMA)) << s);
#if !GBL_TIMING_ASM
    if (gbl_timer_state.tima_irq_pending && (int32_t)(gbl_timer_state.tima_irq_at - t) < 0)
        t = gbl_timer_state.tima_irq_at;
#endif
    return t;
}

/* ---- serial: internal-clock transfers complete with no partner ---------- */
/* (state in gbl_timer_state) */

/* ---- events --------------------------------------------------------------- */

/* Earliest timer or serial event, or best if that comes first (the PPU part
 * and the event cache live in gbl_ppu.c: gbl_update_evt). */
/* The timer/serial part is cached on its own: PPU transitions invalidate
 * the combined event far more often than the timer changes. */
/* (cache in gbl_timer_state) */
#define core_next gbl_timer_state.core_next
#define core_has_next gbl_timer_state.core_has_next
#define core_dirty gbl_timer_state.core_dirty

static void core_invalidate(void) {
    core_dirty = 1;
    gbl_evt_invalidate();
}

uint32_t gbl_core_next_event(uint32_t best) {
    if (core_dirty) {
        uint32_t t = 0;
        uint8_t has = 0;
        if (TAC & 4) { t = timer_next_event(); has = 1; }
#if !GBL_TIMING_ASM
        else if (gbl_timer_state.tima_irq_pending) { t = gbl_timer_state.tima_irq_at; has = 1; }
#endif
        if (serial_busy && (!has || (int32_t)(serial_done - t) < 0)) { t = serial_done; has = 1; }
        core_next = t;
        core_has_next = has;
        core_dirty = 0;
    }
    if (core_has_next && (int32_t)(core_next - best) < 0) best = core_next;
    return best;
}

/* The serial part of gbl_sync, then the next event (the assembly gbl_sync
 * jumps here while a transfer is in progress). */
#if GBL_TIMING_ASM
void gbl_sync_serial(uint32_t now);
#else
static inline __attribute__((always_inline))
#endif
void gbl_sync_serial(uint32_t now) {
    if (serial_busy && (int32_t)(now - serial_done) >= 0) {
        serial_busy = 0;
        core_invalidate();
        gbl_io[0x01] = 0xFF;
        gbl_io[0x02] &= 0x7F;
        IF |= 0x08;
    }
    gbl_update_evt();
}

#if !GBL_TIMING_ASM /* else gbl_timing_asm.S */
void gbl_sync(void) {
    uint32_t now = gbr.cyc;
    gbl_ppu_sync(now);
#if GBL_CGB
    if (gbl_hdma_pend) hdma_service();
#endif
    timer_sync(now);
    gbl_sync_serial(now);
}
#endif

/* ---- wait loops ------------------------------------------------------------ */

/* Iterations to skip so the first one not skipped is the first that can see
 * a change `room` cycles from now (ceil(room / period)). */
static uint32_t spin_iters(uint32_t period, int32_t room) {
    if (room <= 0) return 0;
    uint32_t r = (uint32_t)room;
    if (r > 8u * period) return (r + period - 1u) / period;
    /* Usually a few periods (until the next PPU mode change): subtracting
     * is cheaper than the M0+'s software division. */
    uint32_t k = 1;
    while (r > period) {
        r -= period;
        k++;
    }
    return k;
}

void gbl_spin(uint32_t period, uint32_t read_ofs, uint32_t kind) {
#if GBL_SPIN
    if (kind > 2) return;
    uint32_t now = gbr.cyc;
    /* Skipped iterations must not reach the loop head's interrupt poll at or
     * after next_evt, nor read a device value at or after it. */
    int32_t room = (int32_t)(gbr.next_evt - now);
    if (kind) room -= (int32_t)read_ofs;
    if (kind == 2) {
        int32_t c = (int32_t)(gbl_ppu_stable_until() - now) - (int32_t)read_ofs;
        if (c < room) room = c;
    }
    gbr.cyc = now + spin_iters(period, room) * period;
#else
    (void)period; (void)read_ofs; (void)kind;
#endif
}

uint32_t gbl_spin_n(uint32_t period, uint32_t max) {
#if GBL_SPIN
    uint32_t k = spin_iters(period, (int32_t)(gbr.next_evt - gbr.cyc));
    if (k > max) k = max;
    gbr.cyc += k * period;
    return k;
#else
    (void)period; (void)max;
    return 0;
#endif
}

uint32_t gbl_spin_kind(uint16_t a) {
    if (a < 0xFF00 || a >= 0xFF80) return 0;
    switch (a & 0xFF) {
    case 0x00: case 0x0F: return 1;
    case 0x41: case 0x44: return 2;
    case 0x40: case 0x42: case 0x43: case 0x45: case 0x47: case 0x48:
    case 0x49: case 0x4A: case 0x4B: return 0;
    default: return 3;
    }
}

/* ---- joypad ------------------------------------------------------------- */

static uint8_t buttons;
/* P1 as read, kept current by P1 writes and gbl_set_buttons: generated code
 * reads it directly (games read P1 several times in a row to let it
 * settle, Pinball Dreams some 700 times a frame). */
uint8_t gbl_joyp;

static void joypad_update(void) {
    uint8_t sel = gbl_io[0x00];
    uint8_t v = 0x0F;
    if (!(sel & 0x10)) v &= (uint8_t)~(buttons >> 4);
    if (!(sel & 0x20)) v &= (uint8_t)~(buttons & 0x0F);
    gbl_joyp = (uint8_t)(0xC0 | (sel & 0x30) | (v & 0x0F));
}

void gbl_set_buttons(uint8_t pressed) {
#if GBL_DEBUG_RESTART
    if (pressed != gbl_dbg_restart.buttons[(gbl_dbg_restart.pos - 1u) & 15u]) {
        gbl_dbg_restart.buttons[gbl_dbg_restart.pos & 15u] = pressed;
        gbl_dbg_restart.button_frame[gbl_dbg_restart.pos & 15u] = (uint16_t)gbl_frame;
        gbl_dbg_restart.pos++;
    }
    if ((pressed & 0x0F) == 0x0F) gbl_dbg_restart.combo_frames++; /* A+B+Select+Start */
#endif
    if (pressed & ~buttons) IF |= 0x10;
    buttons = pressed;
    joypad_update();
}

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons the game receives, so that a session
 * played in gm0 replays in the host harness (--input).  gbl_reset
 * references it: --gc-sections would drop it otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    gbl_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &gbl_frame, &buttons, 0 };

/* ---- IO ----------------------------------------------------------------- */

/* LY and STAT (0x44, 0x41), polled in wait loops, are read by the
 * assembly gbl_io_rd (gbl_timing_asm.S); it jumps here for the others. */
#if GBL_TIMING_ASM
uint8_t gbl_io_rd_c(uint8_t n);
#define gbl_io_rd gbl_io_rd_c
#endif
uint8_t gbl_io_rd(uint8_t n) {
    switch (n) {
    case 0x00: return gbl_joyp;
    case 0x02: return gbl_io[0x02] | 0x7E;
    case 0x04: return (uint8_t)((gbr.cyc - div_base) >> 8);
    case 0x05: timer_sync(gbr.cyc); return TIMA;
    case 0x07: return TAC | 0xF8;
    case 0x0F: gbl_sync(); return IF | 0xE0;
#if GBL_CGB
    case 0x4F: return gbl_cgb ? (uint8_t)(0xFE | gbl_vbk) : 0xFF;
    case 0x70: return gbl_cgb ? (uint8_t)(0xF8 | gbl_svbk) : 0xFF;
    case 0x68: case 0x69: case 0x6A: case 0x6B: return gbl_cgb ? gbl_cgb_pal_rd(n) : 0xFF;
    case 0x55: return gbl_cgb ? (uint8_t)(gbl_hdma_active ? hdma_len : 0x80 | hdma_len) : 0xFF;
    case 0x51: case 0x52: case 0x53: case 0x54: return 0xFF;
    case 0x4D: return gbl_cgb ? (uint8_t)(0x7E | gbl_dshift << 7 | (gbl_key1 & 1)) : 0xFF;
#endif
    default:
        if (n >= 0x10 && n < 0x40) return gbl_apu_read(n);
        if (n >= 0x40 && n < 0x4C) return gbl_ppu_read(n);
        /* unconnected registers read 1s on the DMG (mooneye unused_hwio) */
        if (n == 0x03 || (n >= 0x08 && n < 0x0F)) return 0xFF;
#if GBL_CGB
        if (n >= 0x4C && n < 0x80 && !gbl_cgb) return 0xFF;
#else
        if (n >= 0x4C && n < 0x80) return 0xFF;
#endif
        return gbl_io[n];
    }
}
#if GBL_TIMING_ASM
#undef gbl_io_rd
#endif

#ifndef GBL_OAM_DMA_BUS
#define GBL_OAM_DMA_BUS 0
#endif
#if GBL_OAM_DMA_BUS
static void dma_start(uint16_t src);
static int dma_in;
#endif
static inline __attribute__((always_inline)) void oam_dma_copy(uint16_t src) {
    uint32_t changed = 0;
    gbl_ppu_sync(gbr.cyc);
    /* XX00-XX9F never leaves its 4 KiB page: a mapped page is copied
     * straight, without a gbl_rd call per byte (about 30 cycles each
     * on the M0+, 4,800 per DMA). */
    const uint8_t *page = gbl_rmap[src >> 12];
    if (page) {
#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
        changed = gbl_oam_dma_asm(gbl_oam, page + src);
#else
        const uint8_t *p = page + src;
        for (int i = 0; i < 0xA0; i++) {
            uint8_t b = gbl_load_u8(p + i);
            changed |= (uint32_t)(b ^ gbl_oam[i]);
            gbl_oam[i] = b;
        }
#endif
    } else {
        for (int i = 0; i < 0xA0; i++) {
            uint8_t b = gbl_rd((uint16_t)(src + i));
            changed |= (uint32_t)(b ^ gbl_oam[i]);
            gbl_oam[i] = b;
        }
    }
    if (changed) {
        gbl_oam_dirty = 1;
        gbl_ppu_content_changed();
    }
}
/* Scroll, palette and window writes are handled by the assembly gbl_io_wr
 * (gbl_timing_asm.S); it jumps here for the others. */
#if GBL_TIMING_ASM
void gbl_io_wr_c(uint8_t n, uint8_t v);
#define gbl_io_wr gbl_io_wr_c
#endif
void gbl_io_wr(uint8_t n, uint8_t v) {
    switch (n) {
    case 0x00: gbl_io[0x00] = v & 0x30; joypad_update(); return;
    case 0x02:
        gbl_io[0x02] = v;
        if ((v & 0x81) == 0x81) {
            serial_busy = 1;
            core_invalidate();
            serial_done = gbr.cyc + 8u * 512u;
            gbl_update_evt();
        }
        return;
    case 0x04:
        timer_sync(gbr.cyc);
#if GBL_TIMER_EDGE
        if ((TAC & 4) && (((gbr.cyc - div_base) >> (tac_shift[TAC & 3] - 1)) & 1)) tima_edge_tick();
#endif
        div_base = gbr.cyc;
        core_invalidate();
        tima_sync = gbr.cyc;
        gbl_update_evt();
        return;
    case 0x05: timer_sync(gbr.cyc);
#if GBL_TIMER_EDGE
        if ((uint32_t)(gbr.cyc - tima_reload_at) < 4u) return; /* the reload M-cycle wins */
        gbl_timer_state.tima_irq_pending = 0; /* a write before the reload cancels it */
#endif
        TIMA = v; core_invalidate(); gbl_update_evt(); return;
    case 0x06: timer_sync(gbr.cyc); TMA = v;
#if GBL_TIMER_EDGE
        if ((uint32_t)(gbr.cyc - tima_reload_at) < 4u) { TIMA = v; core_invalidate(); gbl_update_evt(); }
#endif
        return;
    case 0x07: timer_sync(gbr.cyc);
#if GBL_TIMER_EDGE
        {
            uint32_t c = gbr.cyc - div_base;
            int o = (TAC & 4) && ((c >> (tac_shift[TAC & 3] - 1)) & 1);
            int n = (v & 4) && ((c >> (tac_shift[v & 3] - 1)) & 1);
            if (o && !n) tima_edge_tick();
        }
#endif
        TAC = v & 7; core_invalidate(); gbl_update_evt(); return;
    case 0x0F: gbl_sync(); IF = v & 0x1F; gbl_update_evt(); return;
#if GBL_CGB
    case 0x4F:
        if (gbl_cgb && gbl_vbk != (v & 1u)) {
            gbl_ppu_sync(gbr.cyc);
            gbl_vbk = v & 1u;
            map_pages();
        }
        return;
    case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
        if (gbl_cgb) hdma_write(n, v);
        return;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
        if (gbl_cgb) gbl_cgb_pal_wr(n, v);
        return;
    case 0x4D:
        if (gbl_cgb) gbl_key1 = v & 1u;
        return;
    case 0x70:
        if (gbl_cgb) {
            uint8_t b = v & 7u;
            gbl_svbk = b ? b : 1u;
            map_pages();
        }
        return;
#endif
    case 0x46: {
        /* OAM DMA completes instantly; games wait it out in HRAM anyway. */
        uint16_t src = (uint16_t)(v << 8);
#if defined(GBL_PPU_REF) && GBL_PPU_REF
#if GBL_CGB
#define GBL_CGB_ON gbl_cgb
#else
#define GBL_CGB_ON 0
#endif
        if (v >= 0xE0 && !GBL_CGB_ON) src = (uint16_t)(src - 0x2000u); /* DMG: $E0-$FF sources read the WRAM echo */
#endif
#if GBL_OAM_DMA_BUS
        dma_start(src); /* the copy itself happens when the transfer begins */
#else
        oam_dma_copy(src);
#endif
        gbl_io[0x46] = v;
        return;
    }
    default:
        if (n >= 0x10 && n < 0x40) { gbl_apu_write(n, v); return; }
        if (n >= 0x40 && n < 0x4C) { gbl_ppu_write(n, v); return; }
        gbl_io[n] = v;
        return;
    }
}
#if GBL_TIMING_ASM
#undef gbl_io_wr
#endif

#if GBL_CGB
/* One 16-byte block of a VRAM DMA (GDMA or HDMA). */
static void hdma_block(void) {
    for (unsigned i = 0; i < 16; i++) {
        uint8_t v = gbl_rd((uint16_t)(hdma_src + i));
        gbl_vram_wr((uint16_t)((hdma_dst + i) & 0x1FFFu), v);
    }
    hdma_src = (uint16_t)(hdma_src + 16u);
    hdma_dst = (uint16_t)((hdma_dst + 16u) & 0x1FF0u);
    gbr.cyc += 32u << gbl_dshift;
}

static void hdma_service(void) {
    gbl_hdma_pend = 0;
    if (!gbl_hdma_active || !gbl_ppu_in_hblank()) return;
    hdma_block();
    if (hdma_len-- == 0) { gbl_hdma_active = 0; hdma_len = 0x7F; }
    gbl_update_evt();
}

static void hdma_write(uint8_t n, uint8_t v) {
    switch (n) {
    case 0x51: hdma_src = (uint16_t)(v << 8 | (hdma_src & 0xFF)); break;
    case 0x52: hdma_src = (uint16_t)((hdma_src & 0xFF00) | (v & 0xF0)); break;
    case 0x53: hdma_dst = (uint16_t)(((v & 0x1F) << 8) | (hdma_dst & 0xFF)); break;
    case 0x54: hdma_dst = (uint16_t)((hdma_dst & 0x1F00) | (v & 0xF0)); break;
    default:
        if (gbl_hdma_active && !(v & 0x80)) { gbl_hdma_active = 0; gbl_hdma_pend = 0; gbl_update_evt(); return; }
        hdma_len = v & 0x7F;
        if (v & 0x80) {
            gbl_hdma_active = 1;
            gbl_ppu_sync(gbr.cyc);
            if (gbl_ppu_in_hblank()) gbl_hdma_pend = 1;
        } else {
            for (unsigned b = 0; b <= hdma_len; b++) hdma_block();
            hdma_len = 0x7F;
        }
        gbl_update_evt();
    }
}

/* STOP with KEY1 armed: change speed (DIV restarts; the switch itself takes
 * about 2050 M-cycles). */
void gbl_speed_switch(void) {
    gbl_key1 = 0;
    gbl_io_wr(0x04, 0);
    gbl_ppu_set_speed(gbl_dshift ^ 1u);
    gbr.cyc += 8200u;
    gbl_update_evt();
}
#endif

/* ---- bus ---------------------------------------------------------------- */

#ifdef GBL_ROM_TRACE
uint8_t gbl_rom_trace_read(uint32_t bank, uint16_t a); /* host harness */
uint8_t gbl_rom_fetch; /* set by the interpreter while fetching opcodes */
/* host harness: a cartridge RAM access (offset in the save), write only
 * when the byte changes (what the card cache sees, m0romsim) */
void gbl_eram_trace(uint32_t off, unsigned write);
#if GBL_ERAM_SIZE > 0
#define GBL_ERAM_TRACE(a, write) do { if ((a) - 0xA000u < 0x2000u && mbc.ram_enable) gbl_eram_trace(eram_offset(a), (write)); } while (0)
#endif
#endif
#ifndef GBL_ERAM_TRACE
#define GBL_ERAM_TRACE(a, write) ((void)0)
#endif


#if GBL_OAM_DMA_BUS
/* OAM DMA bus model (reference/host builds, off on the device): 160 M-cycles
 * from two M-cycles after the $FF46 write.  While it runs the CPU sees the
 * byte being transferred on the DMA's bus (external: ROM/SRAM/WRAM, or
 * VRAM), cannot write there and finds OAM closed; HRAM and I/O are free.  A
 * restart leaves the previous transfer running until the new one starts. */
static uint32_t dma_s[2], dma_e[2];
static uint16_t dma_src[2];
static uint8_t dma_n, dma_pend;
static uint16_t dma_psrc;
static uint32_t dma_pat;
static void dma_apply(int force) {
    if (dma_pend && (force || (int32_t)(gbr.cyc - dma_pat) >= 0)) {
        dma_pend = 0;
        dma_in = 1;
        oam_dma_copy(dma_psrc);
        dma_in = 0;
    }
}
static int dma_find(uint32_t now, uint32_t *idx) {
    for (int i = 1; i >= 0; i--) {
        if (i >= dma_n) continue;
        if ((int32_t)(now - dma_s[i]) >= 0 && (int32_t)(now - dma_e[i]) < 0) { *idx = (now - dma_s[i]) >> 2; return i; }
    }
    return -1;
}
static inline int dma_vram(uint16_t src) { return src >= 0x8000 && src < 0xA000; }
static int dma_conflict(uint16_t a, uint8_t *val) {
    uint32_t idx;
    int w;
    if (dma_n == 0 || a >= 0xFF00 || dma_in) return 0;
    dma_apply(0);
    if ((w = dma_find(gbr.cyc, &idx)) < 0) return 0;
    if (a >= 0xFE00) { *val = 0xFF; return 1; }
    if (dma_vram(a) != dma_vram(dma_src[w])) return 0;
    uint16_t sa = (uint16_t)(dma_src[w] + idx);
    dma_in = 1;
    *val = gbl_rd(sa >= 0xE000 ? (uint16_t)(sa & 0xDFFF) : sa);
    dma_in = 0;
    return 1;
}
static void dma_start(uint16_t src) {
    uint32_t st = gbr.cyc + 8;
    dma_apply(1);
    dma_psrc = src; dma_pat = st; dma_pend = 1;
    if (dma_n && (int32_t)(dma_e[dma_n - 1] - st) > 0) {
        dma_e[dma_n - 1] = st; /* the old one stops when the new one begins */
        if (dma_n == 2) { dma_s[0] = dma_s[1]; dma_e[0] = dma_e[1]; dma_src[0] = dma_src[1]; dma_n = 1; }
    } else dma_n = 0;
    dma_s[dma_n] = st; dma_e[dma_n] = st + 640; dma_src[dma_n] = src; dma_n++;
}
#endif
/* Unmapped pages (IO, OAM, HRAM, MBC registers, streamed ROM...); also the
 * tail of the assembly gbl_rd. */
uint8_t gbl_rd_slow(uint16_t a) {
#ifdef GBL_ROM_TRACE
    if (a >= 0x4000 && a < 0x8000) return gbl_rom_trace_read(gbr.rom_bank, a);
#endif
#if GBL_ROM_STREAM
    if (a >= 0x4000 && a < 0x8000) return stream_read(gbr.rom_bank * 0x4000u + (a - 0x4000u));
#endif
    if (a >= 0xFF80) return a == 0xFFFF ? gbl_ie : gbl_hram[a - 0xFF80];
    if (a >= 0xFF00) return gbl_io_rd((uint8_t)a);
#if GBL_ACCESS_BLOCK
    if (a >= 0xFE00) return a < 0xFEA0 && !gbl_ppu_blocked(2) ? gbl_oam[a - 0xFE00] : 0xFF;
    if (a >= 0x8000 && a < 0xA000) return gbl_ppu_blocked(1) ? 0xFF : gbl_vram[VOFF(a - 0x8000)];
#else
    if (a >= 0xFE00) return a < 0xFEA0 ? gbl_oam[a - 0xFE00] : 0xFF;
#endif
    if (a >= 0xE000) return gbl_wram[wram_echo(a)];
#if GBL_MBC == 2
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable)
        return eram_load(eram_offset(a)) | 0xF0;
#elif GBL_ERAM_SIZE > 0
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable)
        return eram_load(eram_offset(a));
#endif
    return 0xFF;
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
uint8_t gbl_rd(uint16_t a) {
    GBL_ERAM_TRACE(a, 0);
#if GBL_OAM_DMA_BUS
    { uint8_t dv; if (dma_conflict(a, &dv)) return dv; }
#endif
    const uint8_t *p = gbl_rmap[a >> 12];
    if (p) return gbl_load_u8(p + a);
#if GBL_ERAM_ON_SD
    if (a - 0xA000u < 0x2000u) {
        const uint8_t *e = gbl_eram_rmap[(a - 0xA000u) >> CSHIFT];
#if GBL_DEBUG_ERAM_MAPS
        if (e) return gbl_dbg_eram_rd(a, e[a]);
#else
        if (e) return e[a];
#endif
    }
#endif
    return gbl_rd_slow(a);
}
#endif

void gbl_vram_wr(uint16_t off, uint8_t v) {
    off = VOFF(off);
    if (gbl_vram[off] == v) return;
    /* Lines already past mode 3 must be drawn with the old contents. */
    gbl_ppu_sync(gbr.cyc);
    gbl_vram[off] = v;
    gbl_ppu_vram_changed(off);
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
void gbl_wr(uint16_t a, uint8_t v) {
    GBL_ERAM_TRACE(a, eram_load(eram_offset(a)) != v);
#if GBL_OAM_DMA_BUS
    { uint8_t dv; if (dma_conflict(a, &dv)) return; }
#endif
    uint8_t *p = gbl_wmap[a >> 12];
    if (p) { p[a] = v; return; }
#if GBL_ERAM_ON_SD
    if (a - 0xA000u < 0x2000u) {
        uint8_t *e = gbl_eram_wmap[(a - 0xA000u) >> CSHIFT];
#if GBL_DEBUG_ERAM_MAPS
        if (e) { gbl_dbg_eram_wr(a, v); return; }
#elif GBL_DEBUG_STACK
        if (e) { gbl_dbg_stack_wr(a, v, e); return; }
#else
        if (e) {
            if (e[a] != v) {
                e[a] = v;
                gbl_save_last_change = gbl_frame;
            }
            return;
        }
#endif
    }
#endif
    gbl_wr_slow(a, v);
}
#endif

/* Unmapped pages; also the tail of the assembly gbl_wr. */
void gbl_wr_slow(uint16_t a, uint8_t v) {
    if (a >= 0xFF80) {
        if (a == 0xFFFF) { gbl_ie = v; gbl_update_evt(); }
        else gbl_hram[a - 0xFF80] = v;
        return;
    }
    if (a >= 0xFF00) { gbl_io_wr((uint8_t)a, v); return; }
    if (a >= 0xFE00) {
#if GBL_ACCESS_BLOCK
        if (a < 0xFEA0 && gbl_ppu_blocked(2)) return;
#endif
        if (a < 0xFEA0 && gbl_oam[a - 0xFE00] != v) {
            gbl_ppu_sync(gbr.cyc);
            gbl_oam[a - 0xFE00] = v;
            gbl_oam_dirty = 1;
            gbl_ppu_content_changed();
        }
        return;
    }
    if (a >= 0x8000 && a < 0xA000) {
#if GBL_ACCESS_BLOCK
        if (gbl_ppu_blocked(1)) return;
#endif
        gbl_vram_wr((uint16_t)(a - 0x8000), v);
        return;
    }
    if (a >= 0xE000) {
#if GBL_DEBUG_RESTART && GBL_ERAM_ON_SD
        gbl_dbg_tables.echo_writes++;
        gbl_dbg_tables.echo_last = (uint32_t)a << 16 | (uint32_t)v << 8;
        gbl_dbg_tables.echo_frame = gbl_frame;
#endif
        gbl_wram[wram_echo(a)] = v;
        return;
    }
    if (a < 0x8000) { mbc_write(a, v); return; }
#if GBL_MBC == 2
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable) eram_store(eram_offset(a), v & 0x0F);
#elif GBL_ERAM_SIZE > 0
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable) {
#if GBL_DEBUG_STACK
        gbl_dbg_stack_note(a, v, (uint32_t)(uintptr_t)__builtin_return_address(0));
#endif
        eram_store(eram_offset(a), v);
    }
#endif
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
void gbl_push(uint16_t v) {
    gbr.sp = (uint16_t)(gbr.sp - 2);
    uint16_t s = gbr.sp;
    if (s >= 0xC000 && s < WRAM_FAST_END) {
        gbl_wram[s - 0xC000] = (uint8_t)v;
        gbl_wram[s - 0xC000 + 1] = (uint8_t)(v >> 8);
        return;
    }
    gbl_wr((uint16_t)(s + 1), (uint8_t)(v >> 8));
    gbl_wr(s, (uint8_t)v);
}

uint16_t gbl_pop(void) {
    uint16_t s = gbr.sp;
    gbr.sp = (uint16_t)(s + 2);
    if (s >= 0xC000 && s < WRAM_FAST_END)
        return (uint16_t)(gbl_wram[s - 0xC000] | (gbl_wram[s - 0xC000 + 1] << 8));
    uint8_t lo = gbl_rd(s);
#if GBL_DEBUG_STACK
    return gbl_dbg_pop(s, (uint16_t)(lo | (gbl_rd((uint16_t)(s + 1)) << 8)));
#else
    return (uint16_t)(lo | (gbl_rd((uint16_t)(s + 1)) << 8));
#endif
}
#endif

/* ---- interrupts and execution ------------------------------------------- */

uint8_t gbl_safe_request;
__attribute__((weak)) void gbl_platform_safe_point(void) {}

#if GBL_DEBUG_RESTART
#if defined(__arm__)
__attribute__((section(".noinit")))
#endif
gbl_dbg_irq_t gbl_dbg_irq;
#define IRQ_DEPTH 4
static gbl_dbg_irq_regs_t irq_stack[IRQ_DEPTH];
static unsigned irq_depth;

static void dbg_irq_regs(gbl_dbg_irq_regs_t *r, unsigned vector) {
    r->frame = gbl_frame;
    r->sp = gbr.sp;
    r->pc = gbr.pc;
    r->bc = gbr.bc;
    r->de = gbr.de;
    r->hl = gbr.hl;
    r->bank = gbr.rom_bank;
    r->a = gbr.a;
    r->f = gbl_getf();
    r->vector = (uint8_t)vector;
}

gbl_dbg_badirq_t gbl_dbg_badirq;

static __attribute__((noinline)) void dbg_badirq(uint32_t lr) {
    gbl_dbg_badirq_t *b = &gbl_dbg_badirq;
    uint32_t *sp;
#if defined(__arm__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    sp = (uint32_t *)__builtin_frame_address(0);
#endif
    b->magic = 0x51524942u;
    b->frame = gbl_frame | 1u;
    b->lr = lr;
    b->csp = (uint32_t)(uintptr_t)sp;
    b->pc = gbr.pc;
    b->sp = gbr.sp;
    b->bc = gbr.bc;
    b->de = gbr.de;
    b->hl = gbr.hl;
    b->bank = gbr.rom_bank;
    b->a = gbr.a;
    b->ime = gbr.ime;
    for (unsigned i = 0; i < 14; i++) b->gb_stack[i] = gbl_rd((uint16_t)(gbr.sp - 4u + i));
    for (unsigned i = 0; i < 40; i++)
        b->c_stack[i] = (uint32_t)((uintptr_t)(sp + i) < 0x20008000u ? sp[i] : 0);
}

static void dbg_irq_push(int bit) { /* before the PC is pushed */
    if (gbl_dbg_irq.magic != 0x49525131u) {
        memset(&gbl_dbg_irq, 0, sizeof gbl_dbg_irq);
        gbl_dbg_irq.magic = 0x49525131u;
    }
    gbl_dbg_irq.entries++;
    if (irq_depth == IRQ_DEPTH) { /* never returned: forget the oldest */
        memmove(irq_stack, irq_stack + 1, sizeof irq_stack[0] * (IRQ_DEPTH - 1));
        irq_depth--;
    }
    dbg_irq_regs(&irq_stack[irq_depth++], (unsigned)(0x40 + bit * 8));
}

void gbl_dbg_reti(void) { /* after the PC is popped */
    /* Frames left without their RETI (the game reset its stack). */
    while (irq_depth && (int16_t)(gbr.sp - irq_stack[irq_depth - 1].sp) > 0) {
        irq_depth--;
        gbl_dbg_irq.unmatched++;
    }
    if (!irq_depth || gbr.sp != irq_stack[irq_depth - 1].sp) return; /* RETI used as RET + EI */
    gbl_dbg_irq_regs_t *e = &irq_stack[--irq_depth];
    gbl_dbg_irq.checked++;
    if (gbr.pc == e->pc && gbr.bc == e->bc && gbr.de == e->de && gbr.hl == e->hl && gbr.a == e->a &&
        gbl_getf() == e->f)
        return;
    if (gbl_dbg_irq.bad++) return; /* the first one only */
    gbl_dbg_irq.at_entry = *e;
    dbg_irq_regs(&gbl_dbg_irq.at_reti, e->vector);
    for (unsigned i = 0; i < 24; i++) gbl_dbg_irq.stack[i] = gbl_rd((uint16_t)(gbr.sp - 16u + i));
}
#endif

int gbl_irq_enter(void) {
    /* Every caller is between two instructions, gbr.pc exact. */
    if (gbl_safe_request) {
        gbl_safe_request = 0;
        gbl_platform_safe_point();
    }
    uint8_t pending = gbl_ie & IF & 0x1F;
    if (!gbr.ime || !pending) return 0;
#if GBL_STACK_TIMING
    {
        /* Reference mode: two idle M-cycles, the high byte, the low byte, and
         * the vector chosen after the high byte is written, so a write to IE
         * there can cancel the dispatch (PC = $0000, IF untouched). */
        uint32_t base = gbr.cyc;
        uint16_t pc0 = gbr.pc;
        gbr.ime = 0;
        gbr.cyc = base + 8u;
        gbr.sp = (uint16_t)(gbr.sp - 1u);
        gbl_wr(gbr.sp, (uint8_t)(pc0 >> 8));
        gbl_sync();
        pending = gbl_ie & IF & 0x1F;
        gbr.cyc = base + 12u;
        gbr.sp = (uint16_t)(gbr.sp - 1u);
        gbl_wr(gbr.sp, (uint8_t)pc0);
        gbr.cyc = base + 20u;
        if (!pending) gbr.pc = 0;
        else {
            int b = 0;
            while (!(pending & (1u << b))) b++;
            IF &= (uint8_t)~(1u << b);
            gbr.pc = (uint16_t)(0x40 + b * 8);
        }
        gbl_update_evt();
        return 1;
    }
#endif
    int bit = 0;
    while (!(pending & (1u << bit))) bit++;
    IF &= (uint8_t)~(1u << bit);
    gbr.ime = 0;
    gbr.cyc += 20;
#if GBL_DEBUG_RESTART
    if (gbr.pc - 0x68u < 0x150u - 0x68u && !gbl_dbg_badirq.frame)
        dbg_badirq((uint32_t)(uintptr_t)__builtin_return_address(0));
    dbg_irq_push(bit);
#endif
    gbl_push(gbr.pc);
    gbr.pc = (uint16_t)(0x40 + bit * 8);
    gbl_update_evt();
    return 1;
}

int gbl_event(uint16_t resume_pc) {
    for (;;) {
        gbl_sync();
        gbr.pc = resume_pc;
        if (!gbl_irq_enter()) return 0;
        gbl_exec();
        if (gbr.pc != resume_pc) return 1;
    }
}

/* Wait in HALT until an interrupt is pending, then service it when IME is
 * set.  Returns nonzero when control did not come back to resume_pc. */
void gbl_halt_wait(void) {
    gbl_sync();
    uint32_t h0 = gbr.cyc;
    int slept = 0;
    while (!(gbl_ie & IF & 0x1F)) {
        slept = 1;
        /* next_evt is bounded to one frame and the PPU keeps a virtual frame
         * clock while the LCD is off, so this always makes progress. */
        gbr.cyc = gbr.next_evt;
        gbl_sync();
    }
    /* DMG HALT exit (SameBoy model, Gambatte agrees on every traced case):
     * HALT ends at h (on the M-cycle grid: every CPU time is a multiple of
     * 4 from power-on).  Interrupts are sampled at h, then at h+6, h+10, ...
     * and the CPU resumes 2 cycles after the sample that sees the request:
     * already pending at h -> h+4; request at t > h -> max(h+8, (t+5)&~3). */
    uint32_t t = gbr.cyc;
    if (h0 & 3u) {
        /* resumed off the grid after a host cut (deepeel's frame hook ran
         * inside the wait): only bring the CPU back onto the grid */
        gbr.cyc = (t & ~3u) + 4u;
    } else if (!slept) {
        gbr.cyc = h0 + 4u;
    } else {
        uint32_t w = (t + 5u) & ~3u;
        if ((int32_t)(w - (h0 + 8u)) < 0) w = h0 + 8u;
        gbr.cyc = w;
    }
    gbl_sync();
}

int gbl_halt(uint16_t resume_pc) {
    gbl_halt_wait();
    return gbl_event(resume_pc);
}

/* Dispatch: binary search of the bank's sorted entry table, behind a small
 * direct-mapped cache (interrupt returns and unwinds resume at the same few
 * addresses).  Misses, i.e. addresses left to the interpreter, are cached
 * too. */
#if GBL_LOOKUP_CACHE
/* Global for the assembly gbl_lookup (gbl_timing_asm.S), which handles
 * hits and jumps to gbl_lookup_miss otherwise. */
typedef struct {
    uint16_t pc, bank;
    gbl_fn fn;
} gbl_lookup_entry_t;
gbl_lookup_entry_t gbl_lookup_cache[GBL_LOOKUP_CACHE];
#define lookup_cache gbl_lookup_cache
#if GBL_TIMING_ASM /* 32-bit pointers */
_Static_assert(sizeof(gbl_lookup_entry_t) == 8 && offsetof(gbl_lookup_entry_t, bank) == 2 &&
               offsetof(gbl_lookup_entry_t, fn) == 4, "gbl_lookup_entry_t layout");
#endif
_Static_assert(offsetof(gbl_cpu_t, rom_bank) == GBL_GBR_ROM_BANK, "rom_bank");
#endif

/* The dispatch table of pc's bank, or NULL when nothing is compiled in the
 * page of pc (RAM, a bank without code, or an empty 128-byte page). */
static const gbl_bank_table *lookup_page(uint16_t pc) {
    const gbl_bank_table *t;
    if (pc < 0x4000) t = &gbl_tables[0];
    else if (pc < 0x8000 && gbr.rom_bank < gbl_table_count) t = &gbl_tables[gbr.rom_bank];
    else return 0;
    if (!t->count) return 0;
    uint32_t page = (pc & 0x3FFFu) >> 7;
    return GBL_TBL16(&t->idx[17 + (page >> 4)]) >> (page & 15) & 1 ? t : 0;
}

/* Binary search of the entries in pc's 1 KiB block. */
static gbl_fn lookup_table(const gbl_bank_table *t, uint16_t pc) {
    uint32_t block = (pc & 0x3FFFu) >> 10;
    uint32_t lo = GBL_TBL16(&t->idx[block]), hi = GBL_TBL16(&t->idx[block + 1]);
    const uint16_t *addr = t->addr;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint16_t v = GBL_TBL16(&addr[mid]);
        if (v == pc) return t->fn[mid];
        if (v < pc) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* RAM code: a recorded variant is entered only while the RAM still holds
 * its bytes (see gbl_ram_variant).  Never cached: the bytes can change
 * between two lookups.  Empty weak defaults serve projects without variants
 * (the generated code defines the strong ones). */
__attribute__((weak)) const uint16_t gbl_ram_variant_count = 0;
__attribute__((weak)) const gbl_ram_variant gbl_ram_variants[1] = {{0, 0, 0, {0, 0, 0, 0}}};

static gbl_fn ram_lookup(uint16_t pc) {
    for (uint16_t i = 0; i < gbl_ram_variant_count; i++) {
        const gbl_ram_variant *v = &gbl_ram_variants[i];
        if ((uint16_t)(pc - v->addr) >= v->len) continue;
        const gbl_bank_table *t = &v->table;
        uint32_t page = (pc & 0x3FFFu) >> 7;
        if (!(GBL_TBL16(&t->idx[17 + (page >> 4)]) >> (page & 15) & 1)) continue;
        const uint8_t *ram = v->addr >= 0xFF80u ? gbl_hram + (v->addr - 0xFF80u)
                                                : gbl_wram + (v->addr - 0xC000u);
        uint16_t n = 0;
        while (n < v->len && ram[n] == gbl_load_u8(&v->bytes[n])) n++;
        if (n != v->len) continue;
        gbl_fn fn = lookup_table(t, pc);
        if (fn) return fn;
    }
    return 0;
}

#if GBL_TIMING_ASM && GBL_LOOKUP_CACHE
gbl_fn gbl_lookup_miss(uint16_t pc);
#define gbl_lookup gbl_lookup_miss
#endif
gbl_fn gbl_lookup(uint16_t pc) {
    /* Pages with nothing compiled are answered here and never cached: the
     * interpreter's transfers inside uncompiled code would evict entries. */
    if (pc >= 0x8000u) return gbl_ram_variant_count ? ram_lookup(pc) : 0;
    const gbl_bank_table *t = lookup_page(pc);
    if (!t) return 0;
#if GBL_LOOKUP_CACHE
    uint16_t bank = pc < 0x4000 ? 0 : gbr.rom_bank;
    unsigned i = (pc ^ (pc >> 5)) & (GBL_LOOKUP_CACHE - 1);
#if !GBL_TIMING_ASM /* the assembly gbl_lookup checked the entry already */
    if (lookup_cache[i].pc == pc && lookup_cache[i].bank == bank) return lookup_cache[i].fn;
#endif
    gbl_fn fn = lookup_table(t, pc);
    lookup_cache[i].pc = pc;
    lookup_cache[i].bank = bank;
    lookup_cache[i].fn = fn;
    return fn;
#else
    return lookup_table(t, pc);
#endif
}
#if GBL_TIMING_ASM && GBL_LOOKUP_CACHE
#undef gbl_lookup
#endif

/* The assembly interpreter's register offsets (B C D E H L - A) and the
 * innermost gbl_exec's bound (see gbl_exec), in RAM next to each other. */
gbl_interp_ctx_t gbl_interp_ctx = {{1, 0, 3, 2, 5, 4, 0xFF, 10}, 0, 0};
_Static_assert(offsetof(gbl_interp_ctx_t, sp0) == 8 && offsetof(gbl_interp_ctx_t, bounded) == 10,
               "gbl_interp_ctx_t layout");

/* gbl_exec only runs with GBL_EXEC_HEADROOM (gbl_options.h) above
 * gbl_stack_limit, so the region it calls always passes its own ENTER()
 * check: otherwise a region refused at entry would be called again forever
 * without progress (seen with deep call chains in R.C. Pro-Am under the
 * META's 3 KiB budget). */

/* The innermost gbl_exec's entry SP, or no bound under gbl_run: the
 * assembly interpreter continues across a transfer only while this loop
 * would call it again (gbl_interp_asm.S). */
#if GBL_CACHE_DYN
uint32_t gbl_exec_refused;
uintptr_t gbl_pool_sp;
#endif
#define gbl_exec_sp0 gbl_interp_ctx.sp0
#define gbl_exec_bounded gbl_interp_ctx.bounded

void gbl_exec(void) {
    char probe;
#if GBL_CACHE_DYN
    if (gbl_pool_sp && (uintptr_t)&probe > gbl_pool_sp) gbl_platform_pool();
#endif
    if ((uintptr_t)&probe < gbl_stack_limit + GBL_EXEC_HEADROOM) { /* unwind: callers resume */
#if GBL_CACHE_DYN
        gbl_exec_refused++; /* the frontend's measure of the stack's shortage */
#endif
        return;
    }
    uint16_t sp0 = gbr.sp, outer_sp0 = gbl_exec_sp0;
    uint8_t outer_bounded = gbl_exec_bounded;
    gbl_exec_sp0 = sp0;
    gbl_exec_bounded = 1;
    do {
        gbl_fn fn = gbl_interp_only ? 0 : gbl_lookup(gbr.pc);
        if (fn) fn(); else gbl_interp_block();
    } while ((int16_t)(gbr.sp - sp0) <= 0);
    gbl_exec_sp0 = outer_sp0;
    gbl_exec_bounded = outer_bounded;
}

void gbl_run(void) {
    char probe;
    /* Same margin as gbl_exec: a region called here must pass its guard,
     * otherwise it returns without progress and this loop never ends. */
    int room = (uintptr_t)&probe >= gbl_stack_limit + GBL_EXEC_HEADROOM;
    gbl_exec_bounded = 0;
    /* A host that cut the run from a device hook inside a wait (HALT jumps
     * straight to the next event) resumes here off the M-cycle grid; the CPU
     * would have resumed on the next boundary (see gbl_halt_wait). */
    if (gbr.cyc & 3u) {
        gbr.cyc = (gbr.cyc & ~3u) + 4u;
        gbl_sync();
    }
    for (;;) {
        gbl_fn fn = (gbl_interp_only || !room) ? 0 : gbl_lookup(gbr.pc);
        if (fn) fn(); else gbl_interp_block();
    }
}

/* ---- reset ---------------------------------------------------------------- */

/* Every page pointer, from the mapper registers. */
static void map_pages(void) {
    for (int i = 0; i < 16; i++) { gbl_rmap[i] = 0; gbl_wmap[i] = 0; }
    for (int i = 0; i < 4; i++) gbl_rmap[i] = gbl_rom;
    mapped_bank = 0xFFFF;
    map_rom_bank();
#if !GBL_ACCESS_BLOCK
    gbl_rmap[0x8] = gbl_rmap[0x9] = BIAS(gbl_vram, 0x8000u);
#endif
#if !GBL_VRAM_HOOK && !GBL_ACCESS_BLOCK
    gbl_wmap[0x8] = gbl_wmap[0x9] = BIAS(gbl_vram, 0x8000u);
#endif
#if GBL_CGB
#if !GBL_ACCESS_BLOCK
    gbl_rmap[0x8] = gbl_rmap[0x9] = BIAS(gbl_vram + (gbl_vbk ? 0x2000 : 0), 0x8000u);
#endif
    gbl_rmap[0xC] = gbl_wmap[0xC] = BIAS(gbl_wram, 0xC000u);
    gbl_rmap[0xD] = gbl_wmap[0xD] = BIAS(gbl_wram + gbl_svbk * 0x1000, 0xD000u);
#else
    gbl_rmap[0xC] = gbl_rmap[0xD] = BIAS(gbl_wram, 0xC000u);
    gbl_wmap[0xC] = gbl_wmap[0xD] = BIAS(gbl_wram, 0xC000u);
#endif
    /* E000-EFFF echoes C000-CFFF (F000-FDFF shares a page with OAM, IO
     * and HRAM and stays on the slow path). */
    gbl_rmap[0xE] = gbl_wmap[0xE] = BIAS(gbl_wram, 0xE000u);
    map_eram();
}

void gbl_reset(void) {
    __asm__ volatile("" : : "r"(&gbl_input_probe));
    memset(&gbr, 0, sizeof(gbr));
#if GBL_OAM_DMA_BUS
    dma_n = dma_pend = 0;
    dma_in = 0;
#endif
#if GBL_CGB
    gbl_cgb_pal_reset();
    gbl_hdma_active = 0; gbl_hdma_pend = 0; hdma_len = 0x7F; hdma_src = hdma_dst = 0;
    gbl_cgb = 0; gbl_vbk = 0; gbl_svbk = 1; gbl_key1 = 0; gbl_dshift = 0;
#endif
    memset(gbl_wram, 0, sizeof(gbl_wram));
    memset(gbl_vram, 0, sizeof(gbl_vram));
    memset(gbl_oam, 0, sizeof(gbl_oam));
    gbl_oam_dirty = 1;
    memset(gbl_hram, 0, sizeof(gbl_hram));
    memset(gbl_io, 0, sizeof(gbl_io));
    memset(&mbc, 0, sizeof(mbc));
#if GBL_MBC == 5
    mbc.lo = 1; /* ROMB0 powers up on bank 1 */
#endif
#if GBL_ERAM_SIZE > 0
    eram_reset();
#endif
#if GBL_LOOKUP_CACHE
    /* pc 0xFFFF is never looked up (>= 0x8000): an empty slot. */
    for (int i = 0; i < GBL_LOOKUP_CACHE; i++) lookup_cache[i].pc = 0xFFFF;
#endif
#if GBL_ROM_STREAM
    stream_reset();
#endif

    /* DMG register state after the boot ROM. */
    gbr.a = 0x01; gbr.zr = 0; gbr.nf = 0; gbr.hf = 1; gbr.cf = 1;
    gbr.bc = 0x0013; gbr.de = 0x00D8; gbr.hl = 0x014D;
    gbr.sp = 0xFFFE; gbr.pc = 0x0100;
    gbl_ie = 0;
    gbl_io[0x00] = 0x30;
    joypad_update();
    IF = 0x01;
    TAC = 0; TIMA = 0; TMA = 0;
    div_base = (uint32_t)0 - 0xABCCu;
    tima_sync = 0;
    serial_busy = 0;
    core_invalidate();

    map_pages();
#if GBL_CGB
    /* a cartridge with $143 bit 7 boots in colour mode: A=$11 (CGB) */
    if (gbl_rd(0x143) & 0x80) {
        gbl_cgb = 1;
        gbr.a = 0x11; gbr.zr = 1; gbr.nf = 0; gbr.hf = 0; gbr.cf = 0;
        gbr.bc = 0x0000; gbr.de = 0xFF56; gbr.hl = 0x000D;
    }
#endif

    gbl_apu_reset();
    gbl_ppu_reset();
    gbl_update_evt();
}

/* ---- save states ------------------------------------------------------------ */

static uint8_t io_counting; /* gbl_state_bytes: no card access */

static void state_mem(gbl_state_fn io, void *ctx, int load) {
    io(&gbr, sizeof(gbr), ctx);
    io(gbl_wram, sizeof(gbl_wram), ctx);
    io(gbl_vram, sizeof(gbl_vram), ctx);
    io(gbl_oam, sizeof(gbl_oam), ctx);
    io(gbl_hram, sizeof(gbl_hram), ctx);
    io(gbl_io, sizeof(gbl_io), ctx);
    io(&gbl_ie, sizeof(gbl_ie), ctx);
    io(&mbc, sizeof(mbc), ctx);
    io(&gbl_timer_state, sizeof(gbl_timer_state), ctx);
    io(&buttons, sizeof(buttons), ctx);
    io(&gbl_frame, sizeof(gbl_frame), ctx);
#if GBL_CGB
    io(&gbl_cgb, sizeof(gbl_cgb), ctx);
    io(&gbl_vbk, sizeof(gbl_vbk), ctx);
    io(&gbl_svbk, sizeof(gbl_svbk), ctx);
    io(&gbl_key1, sizeof(gbl_key1), ctx);
    io(&gbl_hdma_active, 1, ctx);
    io(&hdma_src, sizeof(hdma_src), ctx);
    io(&hdma_dst, sizeof(hdma_dst), ctx);
    io(&hdma_len, 1, ctx);
    io(gbl_cgb_pal, sizeof(gbl_cgb_pal), ctx);
    io(gbl_cgb_ps, sizeof(gbl_cgb_ps), ctx);
    io(&gbl_dshift, sizeof(gbl_dshift), ctx);
#endif
#if GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD
    /* The cartridge RAM is the save file: copied through the sector cache's
     * first buffer, whose contents were written back first. */
    if (!io_counting) gbl_save_flush();
    uint8_t *buf = gbl_save_scratch(); /* the cache is emptied after */
    for (uint32_t sec = 0; sec * 512u < GBL_ERAM_SIZE; sec++) {
        if (io_counting) {
            io(buf, 512, ctx);
        } else if (load) {
            io(buf, 512, ctx);
            gbl_platform_save_write(sec, buf);
        } else {
            if (!gbl_platform_save_read(sec, buf)) memset(buf, 0xFF, 512);
            io(buf, 512, ctx);
        }
    }
    if (!io_counting) eram_reset();
#elif GBL_ERAM_SIZE > 0
    io(gbl_eram, GBL_ERAM_SIZE, ctx);
#if GBL_BATTERY
    if (load) { /* the save file follows the loaded RAM (gbl_save_tick) */
        eram_dirty_mask = 0xFFFFFFFFu >> (32u - (GBL_ERAM_SIZE + 511u) / 512u);
        save_pending = 1;
        gbl_save_last_change = gbl_frame;
    }
#endif
#endif
    (void)load;
}

void gbl_state_io(gbl_state_fn io, void *ctx, int load) {
    state_mem(io, ctx, load);
    gbl_ppu_state_io(io, ctx, load);
    gbl_apu_state_io(io, ctx, load);
    if (!load) return;
    map_pages();
#if GBL_LOOKUP_CACHE
    for (int i = 0; i < GBL_LOOKUP_CACHE; i++) lookup_cache[i].pc = 0xFFFF;
#endif
    joypad_update();
    core_invalidate();
    gbl_update_evt();
}

static void count_bytes(void *data, uint32_t bytes, void *ctx) {
    (void)data;
    *(uint32_t *)ctx += bytes;
}

/* Size of a state (nothing is read or written: counting only). */
uint32_t gbl_state_bytes(void) {
    uint32_t n = 0;
    io_counting = 1;
    gbl_state_io(count_bytes, &n, 0);
    io_counting = 0;
    return n;
}
