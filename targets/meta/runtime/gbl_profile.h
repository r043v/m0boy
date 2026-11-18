/*
 * Optimisation profiles: the one list to edit to turn an optimisation on or
 * off for a console.
 *
 * Every optimisation is a switch of its own (1 on, 0 off, or a size) and
 * can be flipped independently; a definition on the command line wins
 * (PlatformIO build_flags, the Pokitto Makefile's EXTRA_CFLAGS,
 * m0_estimate.py --cflag).  The profile is chosen by GBL_TARGET_POKITTO,
 * which m0recomp writes into gbl_config.h for --target pokitto; anything
 * else (the META, the host harness, the tests) gets the META profile.
 *
 * What each switch does is documented where it is defined or used:
 *   GBL_*      runtime/gbl_options.h (the runtime, shared)
 *   META_*     platform/src (shared frontend), platform/meta (META drivers)
 *   POKITTO_*  platform/pokitto (Pokitto drivers, pokitto.ld)
 * gbl_options.h turns the Thumb-1 switches off on other compilers (the host
 * harness), whatever the profile says, and resolves their dependencies.
 *
 * Included by gbl_options.h (runtime), meta_target.h (frontend), the
 * assembly files and, through the C preprocessor, the Pokitto linker
 * script: definitions only.
 *
 * The META's board settings (SPI rates, display pipeline) are in
 * gbl_board.h, included below: the SG-1000 and Game Gear profiles carry the
 * same ones.
 */
#ifndef GBL_PROFILE_H
#define GBL_PROFILE_H

#include "gbl_config.h"
#if defined(GBL_TARGET_ESPBOY) && GBL_TARGET_ESPBOY
/* Portable C profile; no SAMD peripherals or SD-backed cartridge RAM. */
#define META_USB 0
#define META_LCD_DMA 0
#define META_LCD_12BPP 0
#define META_OVERCLOCK 0
#define GBL_ERAM_ON_SD 0
#define GBL_PPU_RAMFUNC 0
#define META_SMOOTH_ASM 0
#define META_SMOOTH_SPECULATE 1
#define META_MERGE_MODES 0
#define META_TBLEND 2
#define META_LCD_BUFFERS 2
#define META_STRIP_LINES 2
#define META_DISPLAY_COMPACT 1
#endif
#include "gbl_board.h"

/* META_OVERCLOCK: start overclocked instead of at the nominal 48 MHz: 70
 * MHz on the META (the FDPLL96M, two flash wait states, no code in SRAM:
 * meta_clock.cpp; measured stable on one console), 72 on the Pokitto.  Both
 * are past the chips' ratings (SAMD21 48 MHz, LPC11U68 50).  Default: on
 * for the META, off for the Pokitto (POKITTO_HC, its old switch, turns it
 * on); compile.sh passes its OVERCLOCK choice (1 by default) to both.  The
 * menu's "Processeur" entry still switches, and the loader is always
 * entered at 48 MHz. */
#ifndef META_OVERCLOCK
#if defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO
#if defined(POKITTO_HC) && POKITTO_HC
#define META_OVERCLOCK 1
#else
#define META_OVERCLOCK 0
#endif
#else
#define META_OVERCLOCK 1
#endif
#endif

/* ---- what the cartridge needs (gbl_config.h, written per game) -----------
 * A profile can choose differently for each kind of build; every game gets
 * its own firmware, so the right variant is picked at compile time, with no
 * second build to make:
 *   GBL_PROFILE_SD_ROM     ROM read from the SD card (sector cache, card
 *                          reads on cache misses)
 *   GBL_PROFILE_PACKED_ROM ROM LZSS-packed in flash (sector cache, unpacking
 *                          on cache misses)
 *   GBL_PROFILE_ERAM_ON_SD cartridge RAM in the save file (1 KiB cache)
 * GBL_ERAM_ON_SD itself is a cartridge choice: RAM above 2 KiB goes to the
 * card (override with -D). */
