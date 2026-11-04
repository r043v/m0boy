/*
 * Build options for the gbl runtime.  The value of each optimisation per
 * console is in gbl_profile.h; override any of them with -D.
 *
 * GBL_CGB                Game Boy Color: VRAM and WRAM banks, VBK/SVBK, a CGB cartridge
 *                         ($143 bit 7) boots in colour mode, a DMG one is untouched.
 *                         0 (default) leaves the DMG code exactly as without.
 * GBL_PPU_BATCH           Scanlines per PPU event when no per-line STAT
 *                         interrupt (mode 0 / mode 2) is enabled; 1 restores
 *                         one event per line.  Batching needs GBL_VRAM_HOOK so
 *                         VRAM/OAM writes first render the lines they follow.
 * GBL_FRAME_SIGNATURE     Track a per-frame signature of everything that
 *                         affects the picture (PPU register timeline and a
 *                         VRAM/OAM change counter), for static-frame skipping
 *                         in the frontend.
 * GBL_LINE_SIGNATURE      Before rendering each scanline, ask the frontend
 *                         gbl_platform_line_check(ly, sig) with a signature
 *                         of everything the line depends on (registers, the
 *                         map row and tile versions, its sprites); returning
 *                         0 skips rendering it.  Equal signatures mean equal
 *                         pixels, so skipping unchanged lines is exact.
 * GBL_PPU_PAL32           Convert background pixels four at a time with a
 *                         2 KiB table (gm0's pal32) instead of two at a time.
 * GBL_ACCESS_BLOCK        CPU access blocking by PPU mode, as on the hardware:
 *                         VRAM reads $FF / writes dropped in mode 3, OAM in
 *                         modes 2-3 (SameBoy and Gambatte agree).  Costs the
 *                         direct VRAM page; off by default (firmware speed),
 *                         on for accuracy hosts (deepeel).
 * GBL_VRAM_HOOK           Route VRAM writes through gbl_vram_wr().  Forced on
 *                         by the two options above.
 * GBL_GEN_OPTIMIZE_SIZE   Compile generated code with -Os (it is a small part
 *                         of the run time and the largest part of the flash).
 * GBL_SPIN                Fast-forward wait loops recognised by the generator
 *                         (gbl_spin); 0 runs every iteration.  The results are
 *                         identical either way, only the time spent differs.
 * GBL_INTERP_DELAY_LOOP   Interpreter shortcut for DEC A / JR NZ,-3 (the OAM
 *                         DMA wait most games copy to HRAM).
 * GBL_PPU_ASM             Tile fetch and colour conversion in hand-written
 *                         Thumb-1 (gbl_ppu_asm.S).  Default on for ARMv6-M
 *                         (Cortex-M0+); the C loops are used elsewhere.
 * GBL_RENDER_LINE_ASM     render_line (sprite selection, line signature and
 *                         the calls to the frontend and draw_line) in
 *                         Thumb-1 (gbl_ppu_asm.S); follows GBL_PPU_ASM and
 *                         needs GBL_LINE_SIGNATURE.
 * GBL_PPU_RAMFUNC         Run those loops from SRAM (about 150 bytes), out of
 *                         reach of flash wait states.
 * GBL_CORE_ASM            gbl_push / gbl_pop in Thumb-1 (gbl_core_asm.S) on
 *                         Cortex-M0+.
 * GBL_TIMING_ASM          gbl_ppu_advance (the PPU line state machine),
 *                         gbl_update_evt and gbl_sync in Thumb-1
 *                         (gbl_timing_asm.S); default on for Cortex-M0+.
 * GBL_INTERP_ASM          Interpreter loop and common opcodes in Thumb-1
 *                         (gbl_interp_asm.S), the others through the C
 *                         gbl_interp_op; default on for Cortex-M0+ (not in
 *                         coverage or trace builds).
 * GBL_BATTERY             Cartridge RAM is saved (set by the generator for
 *                         battery-backed cartridges).
 * GBL_ERAM_ON_SD          Cartridge RAM lives in the platform's save file
 *                         behind a 1 KiB cache instead of RAM; default for
 *                         cartridge RAM larger than 2 KiB.
 * GBL_ROM_PACKED          The streamed part of the ROM comes from LZSS-
 *                         compressed flash instead of the SD card (set by the
 *                         generator: gbrecomp --meta-pack).
 * GBL_UNPACK_ASM          Decode those sectors with the Thumb-1 decoder
 *                         (gbl_core_asm.S); default on for Cortex-M0+.
 * GBL_STREAM_SECTORS      512-byte sectors cached for a streamed ROM
 *                         (GBL_ROM_STREAM projects only); with
 *                         GBL_CACHE_DYN, how many the cache starts with.
 * GBL_CACHE_DYN           The ROM cache (or the unified pool) is a fixed
 *                         part, which the option menu runs on, and blocks
 *                         after it in the C stack's RAM: the frontend lends
 *                         those to the stack when it runs short and takes
 *                         them back when it stays shallow (meta_main.cpp,
 *                         m0_stack_pool.h).  The fixed part must be the
 *                         last thing in RAM below the stack (.cachepool,
 *                         gamebuino_meta.ld, pokitto.ld); the device
 *                         projects turn it on, off elsewhere.
 * GBL_STREAM_FIXED        GBL_CACHE_DYN: sectors of the fixed part (2, the
 *                         menu's stack); GBL_STREAM_MAX the most in all.
 * GBL_CACHE_FIXED_BYTES   GBL_CACHE_DYN with GBL_CACHE_UNIFIED: the fixed
 *                         part of the pool (the menu's stack, a sector and
 *                         3 blocks); GBL_CACHE_MAX_BYTES the most in all.
 * GBL_DEBUG_ERAM_MAPS     Check every read through the cartridge RAM read
 *                         table against the sector search (gbl_dbg_eram,
 *                         meta-usb.py eram); off by default.
 * GBL_ERAM_TABLES         gbl_rd / gbl_wr reach cached cartridge RAM sectors
 *                         through gbl_eram_rmap / gbl_eram_wmap; 0 sends
 *                         every access through eram_sector (comparisons).
 * GBL_CACHE_UNIFIED       One pool of blocks for the streamed ROM and the
 *                         cartridge RAM on the card (both needed), instead
 *                         of GBL_STREAM_SECTORS + GBL_ERAM_CACHE_SECTORS
 *                         sectors of each; off by default.
 * GBL_CACHE_BLOCK         Its block size: 64, 128, 256 or 512 bytes.  Below
 *                         512, the card and the unpacker still work on
 *                         512-byte sectors, through one more sector buffer
 *                         that also serves the next block of the same
 *                         sector; a dirty block is written back with the
 *                         rest of its sector read first.
 * GBL_CACHE_BYTES         The pool's size (the sector buffer apart).
 * GBL_CACHE_MENU_STACK    What the option menu needs of the pool, which it
 *                         runs on (all but 3 blocks, written back and kept
 *                         out meanwhile): its stack, and a state's sector
 *                         buffer below it.
 * GBL_DEBUG_ERAM_SUM      Checksum the cartridge RAM cache's sectors across
 *                         their SD round trips (gbl_dbg_eram_sum); off by
 *                         default.
 * GBL_DEBUG_RESTART       Keep the buttons of the frames before each write
 *                         to the RAM enable register (gbl_dbg_restart,
 *                         meta-usb.py restart); off by default.
 * GBL_DEBUG_STACK         Shadow the GBL_DBG_STACK_LEN bytes of cartridge
 *                         RAM below A900 (Super Mario Land 2's stack): the last writer of each byte,
 *                         and every pop checked against the shadow
 *                         (gbl_dbg_stack, meta-usb.py stack); off by default.
 */
