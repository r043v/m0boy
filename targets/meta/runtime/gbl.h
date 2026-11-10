/*
 * gbl - compact DMG runtime for memory-constrained targets (Gamebuino META).
 *
 * The desktop runtime (runtime/) favours instrumentation and accuracy
 * tooling; this one favours flash and RAM footprint.  Devices are advanced
 * lazily from a single T-cycle counter and only publish events when an
 * interrupt or a rendered scanline can become observable.
 *
 * Flag representation used by both the interpreter and generated code:
 *   Z is set      <=> gbr.zr == 0
 *   C, H, N       are stored as 0/1 bytes in cf, hf, nf
 */
#ifndef GBL_H
#define GBL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte fields stay below offset 32 and words below 128 so Thumb-1 can reach
 * every register with a single immediate-offset load/store. */
typedef struct {
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint16_t sp;
    uint16_t pc;
    uint8_t a;
    uint8_t zr;
    uint8_t cf;
    uint8_t hf;
    uint8_t nf;
    uint8_t ime;
    uint16_t rom_bank;
    uint32_t cyc;
    uint32_t next_evt;
} gbl_cpu_t;

extern gbl_cpu_t gbr;

#if defined(__has_include)
#if __has_include("gbl_config.h")
#include "gbl_config.h" /* GBL_CGB decides the RAM sizes below */
#endif
#endif
#if defined(GBL_CGB) && GBL_CGB
/* CGB: WRAM as 8 banks of 4 KiB (bank 0 first, bank 1 at +$1000: the DMG
 * layout is the first 8 KiB), VRAM as 2 banks of 8 KiB. */
#define GBL_WRAM_SIZE 0x8000
#define GBL_VRAM_SIZE 0x4000
extern uint8_t gbl_cgb;  /* 1 when the cartridge runs in colour mode */
extern uint8_t gbl_vbk;  /* FF4F bit 0 */
extern uint8_t gbl_svbk; /* FF70 bank, 1..7 */
#else
#define GBL_WRAM_SIZE 0x2000
#define GBL_VRAM_SIZE 0x2000
#endif
extern uint8_t gbl_wram[GBL_WRAM_SIZE];
extern uint8_t gbl_vram[GBL_VRAM_SIZE];
extern uint8_t gbl_oam[0xA0];
extern uint8_t gbl_hram[0x80];
extern uint8_t gbl_io[0x80];
extern uint8_t gbl_ie;

/* Biased page pointers: gbl_rmap[a >> 12][a] is the byte at a, NULL pages go
 * through the slow path (IO, OAM, MBC registers, disabled cart RAM). */
extern const uint8_t *gbl_rmap[16];
extern uint8_t *gbl_wmap[16];

/* Provided by the generated code (empty tables for interpreter-only
 * projects). */
typedef void (*gbl_fn)(void);
typedef struct {
    uint16_t count;
    const uint16_t *addr; /* sorted */
    const gbl_fn *fn;
    /* Within the bank's 16 KiB: idx[i] (i <= 16) is the first entry at or
     * above offset 0x400 * i, and bit p % 16 of idx[17 + p / 16] is set when
     * 128-byte page p holds an entry. */
    const uint16_t *idx;
} gbl_bank_table;
extern const gbl_bank_table gbl_tables[];
/* Executed RAM code (WRAM $C000-$DFFF, HRAM $FF80-$FFFE) compiled from a
 * recorded variant (gbrecomp --ram-code).  The dispatch enters a variant's
 * table only while the RAM still holds exactly `bytes`; any other content
 * (rewritten, not yet copied) runs in the interpreter.  Absent from a
 * project without variants: gbl_core.c defines empty weak defaults. */
typedef struct {
    uint16_t addr, len;
    const uint8_t *bytes;
    gbl_bank_table table;
} gbl_ram_variant;
extern const gbl_ram_variant gbl_ram_variants[];
extern const uint16_t gbl_ram_variant_count;
extern const uint16_t gbl_table_count;
/* ESP8266's mapped flash only accepts aligned word loads. RAM keeps its
 * normal byte accesses; host and ARM builds compile to the original load. */
