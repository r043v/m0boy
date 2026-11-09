#ifndef M0_GG_H
#define M0_GG_H
/* Game Gear and Master System runtime shared by recompiled code and the Z80
 * interpreter (the Master System: GG_IS_SMS, notes below).
 *
 * Same engine as the SG-1000 runtime (sg1000.h): time is a T-state counter
 * with device events (VBlank, frame end, line interrupts), lines are drawn
 * lazily when the program touches the VDP or at VBlank, and the PSG is
 * caught up before each write.  The Game Gear adds the Sega mapper (three
 * 16 KiB slots paged by FFFD-FFFF, 0000-03FF fixed), 8 KiB of RAM, the
 * mode 4 VDP with its 32-colour CRAM, and the line counter interrupt.
 *
 * The screen is the 160x144 window of the VDP's 256x192 picture (columns
 * 48-207, lines 24-167), drawn 1:1: no horizontal filter is needed.
 *
 * Master System: the same VDP, mapper and PSG.  The whole 256x192 picture
 * is shown, box-filtered to 160 pixels (8 -> 5, as the SG-1000 and the
 * Game Gear's own SMS mode); 6-bit CRAM; joypad ports with the I/O control
 * register (TH lines read back: export machine); PAUSE is the NMI; no
 * Game Gear ports 0-6. */
#include "z80.h"
#include <stddef.h>
#if defined(__has_include)
#if __has_include("sg_config.h")
#include "sg_config.h"
#endif
#endif
#include "m0_sega_mapper.h"
/* Normal generated projects keep their original three-slot layout. */
#if !defined(GG_MAPPER) || GG_MAPPER == 2
#define GG_SLOTS 6
#else
#define GG_SLOTS 3
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define GG_LINES 262
#define GG_LINE_CYCLES 228
#define GG_FRAME_CYCLES (GG_LINES * GG_LINE_CYCLES)
#define GG_ACTIVE 192                 /* VDP lines with picture */
/* The frame interrupt is flagged at line 193 (V counter C1): C0 can be
 * polled for a whole line before it (games wait for it). */
#define GG_VBLANK_CYCLES ((GG_ACTIVE + 1) * GG_LINE_CYCLES)
/* The line counter counts (and interrupts) near the end of each line, so a
 * line interrupt on line 192 comes before the frame interrupt. */
#define GG_LINE_IRQ_AT 200
#define GG_WIDTH 160
#define GG_HEIGHT 144
/* Device line width: the Game Gear window's own 160, box-filtered from the
 * VDP's 256 columns; a generated PC Master System project keeps all 256
 * columns 1:1 (sg_config.h) — no filter, no window crop. */
#ifndef GG_OUT_WIDTH
#define GG_OUT_WIDTH GG_WIDTH
#endif
#define GG_TOP 24                     /* first VDP line shown */
#define GG_LEFT 48                    /* first VDP column shown */
/* Generated projects fix the system (GG_SMS); the generic host chooses at
 * gg_init. */
#ifdef GG_SMS
#define GG_IS_SMS (GG_SMS)
#else
#define GG_IS_SMS (machine.sms)
#endif
#define GG_WIN_TOP (GG_IS_SMS ? 0u : (unsigned)GG_TOP)
#define GG_WIN_LINES (GG_IS_SMS ? (unsigned)GG_ACTIVE : (unsigned)GG_HEIGHT)
/* First source column shown: the Game Gear window's crop, or all of them at
 * the native 256. */
#if GG_OUT_WIDTH == GG_WIDTH
#define GG_WIN_LEFT GG_LEFT
#else
#define GG_WIN_LEFT 0u
#endif
/* Rows given to the renderer have this many writable pixels on each side
 * (fine scroll writes whole tiles). */
#define GG_ROW_PAD 8
#define GG_ROW_STRIDE (GG_OUT_WIDTH + 2 * GG_ROW_PAD)
/* Cartridge RAM: a 16 KiB window at 8000-BFFF, two banks (FFFC bit 2) */
#if defined(GG_CART_RAM_8K) /* ESPboy: 8 KiB in RAM, mirrored over the window */
#define GG_CART_RAM_SIZE 8192
static inline uint32_t gg_cart_offset(uint16_t a, uint8_t ram_ctl) { (void)ram_ctl; return a & 0x1fffu; }
#else
#define GG_CART_RAM_SIZE 32768
static inline uint32_t gg_cart_offset(uint16_t a, uint8_t ram_ctl) { return (ram_ctl & 4u) << 12 | (a & 0x3fffu); }
#endif
/* ROM larger than the flash (SG_ROM_STREAM, generated projects): only some
 * 16 KiB pages are in flash (gg_page_index), the others are read from the
 * SD card through a cache of GG_STREAM_SECTORS 512-byte sectors. */