#ifndef GBL_OPTIONS_H
#define GBL_OPTIONS_H

/* The console's choices for every optimisation switch above. */
#include "gbl_profile.h"

/* Every switch must come from the profile (a missing one would read as 0
 * in #if and silently turn an optimisation off). */
#if !defined(GBL_PPU_BATCH) || !defined(GBL_FRAME_SIGNATURE) || !defined(GBL_LINE_SIGNATURE) ||   \
    !defined(GBL_PPU_PAL32) || !defined(GBL_GEN_OPTIMIZE_SIZE) || !defined(GBL_SPIN) ||           \
    !defined(GBL_INTERP_DELAY_LOOP) || !defined(GBL_LOOKUP_CACHE) || !defined(GBL_STREAM_SECTORS) || \
    !defined(GBL_PPU_ASM) || !defined(GBL_RENDER_LINE_ASM) || !defined(GBL_PPU_RAMFUNC) ||        \
    !defined(GBL_CORE_ASM) || !defined(GBL_TIMING_ASM) || !defined(GBL_INTERP_ASM) ||             \
    !defined(GBL_UNPACK_ASM)
#error "gbl_profile.h: a runtime switch is missing from this console's profile"
#endif

/* ---- the cartridge (gbl_config.h) --------------------------------------- */
#ifndef GBL_BATTERY
#define GBL_BATTERY 0
#endif
#ifndef GBL_ROM_STREAM
#define GBL_ROM_STREAM 0
#endif
#ifndef GBL_ROM_PACKED
#define GBL_ROM_PACKED 0
#endif
#ifndef GBL_ERAM_TABLES
#define GBL_ERAM_TABLES 1
#endif
#ifndef GBL_DEBUG_ERAM_SUM
#define GBL_DEBUG_ERAM_SUM 0
#endif
#ifndef GBL_DEBUG_RESTART
#define GBL_DEBUG_RESTART 0
#endif
#ifndef GBL_DEBUG_ERAM_MAPS
#define GBL_DEBUG_ERAM_MAPS 0
#endif
#ifndef GBL_DEBUG_STACK
#define GBL_DEBUG_STACK 0
#endif
#ifndef GBL_ROM_SD
#define GBL_ROM_SD (GBL_ROM_STREAM && !GBL_ROM_PACKED)
#endif
#ifndef GBL_CACHE_UNIFIED
#define GBL_CACHE_UNIFIED 0
#endif
#if GBL_CACHE_UNIFIED
#if !GBL_ROM_STREAM || !GBL_ERAM_ON_SD || GBL_ERAM_SIZE == 0
#error "GBL_CACHE_UNIFIED: needs a streamed ROM and cartridge RAM on the card"
#endif
#if GBL_DEBUG_ERAM_MAPS || GBL_DEBUG_ERAM_SUM
#error "GBL_DEBUG_ERAM_MAPS / GBL_DEBUG_ERAM_SUM: not with GBL_CACHE_UNIFIED"
#endif
#ifndef GBL_CACHE_BLOCK
#define GBL_CACHE_BLOCK 512
#endif
#ifndef GBL_CACHE_BYTES /* the two caches' RAM, less the tables and sector buffer below 512 */
#define GBL_CACHE_BYTES ((GBL_STREAM_SECTORS + GBL_ERAM_CACHE_SECTORS) * 512 - (GBL_CACHE_BLOCK < 512 ? 1024 : 0))
#endif
#ifndef GBL_CACHE_MENU_STACK
#define GBL_CACHE_MENU_STACK 768
#endif
#else
#undef GBL_CACHE_BLOCK
#define GBL_CACHE_BLOCK 512 /* the separate caches' sectors */
#endif
#if GBL_CACHE_BLOCK == 64
#define GBL_CACHE_SHIFT 6
#elif GBL_CACHE_BLOCK == 128
#define GBL_CACHE_SHIFT 7
#elif GBL_CACHE_BLOCK == 256
#define GBL_CACHE_SHIFT 8
#elif GBL_CACHE_BLOCK == 512
#define GBL_CACHE_SHIFT 9
#elif GBL_CACHE_BLOCK == 32 /* m0_cache.h takes 16 to 512 (studies, host-tested; cartridge RAM tables of 8 KiB / block) */
#define GBL_CACHE_SHIFT 5
#elif GBL_CACHE_BLOCK == 16
#define GBL_CACHE_SHIFT 4
#else
#error "GBL_CACHE_BLOCK: 16, 32, 64, 128, 256 or 512"
#endif