#if defined(ESP8266)
#include <pgmspace.h>
static inline uint8_t gbl_load_u8(const uint8_t *p) {
    return (uintptr_t)p >= 0x40200000u ? pgm_read_byte(p) : *p;
}
/* The generated dispatch tables stay in flash (DRAM is 80 KiB): 16-bit
 * entries are read with aligned word loads. */
#define GBL_TABLE PROGMEM __attribute__((aligned(4)))
#define GBL_TBL16(p) pgm_read_word(p)
#else
static inline uint8_t gbl_load_u8(const uint8_t *p) { return *p; }
#define GBL_TABLE
#define GBL_TBL16(p) (*(p))
#endif
/* ESPboy: the whole ROM, or bank 0 of a larger one (the rest is rom.bin,
 * gbl_espboy.h), is in the application's flash. */
extern const uint8_t gbl_rom[];
extern const uint32_t gbl_rom_size;

/* ---- lifecycle ---------------------------------------------------------- */
void gbl_reset(void);
void gbl_run(void);                 /* never returns */

/* ---- platform hooks (implemented by the host / Meta frontend) ---------- */
/* Destination for DMG scanline ly (160 pixels, 4-byte aligned), or NULL to
 * skip rendering it.  Pixels are gbl_lcd_palette[shade], shade 0 = white ..
 * 3 = black.  Call gbl_ppu_palette_changed() after editing the palette. */
uint16_t *gbl_platform_line_begin(uint8_t ly);
void gbl_platform_line_end(uint8_t ly);
/* With GBL_LINE_SIGNATURE: called before line_begin with a signature of all
 * inputs of line ly (not including gbl_lcd_palette); return 0 to skip it. */
int gbl_platform_line_check(uint8_t ly, uint32_t sig);
/* Nonzero: line_begin buffers receive 20 groups of eight BGP/OBP-mapped
 * 2-bit shades (uint16, leftmost pixel in bits 15:14) instead of 160
 * colours; gbl_lcd_palette is then not used for that line. */
extern uint8_t gbl_ppu_packed;
/* With GBL_PPU_PAL32: set to 0 while gbl_lcd_palette holds a temporary
 * palette so the 2 KiB table is not rebuilt for it. */
extern uint8_t gbl_lcd_palette_primary;
extern uint16_t gbl_lcd_palette[4];
void gbl_ppu_palette_changed(void);
/* Signature of the frame whose visible lines just completed (valid in
 * gbl_platform_vblank); equal signatures mean identical pictures.  Returns
 * 0 when GBL_FRAME_SIGNATURE is disabled. */
uint32_t gbl_ppu_frame_signature(void);
/* Static-frame policy, called once per VBlank: pass whether the frame that
 * just ended was rendered and displayed; returns nonzero when the next frame
 * is expected to look exactly like the picture on screen (signature stable
 * for two frames).  A change is then shown one frame late.  Call
 * gbl_frame_invalidate() when the frontend's palette or layout changes. */
int gbl_frame_unchanged_next(int shown);
/* Order-dependent 32-bit combination of two signatures (full avalanche). */
uint32_t gbl_sig_combine(uint32_t a, uint32_t b);
void gbl_frame_invalidate(void);
/* Entered VBlank.  The frontend presents, paces, and polls input here. */
void gbl_platform_vblank(void);
/* Set by the frontend: nonzero skips scanline rasterization this frame. */
extern uint8_t gbl_skip_render;
/* Joypad state, 1 = pressed.  bit0 A, 1 B, 2 Select, 3 Start,
 * 4 Right, 5 Left, 6 Up, 7 Down. */
void gbl_set_buttons(uint8_t pressed);
extern uint8_t gbl_joyp;           /* P1 (0xFF00) as read */

/* ---- ROM streaming (projects generated with GBL_ROM_STREAM) ------------- */
/* Read 512-byte sector `sector` of the ROM (byte offset sector * 512) into
 * dst; return 0 on failure (the bytes then read as 0xFF). */