#if defined(SG_ROM_STREAM) && SG_ROM_STREAM
#define GG_STREAM 1
#ifndef GG_STREAM_SECTORS
#define GG_STREAM_SECTORS 2
#endif
#else
#define GG_STREAM 0
#endif
/* Cartridge RAM in the save file on the SD card (consoles: no room for
 * 8 KiB more), through the same sector cache, written back when the game
 * disables the RAM and two seconds after the last change. */
#if defined(GG_CART_SD) && GG_CART_SD
#define GG_CART_ON_CARD 1
#ifndef GG_STREAM_SECTORS
#define GG_STREAM_SECTORS 2
#endif
#else
#define GG_CART_ON_CARD 0
#endif
/* The frontend opens the SD card: the ROM or the cartridge RAM are there,
 * or the save states' file (GG_STATE_NAME83, sg_config.h).  The ESPboy
 * (SG_FLASH_GAME_ID) has none: its ROM and states are in flash. */
#if (GG_STREAM || GG_CART_ON_CARD || defined(GG_STATE_NAME83)) && !defined(SG_FLASH_GAME_ID)
#define GG_CARD 1
#else
#define GG_CARD 0
#endif
/* The card cache (m0_cache.h, the Game Boy's): GG_CACHE_BYTES in blocks of
 * GG_CACHE_BLOCK bytes (128, 256 or 512), plus one 512-byte sector buffer
 * under 512.  By default GG_STREAM_SECTORS sectors of RAM in all. */
#if GG_STREAM || GG_CART_ON_CARD
#ifndef GG_CACHE_BLOCK
#define GG_CACHE_BLOCK 512
#endif
#ifndef GG_CACHE_BYTES
#define GG_CACHE_BYTES (GG_STREAM_SECTORS * 512 - (GG_CACHE_BLOCK < 512 ? 512 : 0))
#endif
#if GG_CACHE_BYTES < 512 || GG_CACHE_BYTES % GG_CACHE_BLOCK
#error "GG_CACHE_BYTES: whole blocks, at least 512 bytes (gg_stream_scratch)"
#endif
/* GG_CACHE_DYN (META builds): the cache is a fixed part, the option menu's
 * stack (GG_CACHE_FIXED_BYTES, .cachepool, last in RAM), and blocks after
 * it in the C stack's RAM, up to GG_CACHE_MAX_BYTES: gg_main.cpp fills
 * them at boot while the stack keeps its guard, then lends them to the
 * stack and takes them back (m0_stack_pool.h).  GG_CACHE_BYTES is then
 * the fixed part.  Off on the Pokitto (the cache is in the USB RAM bank),
 * the ESPboy and the host. */
#if defined(__has_include)
#if __has_include("gbl_config.h")
#include "gbl_config.h"
#endif
#endif
#ifndef GG_CACHE_DYN
#if defined(__ARM_ARCH_6M__) && !(defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO) && !defined(GG_BENCH)
#define GG_CACHE_DYN 1
#else
#define GG_CACHE_DYN 0
#endif
#endif
#if GG_CACHE_DYN
#ifndef GG_CACHE_FIXED_BYTES
#define GG_CACHE_FIXED_BYTES (GG_CACHE_BLOCK < 512 ? 768 : 1024)
#endif
#ifndef GG_CACHE_MAX_BYTES
#define GG_CACHE_MAX_BYTES (GG_CACHE_BLOCK * (4096 / GG_CACHE_BLOCK > 255 ? 255 : 4096 / GG_CACHE_BLOCK))
#endif
#undef GG_CACHE_BYTES
#define GG_CACHE_BYTES GG_CACHE_FIXED_BYTES
#if GG_CACHE_FIXED_BYTES < 512 || GG_CACHE_FIXED_BYTES % GG_CACHE_BLOCK || GG_CACHE_MAX_BYTES < GG_CACHE_FIXED_BYTES
#error "GG_CACHE_FIXED_BYTES: whole blocks, at least 512 bytes, up to GG_CACHE_MAX_BYTES"
#endif
uintptr_t gg_cache_top(void);
unsigned gg_cache_ways(void);
int gg_cache_grow(void);
int gg_cache_shrink(void);
extern uint32_t gg_cache_loads;  /* blocks the cache loaded (ROM and cartridge RAM) */
extern uint32_t sg_exec_refused; /* sg_exec calls refused for want of stack (unwinds) */
/* Nonzero: sg_exec calls gg_platform_pool on entry with its frame above
 * this address (the stack pool's blocks taken back, m0_stack_pool_run). */