#ifndef GBL_ERAM_ON_SD
#define GBL_ERAM_ON_SD (GBL_ERAM_SIZE > 2048)
#endif
#ifndef GBL_ROM_SD
#define GBL_ROM_SD (GBL_ROM_STREAM && !GBL_ROM_PACKED)
#endif
#if GBL_ROM_SD
#define GBL_PROFILE_SD_ROM 1
#else
#define GBL_PROFILE_SD_ROM 0
#endif
#if defined(GBL_ROM_PACKED) && GBL_ROM_PACKED
#define GBL_PROFILE_PACKED_ROM 1
#else
#define GBL_PROFILE_PACKED_ROM 0
#endif
#define GBL_PROFILE_ERAM_ON_SD (GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD)

#if defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO
/* ======================================================= Pokitto (LPC11U68) */

/* Pokitto, 72 MHz: see META_OVERCLOCK below.  The LPC11U68 is specified up
 * to 50 MHz ; 72 MHz (PLL M = 6, FCCO 288 MHz) is the console's standard
 * overclock (Pokitto wiki « _OSCT=2 », Pokitto Magazine vol. 1) and runs in
 * gm0 too.  At that speed the SSP redivides (SD card at 18 MHz, under the
 * 25 MHz Default Speed limit of the SPI) and the flash timings stay at their
 * reset value (3 clocks, the documented maximum). */

/* Its two 2 KiB RAM banks hold the line signatures, the audio queue, the
 * PPU's BGP table and the dispatch cache (pokitto.ld); the main RAM this
 * frees pays for the larger stream cache.  0 keeps everything in main RAM
 * (and the META's cache sizes). */
#ifndef POKITTO_RAM_BANKS
#define POKITTO_RAM_BANKS 1
#endif

/* runtime */
#ifndef GBL_PPU_BATCH
#define GBL_PPU_BATCH 8
#endif
#ifndef GBL_FRAME_SIGNATURE
#define GBL_FRAME_SIGNATURE 1
#endif
#ifndef GBL_LINE_SIGNATURE
#define GBL_LINE_SIGNATURE 1
#endif
#ifndef GBL_PPU_PAL32
#define GBL_PPU_PAL32 0
#endif
#ifndef GBL_GEN_OPTIMIZE_SIZE
#define GBL_GEN_OPTIMIZE_SIZE 1
#endif
#ifndef GBL_SPIN
#define GBL_SPIN 1
#endif
#ifndef GBL_INTERP_DELAY_LOOP
#define GBL_INTERP_DELAY_LOOP 1
#endif
#ifndef GBL_LOOKUP_CACHE /* entries, a power of two; 256 = 2 KiB (USB RAM bank) */
#if POKITTO_RAM_BANKS
#define GBL_LOOKUP_CACHE 256
#else
#define GBL_LOOKUP_CACHE 64
#endif
#endif
#ifndef GBL_STREAM_SECTORS /* 512-byte ROM sectors cached (streamed or packed ROMs) */
#if POKITTO_RAM_BANKS
#define GBL_STREAM_SECTORS 8
#else
#define GBL_STREAM_SECTORS 4
#endif
#endif
/* The ROM cache's sectors past 2 in the C stack's RAM: lent to the stack
 * when it runs short, taken back when it stays shallow (gbl_options.h,
 * m0_stack_pool.h).  Not measured on the device yet. */
#ifndef GBL_CACHE_DYN
#define GBL_CACHE_DYN 1
#endif
#ifndef GBL_PPU_ASM
#define GBL_PPU_ASM 1
#endif
#ifndef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 1
#endif
#ifndef GBL_PPU_RAMFUNC
#define GBL_PPU_RAMFUNC 1
#endif
#ifndef GBL_CORE_ASM
#define GBL_CORE_ASM 1
#endif
#ifndef GBL_TIMING_ASM
#define GBL_TIMING_ASM 1
#endif
#ifndef GBL_INTERP_ASM
#define GBL_INTERP_ASM 1
#endif
#ifndef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 1
#endif