int gbl_platform_rom_read(uint32_t sector, uint8_t *dst);
/* 512 bytes of the stream cache, free for the platform before gbl_reset()
 * (e.g. to locate the ROM file); gbl_reset() empties the cache. */
uint8_t *gbl_stream_scratch(void);
/* Empties the stream cache, after the platform used all its buffers
 * (GBL_STREAM_SECTORS x 512 bytes from gbl_stream_scratch()). */
void gbl_stream_invalidate(void);
/* GBL_CACHE_UNIFIED: a stack for the option menu on the cache's pool (all
 * but 3 blocks, written back first), out of the cache until
 * gbl_stream_invalidate. */
uintptr_t gbl_cache_lend(void);
/* GBL_CACHE_DYN (gbl_options.h): the blocks after the cache's fixed part,
 * GBL_CACHE_DYN_BLOCK bytes each.  gbl_cache_top is the end of the last
 * one in use; gbl_cache_grow adds the block at gbl_cache_top (the caller
 * made sure nothing uses that RAM), 0 at the most; gbl_cache_shrink takes
 * the last one out, its RAM free on return, 0 when only the fixed part is
 * left or its cartridge RAM could not be written back. */
uintptr_t gbl_cache_top(void);
extern uint32_t gbl_cache_loads;  /* blocks the cache loaded (ROM and cartridge RAM) */
extern uint32_t gbl_exec_refused; /* gbl_exec calls refused for want of stack (unwinds) */
/* Nonzero: gbl_exec calls gbl_platform_pool on entry with its frame above
 * this address (the stack pool's blocks taken back, m0_stack_pool_run). */
extern uintptr_t gbl_pool_sp;
void gbl_platform_pool(void);
unsigned gbl_cache_ways(void);
int gbl_cache_grow(void);
int gbl_cache_shrink(void);
extern uint32_t gbl_stream_misses;  /* sectors read so far */
extern uint32_t gbl_stream_unpacked; /* ... decoded from compressed flash */

/* ---- cartridge RAM saves ------------------------------------------------- */
/* Sector `sector` (512 bytes) of the cartridge RAM file; return 0 when there
 * is no file (reads then give 0xFF, writes are dropped). */
int gbl_platform_save_read(uint32_t sector, uint8_t *dst);
int gbl_platform_save_write(uint32_t sector, const uint8_t *src);
/* MBC3 RTC: seconds since the platform's epoch; 0 when the console has no
 * clock (the counter then advances only while powered, 1 s per 60 frames).
 * Weak default in gbl_core.c; a platform with a real clock overrides it. */
uint32_t gbl_platform_rtc(void);
#if defined(ESP8266) || defined(GBL_SAVE_IMAGE)
/* The whole battery RAM as a new copy in flash (0 when it could not be written). */
int gbl_platform_save_image(const uint8_t *eram, uint32_t bytes);
#endif
void gbl_save_flush(void);           /* write back what changed */
/* Write back the first changed sector (cache way, or 512-byte sector of
 * in-RAM cartridge RAM) at index i or after; returns the index after it,
 * or -1 when none is left.  Spreads a periodic save over several frames. */
int gbl_save_flush_next(int i);
int gbl_save_pending(void);          /* changes not written yet */
extern uint32_t gbl_save_last_change; /* gbl_frame of the last change */
/* With GBL_ERAM_ON_SD: cached cartridge RAM sectors read and written by
 * gbl_rd / gbl_wr without the slow path, per 512-byte slot of A000-BFFF
 * (gbl_core.c). */
extern uint8_t *gbl_eram_rmap[], *gbl_eram_wmap[]; /* 0x2000 / GBL_CACHE_BLOCK entries */
/* With GBL_DEBUG_ERAM_MAPS: an access through those tables that the sector
 * search contradicts (meta-usb.py eram decodes it; keep the layout in
 * step). */