/* Stack gbl_exec / gbl_run want above gbl_stack_limit (gbl_core.c); the
 * frontend's stack pool keeps the raised limit under it too. */
#ifndef GBL_EXEC_HEADROOM
#define GBL_EXEC_HEADROOM 512u
#endif

/* ---- the cache's blocks after its fixed part (GBL_CACHE_DYN) ------------ */
#ifndef GBL_CACHE_DYN
#define GBL_CACHE_DYN 0
#endif
#ifndef GBL_CACHE_DYN_INLINE /* host tests: every block in the array */
#define GBL_CACHE_DYN_INLINE 0
#endif
/* Device builds only: the host has no such layout, and the estimator
 * (BENCH_FRAMES) keeps the whole cache in its array, as before. */
#if !GBL_ROM_STREAM || (!defined(__ARM_ARCH_6M__) && !GBL_CACHE_DYN_INLINE) || defined(BENCH_FRAMES)
#undef GBL_CACHE_DYN
#define GBL_CACHE_DYN 0
#endif
#if GBL_CACHE_UNIFIED
#if GBL_CACHE_DYN
#ifndef GBL_CACHE_FIXED_BYTES
#define GBL_CACHE_FIXED_MIN \
    ((GBL_CACHE_MENU_STACK + 512 + 3 * GBL_CACHE_BLOCK + GBL_CACHE_BLOCK - 1) / GBL_CACHE_BLOCK * GBL_CACHE_BLOCK)