extern uintptr_t sg_pool_sp;
void gg_platform_pool(void);
#endif
#endif
#ifndef GG_CACHE_DYN
#define GG_CACHE_DYN 0
#endif
/* Room above the limit for the frames between sg_exec and the first
 * ENTER() (gg.c); the stack pool keeps a raised limit under it too. */
#define SG_EXEC_HEADROOM 384u

/* Buttons (gg_buttons): active high */
enum { GG_UP = 1, GG_DOWN = 2, GG_LEFT_KEY = 4, GG_RIGHT_KEY = 8, GG_B1 = 16, GG_B2 = 32, GG_START = 64,
       GG_PAUSE = 128 /* Master System */ };

typedef void (*sg_fn)(void);

typedef struct {
    z80_t cpu;               /* registers; cpu.user points back here */
    uint32_t cyc;            /* T-states, wrapping */
    uint32_t next_evt;       /* sync when cyc reaches it (forced to cyc for IRQs) */
    uint32_t frame_start;    /* cycle of line 0 of the current frame */
    uint32_t render_at;      /* cycle at which line render_line ends */
    uint32_t frames;
    uint16_t render_line;
    uint8_t vblank_done, stop;
    /* Cartridge and mapper */
    const uint8_t *rom;
    uint32_t rom_size;
    uint16_t rom_pages;      /* 16 KiB pages (size rounded up) */
    const uint8_t *slot[GG_SLOTS];  /* 16 KiB Sega/Codemasters or six 8 KiB Korean windows;
                              * NULL: a page read from the SD card */
    uint8_t page[GG_SLOTS];  /* their pages; slot 2 reads FF while cartridge RAM is
                              * mapped there (no compiled code matches it) */
    uint8_t page2;           /* ROM page of slot 2 behind the cartridge RAM */
    uint8_t remapped;        /* a page changed since the last event: code may have moved */
    uint8_t ram_ctl;         /* FFFC: bit 3 cartridge RAM at 8000-BFFF */
    uint8_t mapper;          /* M0_MAPPER_*; Korean: ram_ctl = mode, page2 = bank register */
    uint8_t sms;             /* Master System */
    uint8_t io_ctl;          /* Master System port 3F: TH directions and levels */
    uint8_t *cart_ram;       /* GG_CART_RAM_SIZE, or NULL */
    uint8_t ram[8192], vram[16384], cram[64];
    /* VDP */
    uint8_t vdp[16];
    uint16_t vdp_address;
    uint8_t vdp_code, vdp_latch, vdp_pending, vdp_buffer, vdp_status, cram_latch;
    uint8_t line_irq;        /* line counter underflowed (cleared by a status read) */
    uint8_t vscroll;         /* R9 latched at the start of the frame */
    uint8_t lc_count;        /* line counter */
    uint16_t lc_line;        /* lines of this frame already counted */
    uint16_t sat_base;
    uint8_t sprites_dirty;   /* sprite Y, terminator, size or table changed */
    uint16_t pal[32];        /* CRAM as RGB565 (panel order, see GG_PANEL_SWAP) */
    uint8_t buttons;
    uint8_t io[7];           /* Game Gear ports 0-6: link port (no partner), stereo */
    /* SN76489 (Sega variant) */
    uint16_t psg_tone[3];
    uint8_t psg_volume[4], psg_noise, psg_latch;
    uint32_t audio_rate, audio_phase, audio_tick_phase, audio_lfsr, audio_cyc;
    uint32_t audio_div, audio_tick_int, audio_tick_frac;
    int32_t audio_count[4];
    uint16_t audio_head, audio_tail;
    uint8_t audio_output[4], audio_buffer[512];
    /* Output.  draw_line(y), y a VDP line of the window (24-167):
     *   0 not displayed, 1 render into row_out then call row(),
     *   3 render into row_out (the first of two lines that share a row),
     *   2 render into row_tmp, average it into row_out, then call row().
     * Rows are GG_WIDTH pixels with GG_ROW_PAD writable pixels on each side. */
    uint16_t *row_out, *row_tmp;
    void *platform;
    int (*draw_line)(void *, unsigned y);
    void (*row)(void *, unsigned y, uint16_t *pixels);
    void (*frame)(void *);
    /* Execution */
    sg_fn (*lookup)(uint16_t pc); /* generated entry table, or NULL */
    uint32_t interpreted, compiled;
    uint32_t *coverage;           /* host: hits per location (page << 16 | pc), or NULL */
    uint8_t *entries;             /* host: targets of RET / JP (HL) per location, or NULL */
    uint32_t coverage_size;       /* locations in both arrays */
    uint16_t irq_sp[8];
    uint8_t irq_depth;
    uint8_t cart_ram_used;        /* cartridge RAM was enabled */
} gg_t;