typedef struct {
    uint32_t frame, got_ptr, want_ptr;
    uint16_t addr, pc;
    uint8_t got_v, want_v, bank, ram_enable;
    int8_t got_way, want_way;
    uint8_t mru, ways;
    int16_t tag[16];     /* sector of each way (-1 empty, -2 no such way) */
    int8_t rmap_way[16]; /* way each read slot points to (-1 none, -2 no buffer) */
    int8_t wmap_way[16]; /* the same for writes */
    uint8_t kind, pad[3]; /* 0 read, 1 write */
    uint32_t flags;      /* 2 bits per way: referenced, dirty */
} gbl_dbg_eram_rec_t;
typedef struct {
    uint32_t reads, writes, errors, rebuilds;
    gbl_dbg_eram_rec_t first, last;
} gbl_dbg_eram_t;
extern gbl_dbg_eram_t gbl_dbg_eram;
uint8_t gbl_dbg_eram_rd(uint16_t a, uint8_t v);
/* With GBL_DEBUG_RESTART (gbl_core.c): button changes (Game Boy bits,
 * gbl_set_buttons), and a copy of the last 16 at each RAM enable write.
 * meta-usb.py restart decodes it; keep the layout in step. */
typedef struct {
    uint32_t frame, combo_frames;
    uint16_t addr, sp;
    uint8_t value, ram_enable, bank, pad;
    uint8_t buttons[16];
    uint16_t button_frame[16];
} gbl_dbg_restart_snap_t;
typedef struct {
    uint32_t writes, combo_frames, pos;
    uint8_t buttons[16];
    uint16_t button_frame[16];
    gbl_dbg_restart_snap_t snap[2];
    /* ROM bank writes (2000-3FFF), with $A24E (Super Mario Land 2's copy
     * of the bank; EE when not cached), frozen at the first undefined
     * opcode (lock_frame, lock_pc, lock_bank). */
    uint32_t bank_pos, lock_frame;
    uint16_t lock_pc, lock_bank;
    uint8_t bank_val[16], bank_shadow[16];
    uint16_t bank_frame[16];
} gbl_dbg_restart_t;
extern gbl_dbg_restart_t gbl_dbg_restart;
/* The first write that switches cartridge RAM off (GBL_DEBUG_RESTART):
 * the native return address and C stack (the generated region that made
 * it), the Game Boy registers and the top of its stack. */
typedef struct {
    uint32_t frame, lr, sp;
    uint16_t addr, value;
    uint32_t gbr[6];
    uint8_t gb_stack[16];
    uint32_t c_stack[32];
} gbl_dbg_off_t;
extern gbl_dbg_off_t gbl_dbg_off;
/* gbl_eram_rmap / gbl_eram_wmap against what they should hold
 * (GBL_DEBUG_RESTART): checked at each VBlank (where 0) and on a fault
 * (where 1); the first and the last wrong entries with the 17 words around
 * them, then the entry is repaired.  Also writes to echo RAM. */
typedef struct {
    uint32_t frame, where, slot, addr, bad, want;
    uint32_t around[17];
} gbl_dbg_tables_rec_t;
typedef struct {
    uint32_t magic, checks, bad, echo_writes, echo_last, echo_frame;
    gbl_dbg_tables_rec_t rec[2];
} gbl_dbg_tables_t;
extern gbl_dbg_tables_t gbl_dbg_tables;
void gbl_dbg_tables_check(uint32_t where);
/* Interrupts against their RETI (GBL_DEBUG_RESTART): the registers at the
 * entry (gbl_irq_enter) must be back, and the return address the one
 * pushed, when the RETI pops the entry's frame.  The first difference is
 * kept (.noinit) with the Game Boy stack around sp. */
typedef struct {
    uint32_t frame;
    uint16_t sp, pc, bc, de, hl, bank;
    uint8_t a, f, vector, pad;
} gbl_dbg_irq_regs_t;
typedef struct {
    uint32_t magic, entries, checked, bad, unmatched;
    gbl_dbg_irq_regs_t at_entry, at_reti;
    uint8_t stack[24]; /* Game Boy stack from sp - 16 at the RETI */
} gbl_dbg_irq_t;
extern gbl_dbg_irq_t gbl_dbg_irq;
void gbl_dbg_reti(void);
/* The first interrupt taken at a pc in 0068-014F (GBL_DEBUG_RESTART): data
 * and the cartridge header in most games, so a pc gone wrong before the
 * entry.  The native return address of gbl_irq_enter (interpreter or
 * gbl_event) and the C stack above it name the path that left it. */