/* frontend */
#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif
#ifndef META_STATIC_SKIP
#define META_STATIC_SKIP 1
#endif
#ifndef META_MAX_SKIP
#define META_MAX_SKIP 3
#endif
#ifndef POKITTO_LCD_ASM
#define POKITTO_LCD_ASM 1
#endif
#ifndef POKITTO_AUDIO_ASM
#define POKITTO_AUDIO_ASM 1
#endif
/* C stack kept below gbl_stack_limit (meta_main.cpp); the META's value
 * comes from its own frames, not measured here. */
#ifndef META_STACK_RESERVE
#define META_STACK_RESERVE 768
#endif
/* Longest wait of a cartridge RAM change for the card, in frames
 * (meta_save.cpp); 0, the default, leaves it out. */
#ifndef META_SAVE_PERIOD
#define META_SAVE_PERIOD 0
#endif

#else
/* ============================================== Gamebuino META (SAMD21) */

/* runtime */
#ifndef GBL_PPU_BATCH
#define GBL_PPU_BATCH 8
#endif
#ifndef GBL_FRAME_SIGNATURE
#define GBL_FRAME_SIGNATURE 1
#endif
#ifndef GBL_LINE_SIGNATURE
#define GBL_LINE_SIGNATURE 1
#endif
#ifndef GBL_PPU_PAL32
#define GBL_PPU_PAL32 0
#endif
#ifndef GBL_GEN_OPTIMIZE_SIZE
#define GBL_GEN_OPTIMIZE_SIZE 1
#endif
#ifndef GBL_SPIN
#define GBL_SPIN 1
#endif
#ifndef GBL_INTERP_DELAY_LOOP
#define GBL_INTERP_DELAY_LOOP 1
#endif
#ifndef GBL_LOOKUP_CACHE
#define GBL_LOOKUP_CACHE 64
#endif
/* 8 (4 KiB): the RAM the Arduino core used (about 1.9 KiB) pays for it;
 * streamed builds keep about 1.8 KiB of stack, 200 bytes less than with 4
 * sectors and the core.  4 when 2 KiB of cartridge RAM are in the META's
 * RAM instead of on the card (MBC2's 512 bytes leave room for 8).  More sectors read the card much less
 * (Donkey Kong Land: 764 reads with 4 over 3,000 frames, 400 with 6, 217
 * with 8; Link's Awakening: frames with 8 reads or more 26 -> 1 with 6),
 * not measured on the device.
 * With the cartridge RAM on the card too, its write-back cache
 * (GBL_ERAM_CACHE_SECTORS) gets 6 of those 8: a game that uses that RAM as
 * work RAM cycles through several sectors, and each eviction of a dirty one
 * is a synchronous card write, when a ROM miss is a read (or an unpack from
 * flash).  Measured on device (2026-10-17, Super Mario Land 2, about seven
 * sectors, CPU at 70 MHz): ROM 8 + RAM 2: 157 writes a second, half the
 * CPU in them; 4 + 6: 47 writes a second, 24 frames a second; 2 + 8: 4 a
 * second (the 2 s flush), 42 frames a second. */
/* Audio by the TC4 interrupt by default; since 0.10.28 one ring
 * (META_AUDIO_RING, meta_audio.cpp) feeds both paths and the menus' Audio
 * line switches at run time.  GBL_AUDIO_SPARE hands a sector of the ROM
 * cache to the streaming budget that the ring's size leaves.  Device,
 * 2026-10-18: the DMA path's chained descriptors met SAM D21 erratum 1.7.2
 * (meta_dma.cpp; now chained from its interrupt), and Super Mario Land ran
 * as fast either way (180.7 / 180.9 frames a second, fast mode). */