extern gg_t machine;
extern uintptr_t sg_stack_limit;

void gg_init(const uint8_t *rom, size_t size, uint8_t *cart_ram);
/* The slots' pointers again from the pages (a save state's load). */
void gg_remap(void);
/* bench watch: log every write to this work-RAM address (GG_WATCH_ADDR=hex, -1 off) */
extern int gg_watch_addr;
/* The save state's stream: the machine as raw bytes, then the cartridge RAM
 * when the game has some (m0_state.cpp's io walker). */
uint32_t gg_state_bytes(void);
void gg_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
void gg_init_system(const uint8_t *rom, size_t size, uint8_t *cart_ram, int sms);
void gg_reset(void);
void gg_buttons(uint8_t pressed);
void gg_run(void);

/* Bus.  Generated projects fix the mapper (GG_MAPPER); the generic host
 * detects it at gg_init. */
#ifdef GG_MAPPER
#define GG_CODEMASTERS (GG_MAPPER == 1)
#define GG_KOREAN150 (GG_MAPPER == 2)
#else
#define GG_CODEMASTERS (machine.mapper == 1)
#define GG_KOREAN150 (machine.mapper == 2)
#endif
#define GG_FIXED_KIB (!GG_CODEMASTERS && !GG_KOREAN150)
#define GG_BANK_SHIFT (GG_KOREAN150 ? 13u : 14u)
#define GG_BANK_MASK ((1u << GG_BANK_SHIFT) - 1u)
void gg_mapper_write(uint16_t a, uint8_t v);
#if GG_STREAM
extern const uint8_t gg_page_index[]; /* flash slot of each page, 0xff: on the card */
#ifdef GG_POOL_SECTORS /* hot sectors of the pages left on the card, kept in flash */
#ifdef GG_POOL_WIDE
extern const uint16_t gg_pool_index[];
#define GG_POOL_NONE 65535
#else
extern const uint8_t gg_pool_index[]; /* slot of each ROM sector in gg_pool, 0xff: on the card */
#define GG_POOL_NONE 255
#endif
extern const uint32_t gg_pool_records[]; /* codec in bits 31:30, blob offset in bits 29:0 */
extern const uint8_t gg_pool[];
#endif
uint8_t gg_stream_read(uint32_t offset);
/* Platform: sector (512 bytes) of the ROM file into dst; 0 on failure. */
int gg_platform_rom_read(uint32_t sector, uint8_t *dst);
uint8_t *gg_stream_scratch(void);    /* 512 bytes free before gg_init */
extern uint32_t gg_stream_misses;
/* The card cache's blocks lent to the option menu (sg_menu.h): the save is
 * written back first, lend returns the lent bytes' top, 8-aligned; until
 * gg_cache_unlend the cache holds nothing.  Streaming and card-RAM builds. */
uintptr_t gg_cache_lend(void);
void gg_cache_unlend(void);
static inline uint8_t gg_slot_read(uint16_t a) {
    const uint8_t *p = machine.slot[a >> GG_BANK_SHIFT];
    if (__builtin_expect(p != 0, 1)) return m0_load_u8(p + (a & GG_BANK_MASK));
    const unsigned s = a >> GG_BANK_SHIFT, page = !GG_KOREAN150 && s == 2 ? machine.page2 : machine.page[s];
    return gg_stream_read(((uint32_t)page << GG_BANK_SHIFT) + (a & GG_BANK_MASK));
}
#else
static inline uint8_t gg_slot_read(uint16_t a) { return m0_load_u8(machine.slot[a >> GG_BANK_SHIFT] + (a & GG_BANK_MASK)); }
#endif
#if GG_CART_ON_CARD
uint8_t gg_cart_read(uint32_t offset);
void gg_cart_store(uint32_t offset, uint8_t v);
int gg_platform_save_read(uint32_t sector, uint8_t *dst);   /* 0: no file (reads FF) */
int gg_platform_save_write(uint32_t sector, const uint8_t *src);
void gg_save_flush(void);
void gg_save_tick(void);             /* once per frame */
#define GG_HAS_CART_RAM 1
static inline uint8_t gg_cart_byte(uint16_t a) { return gg_cart_read(gg_cart_offset(a, machine.ram_ctl)); }
#else
#define GG_HAS_CART_RAM (machine.cart_ram != 0)
static inline uint8_t gg_cart_byte(uint16_t a) { return machine.cart_ram[gg_cart_offset(a, machine.ram_ctl)]; }
#endif
#ifdef GG_ROM_TRACE
/* Host profiling (gg_host --rom-trace-out): every read under C000, from the
 * interpreter or from compiled code (RD, sg_gen.h): the traffic the
 * firmware's ROM path and cartridge RAM see. */