typedef struct {
    uint32_t magic, frame, lr, csp;
    uint16_t pc, sp, bc, de, hl, bank;
    uint8_t a, ime, gb_stack[14];
    uint32_t c_stack[40];
} gbl_dbg_badirq_t;
extern gbl_dbg_badirq_t gbl_dbg_badirq;
/* With GBL_DEBUG_ERAM_SUM (gbl_core.c): the cache's SD round trips. kind
 * of the last error: 1 an evicted sector read back at once differs, 2 a
 * sector read back later differs from what was last written. */
typedef struct {
    uint32_t writes, verifies, loads, errors, reread_ok, kind, sec, want, got, frame;
} gbl_dbg_eram_sum_t;
extern gbl_dbg_eram_sum_t gbl_dbg_eram_sum;
void gbl_dbg_eram_wr(uint16_t a, uint8_t v);
/* With GBL_DEBUG_STACK (gbl_core.c): a shadow of GBL_DBG_STACK_LEN bytes of
 * cartridge RAM from GBL_DBG_STACK_BASE, written with every store there
 * (with the writer's gbr.pc, gbr.sp, frame and native return address / 4),
 * and each pop's bytes compared with it.  Frozen at the first difference
 * (frozen 2) or the first write switching cartridge RAM off (frozen 1). */
#ifndef GBL_DBG_STACK_LEN
#define GBL_DBG_STACK_LEN 64u
#endif
#define GBL_DBG_STACK_BASE (0xA900u - GBL_DBG_STACK_LEN)
typedef struct {
    uint32_t magic, pops, checked, bad, frozen, frozen_frame;
    uint32_t bad_frame;
    uint16_t bad_sp, bad_addr, bad_pc, bad_bank;
    uint8_t bad_got, bad_want;
    uint16_t len; /* GBL_DBG_STACK_LEN: the shadow ends at A8FF */
    uint8_t value[GBL_DBG_STACK_LEN], valid[GBL_DBG_STACK_LEN / 8];
    uint16_t pc[GBL_DBG_STACK_LEN], sp[GBL_DBG_STACK_LEN], frame[GBL_DBG_STACK_LEN], lr[GBL_DBG_STACK_LEN];
    /* Cartridge RAM on the card: the shadowed sector's loads into the cache
     * and write-backs (count, last frame); the load count when each byte
     * was written; at the wrong pop, the cache (flags: ref | dirty << 1 per
     * way), the ways the read and write tables and the search give for
     * the sector, and the sector's bytes as the cache held them. */
    uint32_t loads, writebacks, load_frame, writeback_frame, flags;
    int16_t tag[16];
    int8_t rmap_way, wmap_way, tag_way, mru, hand, pad[3];
    uint8_t at_write_loads[GBL_DBG_STACK_LEN], actual[GBL_DBG_STACK_LEN];
} gbl_dbg_stack_t;
extern gbl_dbg_stack_t gbl_dbg_stack;
void gbl_dbg_stack_note(uint16_t a, uint8_t v, uint32_t lr);
void gbl_dbg_stack_wr(uint16_t a, uint8_t v, uint8_t *e);
uint16_t gbl_dbg_pop(uint16_t sp, uint16_t v);
/* 512 bytes free for the platform before gbl_reset() (NULL without
 * cartridge RAM). */
uint8_t *gbl_save_scratch(void);

/* ---- audio -------------------------------------------------------------- */
/* Unsigned 8-bit mono samples (128 = silence) at the configured rate. */
void gbl_apu_set_rate(uint32_t hz);
void gbl_apu_render(uint8_t *out, unsigned n);
extern uint8_t gbl_apu_mute;        /* debug: bit c silences channel c */