#ifndef META_AUDIO_DMA
#define META_AUDIO_DMA 0
#endif
#define GBL_AUDIO_SPARE (META_AUDIO_DMA ? 0 : 512)
#ifndef GBL_STREAM_SECTORS
#if GBL_ERAM_SIZE > 512 && !GBL_ERAM_ON_SD
#define GBL_STREAM_SECTORS (4 + GBL_AUDIO_SPARE / 512)
#elif GBL_ERAM_ON_SD && GBL_ROM_SD
#define GBL_STREAM_SECTORS 2 /* the shared pool takes the spare (below) */
#else
#define GBL_STREAM_SECTORS (8 + GBL_AUDIO_SPARE / 512)
#endif
#endif
#ifndef GBL_ERAM_CACHE_SECTORS
#if GBL_ERAM_ON_SD && GBL_ROM_SD
#define GBL_ERAM_CACHE_SECTORS 8
#else
#define GBL_ERAM_CACHE_SECTORS 2
#endif
#endif
/* GBL_STREAM_SECTORS (or the unified pool's GBL_CACHE_BYTES) is what the
 * cache starts with: past its fixed part (2 sectors, or the menu's stack
 * and 3 blocks in the pool), the blocks sit in the C stack's RAM, lent to
 * the stack when it runs short and taken back when it stays shallow
 * (gbl_options.h, m0_stack_pool.h).  Not measured on the device yet. */
#ifndef GBL_CACHE_DYN
#define GBL_CACHE_DYN 1
#endif
/* A streamed ROM and cartridge RAM on the card share one pool of 128-byte
 * blocks (gbl_options.h), 1 KiB smaller than the two caches it replaces:
 * its tables and sector buffer take about that, and the C stack keeps its
 * room (taking 1 KiB from it cost 5 frames a second, more unwinds).
 * Measured on device (2026-10-18, Super Mario Land 2's demo to frame
 * 6000, ROM 3 + RAM 10 sectors before): 3963 card reads, 1567 writes,
 * 14200 sectors unpacked; 128-byte blocks: 226, 69, 96, the same speed
 * (the demo runs at 60 frames a second). */
#ifndef GBL_CACHE_UNIFIED
#if defined(GBL_ROM_STREAM) && GBL_ROM_STREAM && GBL_ERAM_ON_SD && GBL_ERAM_SIZE > 0 && \
    GBL_STREAM_SECTORS + GBL_ERAM_CACHE_SECTORS >= 6 /* room for the menu's stack */
#define GBL_CACHE_UNIFIED 1
#else
#define GBL_CACHE_UNIFIED 0
#endif
#endif
/* 64-byte blocks: Super Mario Land 2's demo from a fresh save, host, at
 * the same RAM (tags and cartridge RAM tables counted): 1113 save sectors
 * read, 239 written, 1046 ROM sectors decoded, against 10062, 2006 and
 * 5136 with 128-byte blocks; device (2026-10-21, 150 s, fast mode): 823
 * ROM blocks loaded against 3036, frames emulated 13646 against 11758. */
#ifndef GBL_CACHE_BLOCK
#define GBL_CACHE_BLOCK 64
#endif
#if GBL_CACHE_UNIFIED && !defined(GBL_CACHE_BYTES)
#define GBL_CACHE_BYTES \
    ((GBL_STREAM_SECTORS + GBL_ERAM_CACHE_SECTORS) * 512 - \
     (GBL_CACHE_BLOCK == 64 ? 1728 /* the tables and tags of twice the blocks */ : GBL_CACHE_BLOCK < 512 ? 1024 : 0) + \
     GBL_AUDIO_SPARE)
#endif
#ifndef GBL_PPU_ASM
#define GBL_PPU_ASM 1
#endif
#ifndef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 1
#endif
/* GBL_PPU_RAMFUNC 0 on the META: code run from SRAM faulted with the CPU
 * overclocked (device, 2026-10-17: HardFaults in these loops at 70 and 72
 * MHz, none in minutes once they ran from flash).  From SRAM they took 3.9%
 * of the CPU at 48 MHz, from flash 4.8% (USB profiler, title screen). */