void gg_trace_read(uint16_t a);
#else
#define gg_trace_read(a) ((void)0)
#endif
static inline uint8_t gg_read(uint16_t a) {
    if (a >= 0xc000) return machine.ram[a & 8191];
    gg_trace_read(a);
    if (a < 0x400 && GG_FIXED_KIB) return m0_load_u8(machine.rom + a);
    if (!GG_KOREAN150 && a >= 0x8000 && (machine.ram_ctl & 8) && GG_HAS_CART_RAM) return gg_cart_byte(a);
    return gg_slot_read(a);
}
void gg_write_cart(uint16_t a, uint8_t v);
static inline void gg_write(uint16_t a, uint8_t v) {
    if (a >= 0xc000) {
        if (gg_watch_addr >= 0 && (a & 8191) == (unsigned)gg_watch_addr) {
            extern int gg_watch_dump(unsigned, uint8_t, unsigned, uint32_t, unsigned);
            gg_watch_dump(a, v, machine.cpu.pc, machine.cyc, machine.frames);
        }
        machine.ram[a & 8191] = v;
        if (GG_KOREAN150 ? a == 0xfff3 || a == 0xfffc : a >= 0xfffc && !GG_CODEMASTERS) gg_mapper_write(a, v);
    } else if (GG_CODEMASTERS && !(a & 0x3fff)) gg_mapper_write(a, v);
    else if (!GG_KOREAN150 && a >= 0x8000 && (machine.ram_ctl & 8)) gg_write_cart(a, v);
}
static inline uint32_t gg_location(uint16_t pc) {
    return (pc < 0x400 && GG_FIXED_KIB) || pc >= 0xc000 ? pc : (uint32_t)machine.page[pc >> GG_BANK_SHIFT] << 16 | pc;
}
uint8_t gg_in(uint8_t port);
void gg_out(uint8_t port, uint8_t v);

/* Engine (names shared with the SG-1000 runtime: sg_gen.h) */
void sg_sync(void);
int sg_event(uint16_t resume_pc);    /* sync + interrupt; nonzero: pc or its page moved */
void sg_halt(void);
void sg_exec(void);
void sg_interp_block(void);
int sg_step(uint16_t after);
void gg_irq_update(void);

/* VDP */
void gg_vdp_catchup(void);           /* lines that ended before cyc */
void gg_vdp_data_write(uint8_t v);
uint8_t gg_vdp_data_read(void);
void gg_vdp_control_write(uint8_t v);
uint8_t gg_vdp_status_read(void);
uint8_t gg_vcounter(void);
void gg_vdp_frame_start(void);       /* new frame: scroll latch, line counter */
#ifdef GG_ROM_TRACE
void gg_vdp_restore(void);            /* host explorer: after the machine is copied back */
extern uint32_t gg_new_locations;     /* locations executed for the first time */
#endif
uint32_t gg_vdp_next_event(void);    /* next line interrupt, or 0 */
void gg_line_counter_catchup(void);
void gg_render_line(unsigned y, uint16_t *out); /* VDP line y (window) -> GG_WIDTH pixels */
/* Master System: 256 colour indices (0-31) of line y, the reference picture */
void gg_render_indices(unsigned y, uint8_t *pixels);
unsigned gg_render_selftest(void);

/* Audio: unsigned 8-bit samples at rate */
void gg_audio_rate(unsigned rate);
void gg_audio_catchup(void);
void gg_audio_render(uint8_t *buffer, unsigned n);
static inline unsigned gg_audio_available(void) {
    return (unsigned)(machine.audio_head - machine.audio_tail) & 511u;
}
void gg_psg_write(uint8_t value);

#ifdef __cplusplus
}
#endif
#endif