#define GBL_CACHE_FIXED_BYTES (GBL_CACHE_FIXED_MIN < GBL_CACHE_BYTES ? GBL_CACHE_FIXED_MIN : GBL_CACHE_BYTES)
#endif
#ifndef GBL_CACHE_MAX_BYTES
#define GBL_CACHE_MAX_BYTES \
    (GBL_CACHE_BYTES + 2048 > 255 * GBL_CACHE_BLOCK ? 255 * GBL_CACHE_BLOCK : GBL_CACHE_BYTES + 2048)
#endif
#else
#undef GBL_CACHE_FIXED_BYTES
#undef GBL_CACHE_MAX_BYTES
#define GBL_CACHE_FIXED_BYTES GBL_CACHE_BYTES
#define GBL_CACHE_MAX_BYTES GBL_CACHE_BYTES
#endif
#define GBL_CACHE_DYN_BLOCK GBL_CACHE_BLOCK
#define GBL_CACHE_DYN_TARGET (GBL_CACHE_BYTES / GBL_CACHE_BLOCK) /* blocks at boot */
#if GBL_CACHE_FIXED_BYTES % GBL_CACHE_BLOCK || GBL_CACHE_FIXED_BYTES / GBL_CACHE_BLOCK < 4 || \
    GBL_CACHE_MAX_BYTES / GBL_CACHE_BLOCK > 255 || GBL_CACHE_MAX_BYTES < GBL_CACHE_BYTES ||   \
    GBL_CACHE_BYTES < GBL_CACHE_FIXED_BYTES ||                                               \
    GBL_CACHE_MENU_STACK + 512 + 3 * GBL_CACHE_BLOCK > GBL_CACHE_FIXED_BYTES
#error "GBL_CACHE_BYTES: 4 to 255 blocks, room for the menu's stack and a sector"
#endif
#else
#if GBL_CACHE_DYN
#ifndef GBL_STREAM_FIXED
#define GBL_STREAM_FIXED (GBL_STREAM_SECTORS < 2 ? GBL_STREAM_SECTORS : 2)
#endif
#ifndef GBL_STREAM_MAX
#define GBL_STREAM_MAX (GBL_STREAM_SECTORS + 4)
#endif
#else
#undef GBL_STREAM_FIXED
#undef GBL_STREAM_MAX
#define GBL_STREAM_FIXED GBL_STREAM_SECTORS
#define GBL_STREAM_MAX GBL_STREAM_SECTORS
#endif
#define GBL_CACHE_DYN_BLOCK 512
#define GBL_CACHE_DYN_TARGET GBL_STREAM_SECTORS
#if GBL_ROM_STREAM && (GBL_STREAM_FIXED < 1 || GBL_STREAM_SECTORS < GBL_STREAM_FIXED || \
                       GBL_STREAM_MAX < GBL_STREAM_SECTORS || GBL_STREAM_MAX > 255)