/* ---- memory ------------------------------------------------------------- */
uint8_t gbl_rd(uint16_t a);
void gbl_wr(uint16_t a, uint8_t v);
uint8_t gbl_io_rd(uint8_t n);        /* 0xFF00 + n, n < 0x80 */
void gbl_io_wr(uint8_t n, uint8_t v);
void gbl_vram_wr(uint16_t off, uint8_t v); /* off = address - 0x8000 */
void gbl_push(uint16_t v);
uint16_t gbl_pop(void);

/* ---- execution control -------------------------------------------------- */
int gbl_event(uint16_t resume_pc);   /* nonzero: control left resume_pc */
int gbl_halt(uint16_t resume_pc);
void gbl_exec(void);                 /* run from gbr.pc until the frame returns */
void gbl_sync(void);                 /* advance devices to gbr.cyc */
void gbl_update_evt(void);
gbl_fn gbl_lookup(uint16_t pc);
void gbl_interp_block(void);

/* ---- wait-loop fast-forward (generated back edges) ---------------------- */
/* Advance gbr.cyc by whole loop periods while every skipped iteration would
 * read the same values.  kind: 0 reads change only when code runs, 1 at a
 * device event, 2 at the next LY/STAT change after a read read_ofs cycles
 * into the iteration, 3 never skip (see gbl_spin_kind). */
void gbl_spin(uint32_t period, uint32_t read_ofs, uint32_t kind);
/* Delay loop: skip up to max iterations; returns how many were skipped. */
uint32_t gbl_spin_n(uint32_t period, uint32_t max);
uint32_t gbl_spin_kind(uint16_t addr);

/* ---- ALU helpers (full flag results) ----------------------------------- */
void gbl_add(uint8_t v);
void gbl_adc(uint8_t v);
void gbl_sub(uint8_t v);
void gbl_sbc(uint8_t v);
void gbl_cp(uint8_t v);
uint8_t gbl_inc(uint8_t v);
uint8_t gbl_dec(uint8_t v);
void gbl_addhl(uint16_t v);
uint16_t gbl_spofs(uint8_t v);       /* SP + e8 with flags (ADD SP / LD HL,SP+) */
void gbl_daa(void);
uint8_t gbl_cbop(uint8_t op, uint8_t v); /* CB 00-3F rotate/shift group */
uint8_t gbl_getf(void);
void gbl_setf(uint8_t f);

/* ---- save states ------------------------------------------------------- */
/* The machine (CPU, memories, mapper, timer, PPU, APU and the cartridge RAM,
 * also when it lives in the save file) is streamed through io: the same
 * order for saving and loading.  Call them at an instruction boundary,
 * i.e. from gbl_platform_safe_point; after gbl_state_io(.., 1) execution
 * must start over from gbr.pc (gbl_run again, the C stack dropped). */
typedef void (*gbl_state_fn)(void *data, uint32_t bytes, void *ctx);
void gbl_state_io(gbl_state_fn io, void *ctx, int load);
uint32_t gbl_state_bytes(void);
/* Set by the frontend: gbl_platform_safe_point() is called at the next
 * interrupt check, between two instructions with gbr.pc exact. */
extern uint8_t gbl_safe_request;
void gbl_platform_safe_point(void); /* weak default: nothing */

/* ---- diagnostics -------------------------------------------------------- */
extern uint8_t gbl_interp_only;     /* ignore compiled code (reference mode) */
extern uint32_t gbl_frame;
extern uint16_t gbl_rom0_bank; /* bank in the $0000-$3FFF window (MBC1 mode 1 only) */
extern uint32_t gbl_interp_instrs;
extern uintptr_t gbl_stack_limit;   /* lowest C stack address gbl_exec may use */
#ifdef GBL_TRACE
void gbl_trace_hook(uint16_t pc);
#endif
#ifdef GBL_COVERAGE
void gbl_coverage_hit(uint16_t bank, uint16_t addr, int entry);
#endif

#ifdef __cplusplus
}
#endif

#endif