#ifndef GBL_PPU_RAMFUNC
#define GBL_PPU_RAMFUNC 0
#endif
#ifndef GBL_CORE_ASM
#define GBL_CORE_ASM 1
#endif
#ifndef GBL_TIMING_ASM
#define GBL_TIMING_ASM 1
#endif
#ifndef GBL_INTERP_ASM
#define GBL_INTERP_ASM 1
#endif
#ifndef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 1
#endif

/* frontend */
#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif
#ifndef META_STATIC_SKIP
#define META_STATIC_SKIP 1
#endif
#ifndef META_MAX_SKIP
#define META_MAX_SKIP 3
#endif
/* META_LCD_DMA, META_LCD_12BPP, META_LCD_SPI_MHZ, META_SD_SPI_HZ and
 * META_BUTTONS_SPI_HZ are the board settings the SG-1000/Game Gear profile
 * needs too: gbl_board.h, included above. */
/* C stack kept below gbl_stack_limit (meta_main.cpp): generated code checks
 * the limit on entry only, and what it calls below it is unguarded.  Static
 * worst case from the frames and call graph of the Super Mario Land 2
 * firmware (2026-10-17): a slow-path read that renders a line, about 390
 * bytes with the region's frame, plus the USB and DMA interrupts nested,
 * about 220.  The option menu (about 590 more) runs on the ROM cache's
 * buffers in streamed builds (meta_main.cpp). */
#ifndef META_STACK_RESERVE
#define META_STACK_RESERVE 640
#endif
/* Longest wait of a cartridge RAM change for the card, in frames
 * (meta_save.cpp; 1800 = 30 s): Super Mario Land 2 changes its RAM every
 * frame, so the write-back two seconds after the last change never comes.
 * 0, the default, leaves it out. */
#ifndef META_SAVE_PERIOD
#define META_SAVE_PERIOD 0
#endif
/* Number of rows per DMA strip (not a boolean). Default 2 saves 1280
 * bytes with two buffers; 4 reduces transfer/IRQ count.
 * Both sizes work with the menu (it slices glyphs to the scratch size). */
#ifndef META_STRIP_LINES
#define META_STRIP_LINES 2
#endif
/* Same picture: flash layouts and bitmap validity. 0 restores RAM arrays. */
#ifndef META_DISPLAY_COMPACT
#define META_DISPLAY_COMPACT 1
#endif
#ifndef META_SMOOTH_ASM
#define META_SMOOTH_ASM 1
#endif
#ifndef META_SMOOTH_SPECULATE
#define META_SMOOTH_SPECULATE 1
#endif
/* Builds with a ROM sector cache (SD or packed) or cartridge RAM on the
 * card use two strips in flight instead of three, no BLEND/DARK (their
 * 1.3 KiB of tables). Check the linked RAM/stack budget per game. TBLEND
 * (META_TBLEND, the default mode) is in every build, the same picture:
 * 1 keeps its 15 held lines (672 bytes), 2 one held line in SMOOTH's
 * buffers (32 bytes; 704 bytes saved on Super Mario Land 2) at the cost of
 * rendering those 15 lines on every drawn frame, also when they did not
 * change (frames that did not change at all are still skipped); 0 leaves
 * it out (TILE instead). */
#if (defined(GBL_ROM_STREAM) && GBL_ROM_STREAM) || GBL_ERAM_ON_SD
#define META_STREAM_RAM 1
#ifndef META_TBLEND
#define META_TBLEND 2
#endif
#ifndef META_LCD_BUFFERS
#define META_LCD_BUFFERS 2
#endif
#ifndef META_MERGE_MODES
#define META_MERGE_MODES 0
#endif
#else
#define META_STREAM_RAM 0
#ifndef META_TBLEND
#define META_TBLEND 1
#endif
#ifndef META_LCD_BUFFERS
#define META_LCD_BUFFERS 3
#endif
#ifndef META_MERGE_MODES
#define META_MERGE_MODES 1
#endif
#endif

#endif

#endif