#error "GBL_STREAM_FIXED <= GBL_STREAM_SECTORS <= GBL_STREAM_MAX <= 255"
#endif
#endif

/* ---- what the build can do: Thumb-1 paths need an ARMv6-M compiler (not
 * the host harness), the assembly interpreter has no coverage or trace
 * hooks, the assembly render_line needs the assembly PPU and line
 * signatures. ---------------------------------------------------------------- */
#ifndef GBL_CGB
#define GBL_CGB 0 /* Game Boy Color support: optional, off in every DMG firmware */
#endif
#if !defined(__ARM_ARCH_6M__) || GBL_CGB
#undef GBL_CORE_ASM
#define GBL_CORE_ASM 0
#undef GBL_PPU_ASM
#define GBL_PPU_ASM 0
#undef GBL_TIMING_ASM
#define GBL_TIMING_ASM 0
#undef GBL_INTERP_ASM
#define GBL_INTERP_ASM 0
#undef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 0
#endif
#if GBL_INTERP_ASM && (defined(GBL_COVERAGE) || defined(GBL_TRACE) || defined(GBL_ROM_TRACE))
#undef GBL_INTERP_ASM
#define GBL_INTERP_ASM 0
#endif
#if GBL_RENDER_LINE_ASM && !(GBL_PPU_ASM && GBL_LINE_SIGNATURE)
#undef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 0
#endif
#if GBL_PPU_BATCH > 1 || GBL_FRAME_SIGNATURE || GBL_LINE_SIGNATURE
#undef GBL_VRAM_HOOK
#define GBL_VRAM_HOOK 1
#endif
#if GBL_CGB
#undef GBL_VRAM_HOOK /* VBK: a VRAM write goes through gbl_vram_wr, which knows the bank */
#define GBL_VRAM_HOOK 1
#endif
#ifndef GBL_VRAM_HOOK
#define GBL_VRAM_HOOK 0
#endif
#ifndef GBL_ACCESS_BLOCK
#define GBL_ACCESS_BLOCK 0
#endif
#if GBL_LOOKUP_CACHE & (GBL_LOOKUP_CACHE - 1) || GBL_LOOKUP_CACHE > 256
#error "GBL_LOOKUP_CACHE must be 0 or a power of two up to 256"
#endif
#if GBL_PPU_BATCH < 1 || (GBL_PPU_BATCH & (GBL_PPU_BATCH - 1))
#error "GBL_PPU_BATCH must be a power of two"
#endif

/* Host/reference conformance mode (never set on a device build): GBL_HOST_REF=1
 * turns on the sub-instruction models the test ROMs observe but games do not
 * need, each also switchable alone.
 *   GBL_OAM_DMA_BUS   OAM DMA as a timed transfer with bus conflicts (gbl_core.c)
 *   GBL_STACK_TIMING  PUSH/CALL/RST stack writes in their own M-cycles (gbl_cpu.c)
 *   GBL_PPU_REF       DMG PPU details of the C line state machine (gbl_ppu.c)
 *   GBL_TIMER_EDGE    DIV/TAC writes clock TIMA on the falling edge (gbl_core.c) */
#ifndef GBL_HOST_REF
#define GBL_HOST_REF 0
#endif
#ifndef GBL_OAM_DMA_BUS
#define GBL_OAM_DMA_BUS GBL_HOST_REF
#endif
#ifndef GBL_STACK_TIMING
#define GBL_STACK_TIMING GBL_HOST_REF
#endif
#ifndef GBL_PPU_REF
#define GBL_PPU_REF (GBL_HOST_REF && !GBL_TIMING_ASM)
#endif
#ifndef GBL_TIMER_EDGE
#define GBL_TIMER_EDGE (GBL_HOST_REF && !GBL_TIMING_ASM)
#endif

#endif
