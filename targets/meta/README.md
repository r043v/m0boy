# Gamebuino META target (`--target meta`)

`gbrecomp --target meta` turns a DMG ROM into a PlatformIO project for the
[Gamebuino META](https://gamebuino.com) (ATSAMD21G18A: Cortex-M0+ at 48 MHz,
32 KiB RAM, 256 KiB flash with a 16 KiB loader, 160×128 ST7735 display, DAC
speaker). It is a separate, compact backend: the desktop runtime and emitter
are untouched.

| Part | Location |
| --- | --- |
| Emitter | `recompiler/src/codegen/meta_emitter.cpp` |
| Runtime (portable C) | `targets/meta/runtime/gbl_*.c` |
| Optimisation profiles | `targets/meta/runtime/gbl_profile.h` (every switch, per console) |
| Shared frontend | `targets/common/platform/` (`m0_*`: menu, states, FAT, SD protocol, pad; console facts in `m0_target.h`) + the Game Boy's own `targets/meta/platform/` (`gb_main.cpp`, `gbl_save/stream/display/palettes`) |
| META drivers | `targets/consoles/meta/` (startup, SPI, display + DMA, audio, SD bus, clock; PlatformIO templates, board) |
| Host harness | `targets/meta/host/gbl_host.c` |
| Cycle estimator | `targets/meta/tools/m0bench/` |
| Tests | `targets/meta/tests/`, `tests/test_meta_target_e2e.py`, `tests/test_pokitto_target.py` |
| Pokitto drivers | `targets/consoles/pokitto/` (see [Pokitto](#pokitto---target-pokitto)) |

## How it works

- **Coverage-driven compilation.** Only instructions that a profiling run
  actually executed are compiled, so data is never mistaken for code and the
  output fits in flash. Everything else, including RAM code such as OAM DMA
  routines, runs in the compact `gbl` interpreter. The interpreter is also
  the reference for differential tests.
- **Regions.** Covered instructions are grouped by address into C functions.
  Inside a function, control flow uses `goto`. CALL is a C call followed by a
  return-address check, and RET is a C return. A mismatch unwinds to the
  nearest `gbl_exec()` loop, which resumes from `gbr.pc` through compiled code
  or the interpreter. A C stack budget bounds nesting.
- **Cheap state.** Flags are stored as `zr` (Z is set when it is zero) plus
  0/1 bytes for C/H/N. A per-region liveness pass emits only the flags that a
  later instruction can observe. Cycles are summed per straight-line run and
  flushed before control transfers and IO accesses. Pending cycles carry
  across labels (most instructions are labels, because an interrupt can
  return there); gotos and the region's entry switch subtract the carried
  amount instead.
- **Wait loops.** The generator recognises loops with no side effect other
  than registers and flags, that carry nothing from one iteration to the
  next: polling LY, STAT, IF, the joypad or a RAM flag set by an interrupt.
  They repeat the same iteration until the value they read can change: at
  the next PPU mode or line change for LY/STAT, at the next device event
  otherwise. The back edge then adds whole loop periods to the cycle counter
  (`gbl_spin`). Delay loops (`DEC r`, or `DEC rr; LD A,hi; OR lo`, then
  `JR NZ`) skip whole iterations up to the next event (`gbl_spin_n`), then
  replay the last one so registers and flags match. The interpreter
  shortcuts the `DEC A / JR NZ,-3` OAM DMA wait that games copy to HRAM.
  The results are identical to running every iteration, frames, WRAM,
  registers and cycle counter included.
- **Lazy devices.** One T-cycle counter drives everything:
  - The PPU renders a whole scanline at the end of mode 3, straight into the
    frontend's RGB565 DMA buffer.
  - The timer, STAT, LYC and VBlank raise events only when they become
    observable.
  - Interrupts are polled at region entry, at loop heads, on HALT, after EI
    and after writes to IE, IF, STAT, LCDC or LYC.
- **Sound.** The APU keeps channel state on register writes and synthesizes
  a whole frame of 8-bit mono samples at VBlank. The frame sequencer runs in
  sample time.

The `--meta-precise` mode polls interrupts before every instruction, exactly
like the interpreter. It is meant for differential testing, not for the
device.

## Optional Clang firmware compiler

GCC remains the default. Pass `--meta-compiler clang` to generate a META or
Pokitto project using Clang for C/C++; GNU ARM assembly, libraries and linking
remain in use. Batch builds accept `FIRMWARE_COMPILER=clang`; the Node frontend
also accepts `--compiler clang`. The estimator accepts `--compiler gcc|clang`.
See [compiler selection and requirements](../../docs/COMPILATEURS.fr.md).

## Experimental generated Thumb blocks

`--meta-thumb` symbolically simplifies register-only `LD` / `NOP` runs and
emits Thumb-1 for small plans, with equivalent C on the host. It preserves
cycle accounting and does not cross entry labels, memory effects or EI's
interrupt boundary. `--meta-precise` retains the original per-instruction
path. The option is off by default; performance gains are not established.
See [the design and validation notes](../../docs/THUMB_REGISTER_IR.fr.md).

## Workflow

```bash
# 0. Build gbrecomp (repository root)
cmake -G Ninja -B build . && ninja -C build

# 1. Interpreter-only project + host tools
./build/bin/gbrecomp game.gb --target meta -o output/game_meta
cmake -G Ninja -S output/game_meta/host -B output/game_meta/host/build
ninja -C output/game_meta/host/build

# 2a. Record coverage by playing (SDL2 window; Esc saves and quits)
#     The coverage-guided player (--explore) is retired on this core: deepeel
#     explores (deepeel explore ROM --play --cov game.cov) and the host
#     consumes its maps. Playing with your own hands and recording stays.
./output/game_meta/host/build/game_play --interp \
  --coverage logs/game-1.cov --record-input logs/game-1.input

# 2b. ...or from a scripted run ("<frame> <buttons>" lines)
./output/game_meta/host/build/game_profile --interp --frames 3000 \
  --input logs/game-1.input --coverage logs/game-1.cov

# 3. Compile the covered code (several --coverage files are merged)
./build/bin/gbrecomp game.gb --target meta --coverage logs/game-1.cov \
  -o output/game_meta

# 4. Firmware
pio run -d output/game_meta                 # check the RAM/Flash report
pio run -d output/game_meta -t upload       # USB (sam-ba, offset 0x4000), see below
# or copy output/game_meta/.pio/build/meta/firmware.bin to the SD card
```

Coverage is incremental. Run a compiled build with `--coverage` but without
`--interp`: it records only the instructions that still fall back to the
interpreter. Regenerate with every coverage file.

`--coverage` also takes a deepeel coverage profile directly (`.cov`, the
"gbl coverage v1" text: `bank addr flags(1=exec,2=entry,4=speculative,8=data)
hits`, runs as data when flag 8 carries a fifth run-length column) — this is
what a full deepeel campaign writes, so its map drives the compilation with
no m0boy-side discovery:

    deepeel explore game.gb --play --tas ... --cov game.cov
    ./build/bin/gbrecomp game.gb --target meta --coverage game.cov --output output/game_meta

Super Mario Land (World), cov of the deepeel TAS campaign (42 706 records):
6 963 compiled instructions in 74 regions, interpreter-only 0.

`--coverage` also takes a deepeel map PNG (`deepeel explore game.gb --play
--map game.map.png`). Its `deepeel_cov` chunk is the coverage of the played
sessions only (hit counts rank the flash budget; proven-data runs exclude
bytes from compilation), and, with no `--rom-profile`, its `deepeel_rom`
chunk (played ROM reads per 256-byte page) picks the resident ROM chunks when
the ROM is streamed or packed (`ROM read profile:` in the summary).
`deepeel mapx game.map.png out --chunk deepeel_heat` lists the zones ranked
by played usage (the hot set carries 90% of it).

The firmware is bare metal (`meta_startup.c`, `meta_spi.c`; no Arduino
core, no USB stack): it prints nothing on USB, and `-t upload` only reaches
a META whose bootloader is already running, since nothing in the firmware
answers PlatformIO's 1200-baud reset request. Copying `firmware.bin` to the
SD card works as for any META game.

Play-mode keys: arrows, X = A, Z = B, Enter = Start, Backspace or Right
Shift = Select.

On the META:

| Button | Game Boy |
| --- | --- |
| D-pad | D-pad |
| A | B (swapped: `META_SWAP_AB`, default 1) |
| B | A |
| MENU | Start |
| HOME (tap) | Select |
| HOME (hold ½ s, alone) | Option menu |
| HOME (hold) + direction, A, B | one of the GBC boot ROM's 12 generic palettes, live (its power-on combos: first direction in R, L, U, D order, +4 with A, +8 with B); any second key keeps the menu off — HOME stays exclusive, and no Select reaches the game on release |

The option menu (`meta_menu.cpp`, after the menus of fgb, the GP32 port of
gnuboy) pauses the game: up/down pick a line, left/right (or A) change its
value, A runs an action, B or HOME closes it. The game's sound is paused
too (`m0_audio_pause`, added for 0.7.0: the DAC to mid-scale and the
sample source stopped while the menu is open). Its text uses dstar's
greenfont, converted from Gdl's agf format to 2 bits a pixel by
`tools/font/agf2meta.py` (`meta_font.h`, 1.5 KiB of flash instead of 6.6 KiB
as agf), drawn through the strip buffers of the display pipeline. Under
the lines, the emulated and drawn frames of the last second. Options (not
kept across power-off), from fgb's menu where they apply on the META:

| Line | Values |
| --- | --- |
| Reprendre | back to the game |
| Redemarrer | writes the save, then `gbl_reset()` and `gbl_run()` again (a `longjmp` back to `setup()`) |
| Etat, Sauver l'etat, Charger l'etat | save states, slot 1 to 10 (shown only when the state file is on the card, see below) |
| Volume | 0 to 8; 4 (default) is the former level, half the DAC's range, 8 the whole range |
| Ecran | display mode (below) |
| Palette | Vert (default), GBC (only when gbrecomp recognised the title: the Game Boy Color boot ROM's palette for this game, resolved at conversion time — the table lives in the converter, the firmware embeds the four colours), Gris, Pocket, Ambre, Bleu: every mode but SMOOTH, whose 4 KiB colour tables are built at compile time for the first one. TBLEND's ½-weight table is built from the palette in RAM (1 KiB in the union of mode buffers, or 16 colours without `META_MERGE_MODES`). The boot ROM's 12 generic palettes are in every firmware too (its 30 base palettes and the (OBJ0, OBJ1, BG) triplets, `GBL_GBC_BASE`/`GBL_GBC_COMBOS` in `gbl_config.h`): a direction held at power-on, plus A and/or B, picks one like on the GBC — until sprites get palettes of their own, only the BG third shows |
| Images | frame skip: Auto (default: when emulation falls a frame behind, up to `META_MAX_SKIP` frames in a row are not rendered), Aucun, 1 sur 2/3/4 (one frame rendered in two, three, four) |
| Vitesse | Normale, or Rapide: no frame limit (fgb's Synchronization off) |
| Processeur | 70 MHz (the META's default, `META_OVERCLOCK`) or 48 MHz (below; the Pokitto runs 72) |
| SPI ecran | display SPI clock: 12, 24, 36 or 48 MHz, 48 (default, `META_LCD_SPI_MHZ`) at 48 MHz CPU and 36 with the CPU at 72 MHz (below). A rate the panel does not take scrambles the screen, menu included: switching off restores the default, and a firmware rebuilt with `-DMETA_LCD_SPI_MHZ=24` (build_flags) boots at that rate |
| Quitter | back to the Gamebuino loader |

Not taken from fgb: gamma (the palettes cover it), double buffering and
the CGB/debug switches (no use here).

Save states: the converters (`compile.sh`, `tools/meta-compiler`) create
`<NAME>.STA` next to the game, `GBL_STATE_SLOTS` (10) slots of
`GBL_STATE_SLOT_SECTORS` sectors (a header, the machine within 18 KiB, the
cartridge RAM: 190 KiB in all for a game without it, 270 KiB with 8 KiB).
The firmware finds it at boot like the save file and only then shows the
three lines; it never resizes it. `gbl_state_io` (runtime) streams the
machine in a fixed order: CPU, WRAM, VRAM, OAM, HRAM, IO, mapper, timer,
buttons, frame counter, cartridge RAM (copied from the save file when it
lives there), PPU and APU state; a load rebuilds what is derived (page
pointers, dispatch cache, sprite order, tile versions, events). The menu
runs inside a frame, possibly mid-instruction, so the operation waits for
the next interrupt check (`gbl_safe_request`, tested in `gbl_irq_enter`,
whose callers are all between two instructions with `gbr.pc` exact); a
load then restarts `gbl_run` from the loaded `gbr.pc`, dropping the C
frames of the code that was running. Each slot's header (written last)
holds the generator version and state size, the title, the size and a
checksum; a load reads the slot once to check it before applying it, so
a state cut short by a power loss or from another firmware is refused.
The host harness checks it: `--state-test N M` saves after frame N, plays
M frames, loads and plays them again, and compares the whole state (in the
e2e test: cartridge RAM on the card and in RAM, interpreted and compiled).
In fast builds, restarting from `gbr.pc` can move an interrupt by a few
instructions compared with running on: both are valid executions.

Pacing keeps the 59.73 Hz rate with an accumulator of elapsed time against
emulated frames (Gdl's `rate`): an early frame waits, and the automatic
frame skip renders nothing while a frame or more is owed.

72 MHz (`meta_clock.cpp`) is past the SAMD21's 48 MHz rating. Generator 5
divides the DFLL48M to 1 MHz, the FDPLL96M multiplies it by 72 and
generator 0 (CPU) switches to it with two flash wait states;
TC4/TC5 and the DAC run from generator 4 (DFLL48M) at either clock,
SysTick is reloaded for 1 ms and `m0_micros()` scales its fraction.

SERCOM4 (the SPI) has generator 6 to itself. A synchronous SPI runs at
most at half the SERCOM's clock, so the display's rate picks the source:
12 and 24 MHz from the DFLL48M, 36 and 48 MHz from the FDPLL96M at 72 or
96 MHz (past the SERCOM's 48 MHz clock rating; gm0's overclocked META port
ran the ST7735 at 36). The PLL has one frequency: with the CPU at 72 MHz
the display gets 36 MHz at most. `meta_spi_rate()` derives every divider
from the SERCOM's actual clock, so the card and the buttons keep their own
rates: the buttons at 12 MHz, the card at 17.5 with the CPU at 70 MHz
(`META_SD_SPI_HZ` asks 18; `META_BUTTONS_SPI_HZ`, gbl_board.h). The
SAMD21's SPI master does not sample MISO at its fastest rates: on a META
no data token came at 24 MHz (DFLL48M, 2026-10-16) nor at 35 (PLL at 70,
2026-10-17); 17.5 read Super Mario Land 2 without a CRC error. With
`META_SD_CRC` every block's CRC16 is checked and a failed read halves the
rate (down to 12 MHz) and is tried again. The display only writes and is not limited by this. The
SAMD21 has a single FDPLL96M: with the CPU at 70 MHz the display's SERCOM
runs from it too, so 35 MHz is the display's ceiling; 48 MHz needs the PLL
at 96 MHz, which only the CPU at 48 MHz leaves free. When the PLL does not lock the menu stays at 48 MHz, and the
loader is always entered at 48 MHz. On the META the overclock is 70 MHz
(the menu's "72" asks for it, `OC_MHZ` in meta_clock.cpp), with no code in
SRAM (`GBL_PPU_RAMFUNC` 0). Measured on one META (2026-10-17): at 72 MHz,
and at 70 with the PPU loops in SRAM, the first frames computed wrong
values and faulted (PLL from the DFLL48M or the 32.768 kHz crystal, two or
three flash wait states); at 70 MHz from flash, minutes of play without a
fault. An earlier Game Boy emulator ran the META at 70 MHz too. On the M0 estimator a frame at 72 MHz (two wait states) costs
about 1.35 times less of its budget than at 48 MHz.

Display modes (144 DMG lines on 128 META lines), chosen in the menu:

| Mode | Method |
| --- | --- |
| SMOOTH | Area resampling, 9 lines onto 8 rows: row k of each group is (8 − k)/9 of line k plus (k + 1)/9 of line k + 1, nothing dropped. Lines are rendered packed (2-bit shades, `gbl_ppu_packed`, no colour conversion in the PPU) and every row is blended from two of them, two pixels per lookup in 4 KiB of compile-time tables (Thumb-1 loop in `gbl_display_asm.S`) |
| TILE | Drops the last line of each tile row (8k+7), keeping lines 63 and 95; fonts usually leave that line blank, so text stays intact (taken from gm0) |
| BLEND | Merges lines 8 and 9 of every 9 with averaged colours (1 KiB table) |
| DARK | Same merge, keeping the darker shade |
| CROP | No scaling: DMG lines 8–135 |
| SKIP | Drops the 9th line of every 9 |
| TBLEND (default) | TILE's rows, but line 8k+7 is averaged into line 8k+8 instead of dropped: 7 rows of 8 stay sharp, only line 143 is lost (taken from gm0's ESPboy port). The two lines are rendered packed; the 15 held lines are kept across frames (600 bytes, in the RAM of the BLEND/DARK buffers) so a held line is rendered only when it changes, and the pair goes through SMOOTH's Thumb-1 loop with a 1 KiB table of ½ weights |

SMOOTH and line skipping: a row needs both of its lines, but only the
previous one is kept (keeping all 144 would take 5.6 KiB of RAM). When only
the lower line of a row changed, the upper one was not rendered: that row
is drawn from the lower line alone for one frame and blended on the next.
Lines that changed are rendered again on the next frame, so things that
keep moving find their upper line (0 late rows over 119 frames of sprites
moving down in the test). M0 estimator, real display pipeline
(`--cflag=-DBENCH_META_DISPLAY=0`, 1 for TILE), 25 games: SMOOTH costs
3.6% to 31% more per frame than TILE (median 12%; Super Mario Land 106k →
131k cycles), mostly the blend of every row; frames over the 60 Hz budget
go up on Prehistorik Man (23 → 103 of 1,599) and Faceball 2000 (35 → 67).
With the identity BGP (0xE4, most games) packed lines are the raw shades,
copied without a table.

TBLEND on the same 22 non-streaming games of the panel (estimator, real
display pipeline, `-DBENCH_META_DISPLAY=6`, projects regenerated with the
current runtime): +0.9% to +7.2% per frame over TILE (median +1.9%;
Lemmings, non-identity BGP, is the +7.2%), against +9.5% to +32% for SMOOTH
(median +15.3%) and +7.9% to +27.8% for BLEND. Frames over the budget stay
at TILE's level (Prehistorik Man 29 against 25; SMOOTH 100). RAM is
unchanged (Super Mario Land 29,784 bytes either way), flash +1.4 KiB. A
first version that rendered the held line every frame through the palette
switch of BLEND cost as much as SMOOTH (median +15.5%).

`gbl_display_panel_matches_reference` runs the real display pipeline
against a simulated ST7735 (row windows included) and checks every mode
pixel for pixel over 400 frames of random changes (SMOOTH: on the frames
after an unchanged one, the late rows being counted), plus the SMOOTH
tables against the formula and a run of sprites moving down. `<name>_metaview
--mode … --dump N file.ppm` writes the exact META screen of a game on the
host.

Faults and hangs (meta_startup.c): a HardFault, an unhandled interrupt or
the watchdog's early warning (the main loop fed it last 2 s ago:
`m0_watchdog_feed`, once per frame, in the menus and on every SD sector)
logs the exception frame in `meta_fault` (`.noinit`, kept across resets),
answers USB for two seconds, then restarts the game with a software reset.
Three faults in a row within 5 s of boot stop the restarts (the firmware
then waits for the host). The watchdog itself resets after 4 s when
interrupts are off; the bootloader then starts the launcher, and the next
boot's reset cause says so. `tools/meta-usb.py fault --elf firmware.elf`
prints the log with addr2line.

The buttons are read by the bus driver (`meta_pad_read`, meta_spi.c) the
Gamebuino library's way: 12 MHz (`META_BUTTONS_SPI_HZ`; 48/4, 72/6 and
96/8 all give it), CS low, 1 us for the parallel load
(`META_BUTTONS_PL_US`), one byte, in the library's bit order (0 down,
1 left, 2 right, 3 up, 4 A, 5 B, 6 MENU, 7 HOME), measured on a META on
2026-10-16 from a RAM log read back through sam-ba, in game and on the
error screens. The read first waits for the display's strips (its CS is
low between strips of a frame), lets the last byte leave the shifter and
drains the receive FIFO. `meta_spi_init()` sets PA12 (MISO) as an input
(INEN) and every chip select inactive: without INEN the pad and the card
read 0. Every frontend then decodes its console's raw bits once through
`META_PAD` (meta_pad.h) into the menu's `M0_MB_*` order — the same
mapping code everywhere, the Game Boy's A/B swap, the Sega START and the
PICO-8's C-tap reading the same named bits — and gm0 (EMU_INPUT) drives
the same roles: H is the m0boy menu key on every console (the META's
HOME, the Pokitto's C).

## Saves and large cartridge RAM

Battery-backed cartridges (`GBL_BATTERY`, from header byte 0x147) keep
their cartridge RAM in a file on the SD card, `GBL_SAVE_NAME` (e.g.
`MQ.SAV`). `compile.sh` creates it at exactly the size of the RAM (filled
with 0xFF, an erased cartridge) and never overwrites it, inside the game's
folder (see below). Copy the folder to the card once, in the root
directory: the console rewrites its 512-byte sectors in place and never
touches the FAT.

- RAM up to 2 KiB (MBC2's 512 half-bytes, for instance) stays in RAM. It is
  loaded at boot; writes go through the slow path and mark sectors dirty.
- Larger RAM (`GBL_ERAM_ON_SD`, the default above 2 KiB: 8 KiB for Link's
  Awakening, 32 KiB for Pokémon) lives only in the file, behind a two-sector
  write-back cache. Without that, those games did not fit in RAM (8 KiB on
  top of 29 KiB). Such builds, like streaming ones, keep two display strips
  and drop the BLEND and DARK modes (TBLEND stays with one held line, `META_TBLEND` 2). Zerd no Densetsu (8 KiB) builds at
  27.5 KiB of RAM.
- Changed sectors are written back in three cases:
  - when the game disables cartridge RAM, which games do right after
    saving;
  - two seconds after the last change;
  - before returning to the loader (menu, Quitter).

  Switching the console off during the two seconds that follow a save
  without RAM disable can lose it.
- With a streamed ROM too, both share one pool of 64-byte blocks
  (`GBL_CACHE_UNIFIED`, `GBL_CACHE_BLOCK` 64/128/256/512, `GBL_CACHE_BYTES`,
  by default 1 KiB less than the two caches it replaces), CLOCK
  replacement, the last ROM block and the newest RAM block never evicted.
  The card and the unpacker still work on 512-byte sectors through one
  sector buffer, which also serves the next block of the same sector; a
  dirty block is written back with the rest of its sector read first. The
  option menu runs on the pool (all but 3 blocks, written back first and
  kept out meanwhile); `GBL_CACHE_MENU_STACK` is what it needs there. Super Mario Land 2's demo to frame 6000 (device):
  3963 card reads, 1567 writes and 14200 ROM sectors unpacked with ROM 3 +
  RAM 10 sectors; 226, 69 and 96 with 128-byte blocks. Taking 1 KiB from
  the C stack for the pool cost 5 frames/s (more unwinds).
- Cached blocks are read and written through two tables (one pointer per
  block of A000-BFFF, `gbl_eram_rmap` / `gbl_eram_wmap`), rebuilt whenever
  a block, its reference bit, its dirty bit, the bank or RAM enable
  changes; only blocks not in them go through the search
  (`GBL_ERAM_TABLES` 0 sends every access there). Super Mario Land 2, whose
  stack and variables live there: about 38 frames/s through the search,
  47-50 with the tables (device, demo, separate caches).
- A card read or write that fails identifies the card again, as at boot,
  and is tried once more (`meta_sd_reinits`, `meta_sd_reinit_fails`); a
  sector whose write fails stays dirty. With a flat battery the card stopped
  answering after a few thousand frames: the cache then gave the game
  sectors of 0xFF or stale ones (Super Mario Land 2 jumped to its ending or
  restarted). Debug builds: `GBL_DEBUG_STACK` (`meta-usb.py stack`),
  `GBL_DEBUG_RESTART` (`meta-usb.py restart`: RAM switch-off, RETI
  registers, the first interrupt taken at a pc in 0068-014F), `META_DEBUG_TRACE`
  (each frame's pc, sp, hl, stack top in a ring that `meta-usb.py trace`
  appends to a file), `META_DEBUG_RESET`, `M0_DEBUG_BUS`;
  `GBL_DEBUG_ERAM_MAPS` and `GBL_DEBUG_ERAM_SUM` need `GBL_CACHE_UNIFIED=0`.
  These records take RAM from the C stack: at 98.6% of RAM the stack ran
  into them. A build that leaves the generated code less than
  `META_STACK_NEST_MIN` (256) bytes above `META_STACK_RESERVE` stops on a
  cyan screen.
- The pool, its CLOCK replacement, the sector buffer and the write-back
  are one module shared with the Game Gear / Master System runtime
  (`targets/common/runtime/m0_cache.h` and `m0_cache.inc`, included with
  each console's hooks): Super Mario Land 2 and Pokémon firmwares built
  from it are byte-identical to 0.10.25's but for the version string. The
  card files (`meta_card.h`: three attempts per sector, the ROM lookup) and
  the boot error colours are shared too: blue no card, red ROM file
  missing, orange fragmented, magenta another game's ROM, yellow save file
  missing (the Game Gear used other colours before 0.10.26).
- Audio goes out by the TC4 interrupt (`META_AUDIO_DMA` 0): since 0.10.29
  the define selects the output path at **build** time — 0 the interrupt,
  1 a DMAC beat per ring entry — and the run-time switch of 0.10.28
  (`meta_audio_mode()`, the menus' Audio line) is gone: the DMA path's 0.5 %
  of CPU (measured on Pokémon Yellow, below) were not worth its restart
  logic and descriptor history (SAM D21 erratum 1.7.2), and on the Game Gear
  the display already shares the DMAC.  One shared ring either way
  (`META_AUDIO_RING` samples of 2 bytes, 512, about 1.4 frames, lowered so
  the card cache grows — the streaming Game Gear builds keep about 2 KiB of
  RAM free with it; the deepest stack measured there, device 2026-10-20, is
  1656 bytes).  Until 0.10.28 the paths had two rings chosen at build time
  (the interrupt's `META_AUDIO_IRQ_RING`, 1024 one-byte samples, 512 on the
  Game Gear; the DMA's `META_AUDIO_RING`, 768 samples of 2 bytes).  Device,
  2026-10-19, fast mode, frames 600-4800 of Pokémon Yellow's intro, two runs
  each, identical: DMA 202.0 frames/s, interrupt 200.9 (0.5%); Super Mario
  Land 180.7 / 180.9.
- The option menu is on every core since 0.10.28 (it was the Game Boy's
  alone): HOME held half a second (Pokitto: C, whose tap keeps the game's
  key; ESPboy: HOME) opens it on the Game Gear, Master System, SG-1000 and
  PICO-8 cores too, with lines of their own (Reprendre, Redemarrer, Volume,
  Vitesse, Processeur, Quitter; the Game Boy keeps its states, screen modes
  and palettes).  Since 0.10.29 the menus also carry **save states on the
  Sega cores** (Etat, Sauver l'etat, Charger l'etat; the state file
  `<NAME>.ST8` sits on the card next to the ROM, made by `compile.sh`, ten
  slots of the machine's bytes — on the host, `Ristar_host --save-state` /
  `--load-state` round-trips the same stream, byte-identical resumes).
  Since 0.10.34 every Sega game has them, on the three consoles: the
  Game Gear and Master System games whose ROM is all in flash open the card
  for the state file alone (packed ROMs too, which did not open it), the
  SG-1000 gains `sg_state_io` (the machine's bytes, its pointers kept, the
  renderer's caches rebuilt), and the ESPboy keeps the slots in its flash
  after the cartridge RAM's copies (8 for a Game Gear game, 5 with
  cartridge RAM, 10 for an SG-1000 one; `espboy_flash.cpp` serves all the
  cores), and the PICO-8 has them too (META, Pokitto and ESPboy: `p8_state_io`, the
  heap with its absolute pointers, so a state loads only into the firmware
  whose code hash wrote it; `docs/PICO8.fr.md`).  Checked in gm0 on each console: save, play on, load, the menu
  says "Etat charge" and the picture is the saved one.  Also a
  **Bootloader USB** line (the sam-ba magic where the META's bootloader
  looks after a software reset — reached without META_USB, where the double
  reset opens the loader instead of the bootloader), and **Vitesse runs
  without drawing but for one frame in thirty**: drawing every frame put the
  LCD bus in every frame and made the fast mode slower than the paced one.
  The Audio and SPI ecran lines are gone (the audio path is a build choice,
  the display SPI always runs at the most the clock tree gives —
  `META_LCD_SPI_MHZ` clamped by `meta_clock.cpp`).  The shared part is
  `targets/common/platform/sg_menu.h`; the menu draws through each
  frontend's line buffers (`m0_menu_scratch`, meta_menu.h).  The
  streaming Game Gear builds run it on the card cache's blocks, lent out
  (`gg_cache_lend`, the save written back first); its Redemarrer is a power
  cycle: the cartridge RAM is flushed to the card, then cleared.

On the META, `META_USB_MODE` selects 0 none, 1 serial, 2 SD, or 3 serial + SD
(default). With SD enabled, the GB, Sega and PICO-8 menus add **Carte SD USB**.
Choosing it finishes display DMA, flushes the GB save, stops audio and the
watchdog, then detaches USB and exposes the whole SD card as a Mass Storage
drive. The game remains stopped until reset: the PC owns FAT exclusively.
USB endpoint buffers are shared with the serial mode; the 512-byte sector
buffer comes from display scratch RAM after the menu has closed. A host eject
request returns to the game automatically; unplugging USB does the same after
the bus has been disconnected for 750 ms. You can also hold the META's MENU
and HOME buttons together for half a second. Eject or unmount the drive on the
PC before using the button shortcut. ZIP extraction is performed by
`tools/meta-sd-usb/meta_sd.py` on the PC, not in the firmware.
On 2026-10-23, a synthetic SG-1000 firmware was uploaded to a physical META:
the serial port responded during play, then **Carte SD USB** re-enumerated as
`META SD Bridge`; Linux mounted and listed its 7.3 GiB FAT32 card. A small
file was written, read back, deleted and the volume unmounted cleanly.
For a synthetic GB firmware built with GCC `-O2`, mode 3 versus serial-only
mode 1 adds 3,652 bytes of flash and 56 bytes of static RAM (PlatformIO
linker report, 2026-10-23). That number is a build comparison, not a device
memory measurement; each game's compiled-code budget must still fit.
- The first-level folders of the card are searched 32 at a time: before
  0.10.26 only the first 32 were, and a game in a later folder found
  neither its own folder nor its ROM (red screen).
- If the file is missing, a game whose RAM must live on the card shows a
  yellow screen. A game whose RAM stays in RAM runs normally, but its
  progress is not kept.
- On the host, `--save FILE` uses the same file format. Without it, the RAM
  starts erased and is not kept.

Test on the host: Gem Gem (8 KiB) writes 5 sectors on its first run and
reads them back on the next (different state, nothing rewritten). The e2e
test checks the same round trip on a synthetic battery cartridge, and that
RAM on the card and RAM in memory give the same machine state.

Compiled regions call each other directly. Each one now checks the C stack
on entry (`ENTER()`, `GBL_STACK_GUARD`), like `gbl_exec`. A guest that
recurses without returning (a crashed game looping on `RST $38`) therefore
unwinds instead of overflowing the C stack. The check costs about 0.3%
(SML).

`gbl_exec` and `gbl_run` only call a compiled region when at least
`GBL_EXEC_HEADROOM` (512) bytes remain above the limit, so the region's own
guard always passes at least once. Without that margin a region entered
just above the limit returned at once without progress and the dispatcher
called it again forever (seen on R.C. Pro-Am with a packed ROM at frame 86).
With no room at all, `gbl_run` interprets instead. The host takes
`--stack-kib N` or `--stack-bytes N` to reproduce small budgets.

### ROM cache and C stack sharing their RAM (`GBL_CACHE_DYN`)

On the META and the Pokitto, the streamed ROM's cache (or the unified pool)
is a fixed part plus blocks that live in the C stack's RAM. The fixed part
(2 sectors, or in the pool the menu's stack, a sector and 3 blocks;
`GBL_STREAM_FIXED`, `GBL_CACHE_FIXED_BYTES`) is the `.cachepool` section,
the last thing in RAM in `gamebuino_meta.ld` and `pokitto.ld`. The option
menu runs on that part only. The other blocks follow it, up to
`GBL_STREAM_MAX` / `GBL_CACHE_MAX_BYTES`. At boot the cache gets as many
as `GBL_STREAM_SECTORS` / `GBL_CACHE_BYTES` ask, so it starts with the same
split as a fixed cache, while the stack keeps `META_STACK_DYN_MIN`.

After that, `m0_stack_pool.h` (targets/common/runtime) moves the boundary
by moving `gbl_stack_limit` (`sg_stack_limit` for the Game Gear and Master
System, `GG_CACHE_DYN`). The generated code is unchanged.

- A frame with an unwind (a `gbl_exec` / `sg_exec` call refused for want of
  stack, counted in `gbl_exec_refused` / `sg_exec_refused`) gives the stack
  one block at once: free RAM first, then the cache's last block (written
  back first if it holds dirty cartridge RAM), never below the size the
  cache had after boot. A game is therefore never slower than with the
  cache the build sized.
- RAM goes back to the cache only with room to spare: no unwind and the
  calm zone (a block, `M0SP_NEAR` and the dispatcher's headroom above the
  limit, painted) untouched for `M0SP_CALM_WINDOWS` windows of
  `M0SP_WINDOW` frames (8 x 30), the cache loading blocks meanwhile, and the
  stack `M0SP_MARGIN` above that zone when the block is taken. One block at
  a time, then the count starts over. Nothing flickers between the two.
- The frame's call can come deep in the game's calls (Super Mario Land 2:
  always). The block is then taken from `gbl_exec` / `sg_exec`, on entry
  with the stack above `gbl_pool_sp` / `sg_pool_sp`.

Raising the limit needs the stack above the calm zone at that moment: the
RAM under it is dead, and every running dispatcher loop (above the current
frame) stays at least its headroom above the raised limit, so no region is
refused at entry forever.

The Game Gear / Master System frontend fills the cache at boot while the
stack keeps the guard `compile.sh` used to size the cache by (2000 bytes,
2400 without USB; `GG_DYN_STACK_GUARD`); `compile.sh` no longer grows
`GG_STREAM_SECTORS` from the RAM report (`CACHE_DYN=0` restores fixed
caches). On the Pokitto the Game Gear cache is in the USB RAM bank and
stays fixed.

The menu's info line reads `59/41 pile 812 c36`: emulated and drawn frames,
stack never used above the cache, cache blocks. The host and the estimator
(`BENCH_FRAMES`) keep the whole cache in its array, as before.

Cache blocks are 64 bytes by default on the META (`GBL_CACHE_BLOCK` for the
unified pool, whose default `GBL_CACHE_BYTES` keeps the same RAM: 3904;
`GG_CACHE_BLOCK` written by the generator). On the host, Super Mario Land 2's demo from a fresh save (12000 frames) at the
same RAM (pool, tags and cartridge RAM tables, about 5.8 KiB): 64-byte
blocks (3904-byte pool) read 1113 save sectors and wrote 239, and decoded
1046 ROM sectors; 128-byte blocks (4608) 10062, 2006 and 5136; 256-byte
blocks 27254, 7592 and 20308. Ristar's demo (8000 frames, about 1.6 KiB):
718 sectors decoded and 357 read with 64-byte blocks, 1576 and 402 with
128. On the device (2026-10-21, 150 s of each demo, fast mode, one run
each): Super Mario Land 2 with the corrected profile (below) loaded 823 ROM
blocks with 64-byte blocks against 3036 with 128 (13646 frames emulated
against 11758; the demo's path depends on timing, so frame counts compare
loosely), Ristar 4131 against 7573 (116.4 frames a second on average
against 115.3; 114.7 with the fixed 128-byte cache).

The host harness, `gbl_metaview` and the estimator read cartridge RAM that
was never written as the fresh `.SAV` `compile.sh` puts on the card (0xFF).
They used to fail those reads; a block smaller than a sector is written
back after reading its sector, so the write-back failed too and the game's
writes were lost. Profiles were then recorded on another path than the
console's: Super Mario Land 2's demo ran a quarter of the interpreted
instructions it runs from a fresh save.

## ROMs larger than the flash (SD streaming)

The whole ROM normally sits in the META's 240 KiB of application flash, next
to about 38 KiB of runtime, so ROMs of 256 KiB and more cannot fit. For
them, the generator switches to streaming (`--meta-stream auto`, the
default; `on` / `off` force it):

- Bank 0 and the most-read 4 KiB chunks of the other banks stay in flash
  and are mapped directly: reading them costs nothing more than usual.
  Compiled code never reads its own instructions from the ROM, so only
  data (tiles, maps, text, tables, music) and the code left to the
  interpreter count.
- The other chunks are read from the ROM file on the SD card, one 512-byte
  sector at a time, into an LRU cache of `GBL_STREAM_SECTORS` sectors
  (8, i.e. 4 KiB; 4 when 2 KiB of cartridge RAM stay in RAM). A read
  first lets the display's DMA queue drain and frees the shared SPI bus,
  then issues CMD17 at `META_SD_SPI_HZ`. A 512-byte sector spends 234 us
  on the wire at 17.5 MHz (341 at 12), card latency on top: about 0.95 ms
  a read in all at 12 MHz on the META (USB profiler, Super Mario Land 2).
- The resident set comes from ROM data-read profiles. In streaming
  projects, the host `_profile` and `_play` builds record them with
  `--rom-profile-out FILE`; pass them back with `--rom-profile FILE`.
  By default, resident data may take up to half the free flash
  (`--meta-rom-resident-kib` overrides this), and compiled code gets the
  rest.
- The ROM file must be on the card under the 8.3 name printed by the
  generator (`GBL_STREAM_NAME`, e.g. `MYSTICQU.GB`), in the root directory
  or one directory below. At boot the firmware finds it through FAT16 or
  FAT32 (`meta_fat.c`; no FAT access afterwards; at most 16 fragments) and
  checks its cartridge header against bank 0. If that fails, the screen
  turns red (file not found), blue (card unreadable), orange (too
  fragmented) or magenta (a different ROM). Holding HOME for 3 s returns
  to the loader.
- To make room for the 2 KiB cache, streaming builds use two display strips
  instead of three and drop the BLEND and DARK modes; TBLEND stays with one held line (32 bytes, `META_TBLEND` 2). RAM then stays at the
  usual level: 28.9 KiB.

`compile.sh` records the profiles, regenerates with them, and copies the
ROM into the game's folder under its card name.

## Compressed ROM in flash (`--meta-pack`, default on)

With `--meta-pack` (the default in `compile.sh` and the Node compiler;
`PACK=0` / `--no-pack` turn it off), the streaming machinery
reads from flash instead of the SD card (`GBL_ROM_PACKED`):
- Bank 0 and the hot 4 KiB chunks stay raw and mapped directly. By default
  they take at most a third of the free flash, so more of it goes to
  compiled code.
- Every other 512-byte sector is LZSS-compressed in flash (Heatshrink-style:
  a flag bit per token, 8-bit offset, 4-bit length), or stored as is when
  compression does not shrink it.
  The encoder now uses an optimal bit-cost parse, retaining exactly the same
  format (8-bit distance, 4-bit length). It is shared with GG/SMS in
  `recompiler/src/rom_storage.cpp`; the reference and Thumb-1 decoders live
  in `targets/common/runtime/m0_rom_codec.h` and `m0_rom_lzss.inc`.
- A cache miss decodes the sector into the same 512-byte slot the SD
  reader would fill. It needs no card, no `.GB` file and no extra RAM:
  back-references read the sector being decoded.
- `gbl_unpack512_asm` (Thumb-1, in SRAM) decodes on Cortex-M0+
  (`GBL_UNPACK_ASM`); the C version in `gbl_core.c` is the reference.
- Before packing, if the whole raw ROM and requested code fit the estimated
  flash budget, a plain build is preferred (explicit streaming/residency
  requests override this). When the whole compressed ROM does not fit and
  read profiles are available, a hybrid build keeps hot chunks raw or
  compressed in flash and reads the rest from SD (`GBL_ROM_PACKED` together
  with `GBL_ROM_SD`). Otherwise it falls back to SD or a plain build.
  Hot raw/packed choices account for their actual byte costs, not an average
  compression ratio. All of these are planning estimates; the final linker
  report is still the authority on fit.
- The raw part is chosen from the data-read profiles (`--rom-profile`).
  Without a profile nothing but bank 0 stays raw and every cache miss
  decodes. `compile.sh` therefore replays the recorded inputs once with the
  profile build when a streamed or packed project has no `romdata-*.prof`
  (sessions recorded with a plain build have none), and keeps the result
  with the sessions. `PACK` is stored in `.gbrmeta.json`; changing it
  rebuilds the game. For a ROM that fits in flash (Tetris), everything
  touched stays raw and packing changes nothing.

R.C. Pro-Am, 600 frames, M0+ estimator (not a device measurement):

| Build | Cycles/frame | `.text` |
| --- | --- | --- |
| plain (whole ROM in flash) | 359,596 | 218,780 |
| packed, 60 KiB raw | 336,881 (−6.3%) | 218,076 |
| packed, 4 KiB raw, asm decoder | 345,380 (−4.0%) | 210,352 |
| packed, 4 KiB raw, C decoder | 354,050 (−1.5%) | 210,512 |

The gain comes from the flash left to compiled code (68k instead of 188k
interpreted instructions over 400 frames), not from the decoder. The asm
and C decoders give the same pixel hash; with `--interp` all three builds
give identical hashes (every ROM read goes through the decoder). In fast
mode the frames differ slightly from the plain build, as with any change of
compiled coverage.

Measured compression (compressed size / original, 512-byte blocks,
Heatshrink-style 256-byte window): SML 68%, Batman 63%, R.C. Pro-Am 78%,
Tetris 73%, Mystic Quest 78%. FastLZ does 5 to 10 points worse on these
ROMs; zlib does 10 points better, but it needs Huffman tables in RAM.
The SD remains raw: CMD17 transfers a physical 512-byte sector even when
the logical payload is smaller; tightly packed variable-length blocks may
straddle sectors and require two reads. No SD compression speedup has been
demonstrated.

### Shared storage and codec benchmarks (2026-10-15)

GG/SMS now use the same encoders and C/Thumb-1 LZSS decoders as GB. They
prefer all-raw flash, then all-flash raw/LZSS with hot sectors promoted to
raw, then a raw/LZSS flash pool with raw SD fallback. Unused flash is used
to remove decoding from hot reads; a raw-only pool retains its compact
original index and has no decoder/record overhead. Native code remains
directly executable: only source-ROM bytes are packed.

An independent raw-DEFLATE decoder (stored, fixed and dynamic Huffman
blocks, bounded reads/writes) is available in the common codec header for
testing. Its 7/5-bit prefix tables reuse code-length scratch; work space is
**1032 bytes**, not a 32 KiB history window. It is **not selected in generated
firmwares yet**. On six GG hot-sector pools, the production-encoder M0+
microbenchmark estimates **11.7–12.7 kcycles/sector** for LZSS Thumb-1 versus
**66.7–76.1 kcycles/sector** for DEFLATE, plus about 3.4 KiB more decoder
code and 1032 bytes of working RAM (as opposed to the LZSS decoder's SRAM
instructions). These are unweighted compressed-sector microbenchmarks,
not whole-game timings or device measurements. The assumed SD miss cost
in the GG allocator is 50 kcycles, so DEFLATE is not an automatic hot-ROM
candidate under that model.

To reproduce (requires the `m0bench` Python dependencies and the ARM compiler):

```sh
python targets/common/tools/rom_codec_bench.py game.gg --output out/codec-bench
```

The script builds its host preparation tool with CMake/Ninja and the actual
common C/Thumb-1 decoders for Cortex-M0+. It checks **every decoded byte**
and output canaries in Unicorn, reports estimated cycles, static RAM and
observed stack high-water, and leaves ROM-derived files only in the output
directory. The host encoder now requires zlib development headers/library;
no zlib library is linked into firmware.

## Pokitto (`--target pokitto`)

`gbrecomp --target pokitto` writes the same project for the
[Pokitto](https://www.pokitto.com) (LPC11U68: Cortex-M0+, 48 MHz nominal —
the datasheet rates the chip to 50 MHz, PokittoLib's own SystemInit uses
SYSPLLCTRL 0x23 = 48; the menu's « Processeur » offers the 72 MHz _OSCT=2
overclock —; 32 + 2 + 2 KiB RAM; 256 KiB flash
of which the loader keeps the top 28 KiB; 220×176 panel on a 16-bit
parallel bus; 8-bit R2R DAC). Every `--meta-*` option applies; the runtime,
the generated code and the frontend are shared, only the drivers differ.

- **Configuration.** `runtime/gbl_profile.h` lists every optimisation
  switch with its value for each console (see [Build options](#build-options));
  `targets/common/platform/m0_target.h` selects the
  console from `GBL_TARGET_POKITTO` (written into `gbl_config.h` by the
  generator) and sets the `META_*` switches the shared files test with
  `#if`: `M0_TARGET_POKITTO` (whose drivers, `pokitto_hw.h` or
  `meta_hw.h`), `M0_SCALED_MODES` (the META's 144→128 display modes), `M0_SCREEN_ROWS`
  (menu height), `M0_SWAP_AB_DEFAULT`, `M0_CLOCK_DEFAULT` and
  `M0_COLOR` (byte-swapped RGB565 on the META's SPI, native on the
  Pokitto's bus). When the Pokitto target was added, the META firmware
  built from these files was byte-identical to the one built before.
- **Bare metal.** No mbed, no PokittoLib: `targets/consoles/pokitto/` holds the
  startup (vector table, LPC boot checksum written into word 7 by the
  Makefile), the clock (crystal + PLL, 48/72 MHz from the menu; flash wait
  states left at their reset value, as PokittoLib and gm0 do), the display
  (PokittoLib's init sequence; each line written as the PPU completes it,
  unchanged lines skipped by their signature; menu "Ecran": `1:1`, the
  default, the 160×144 picture unscaled in the middle; `200x176`, gm0's
  5:4 enlargement — the second pixel of every four and line 4k latched
  twice, the first and last two of the 180 rows off the panel, so lines 0,
  142 and 143 are not shown; `200x176 entier`, the same with lines 4, 12,
  132 and 140 latched once instead of twice: 176 rows and every line, no
  blending needed since a doubled line is only a repeat. The pixel pushes
  are Thumb-1 from SRAM (`pokitto_lcd_asm.S`, after FManga's
  `FLUSH_QUAD_SCALE` in gm0: about 9 cycles a pixel, the next load placed
  inside the WR strobe; `-DPOKITTO_LCD_ASM=0` keeps the C reference, same
  pixels in all three modes in the emulator), audio (CT32B0 at 22,050 Hz into the R2R DAC, amplifier
  on P1_17, the volume potentiometer set over software I2C), the SD card on
  SSP0 and the buttons. The project builds with `make` and only needs
  `arm-none-eabi-gcc` (PATH, or PlatformIO's `toolchain-gccarmnoneeabi`).
- **Flash and RAM.** 228 KiB below the loader at `0x39000`, so the code
  budget is 12 KiB smaller than on the META. Main RAM holds the machine and
  the stack. The two 2 KiB banks, placed by `pokitto.ld` by section name
  (the shared runtime has no Pokitto attribute), take the line signatures,
  the audio queue and the PPU's BGP byte table (RAM1) and a 256-entry
  dispatch cache instead of 64 (USB RAM, `GBL_LOOKUP_CACHE 256`, about 1%
  on the estimator); the main RAM this frees pays for an 8-sector ROM
  stream cache in streamed builds (`GBL_STREAM_SECTORS 8`: Link's
  Awakening reads 1,365 sectors from the card instead of 2,269 over 6,000
  frames, about 0.5 ms each on the device, not modelled by the estimator).
- **Audio interrupt** in Thumb-1 from SRAM (`pokitto_audio_asm.S`): r0-r3
  only (no push), no flash access, the SET/CLR choice for bit 7 an address
  offset instead of a branch; about 52 cycles plus exception entry and exit
  instead of about 96, i.e. some 2.4% of the CPU at 72 MHz instead of 3.7%.
  Same DAC samples as the C handler (`-DPOKITTO_AUDIO_ASM=0`) over 270,000
  samples of Link's Awakening in the emulator.
- **Controls.** D-pad, A, B (not swapped); C tap = Start, C + B = Select,
  C held half a second (alone) = option menu (same menu as the META, with the
  Pokitto's two display modes), C held at power-on = the Pokitto loader (as PokittoLib
  does; "Quitter" in the menu jumps there too).  Another key while C is
  held (directions and A; B stays the Select gesture) is the GBC's
  power-on palette combo, live, and keeps the menu off.
- **Batch.** `TARGET=pokitto ./compile.sh` produces `roms/pokitto/<Game>/`
  (firmware, `.SAV`, `.STA`, streamed `.GB`; no icons) from projects in
  `out/pokitto/<game>/`, reusing the recorded play sessions of the META.

Status: runs in `tools/meta-emu-sdl --target pokitto` (picture in both
modes, buttons, menu, SD streaming, battery RAM file, save and load of a
state within a session). **Not run on a Pokitto yet.** The display bus and
init sequence, the entry mode, the button pins and pulls, the 72 MHz PLL
setting, the flash wait states and the jump to the loader match gm0 (the
author's Game Boy emulator for the Pokitto, run on the console); the audio
path (R2R DAC, amplifier, volume potentiometer, from PokittoLib) and the
loader's file layout are not covered by it.
The emulator's Pokitto SD model does not answer ACMD41 and its byte-wide
GPIO registers ignore unaligned pins, so the firmware reads the buttons
from `PIN[1]` and writes the DAC through `SET`/`CLR` (both equally valid on
the chip); card tests need an emulator with ACMD41 accepted.

Cost estimate: `m0_estimate.py <project> --target pokitto [--display-mode
0|1|2]` builds the Pokitto display pipeline into the benchmark (its GPIO
pixel stores counted like any store), at 72 MHz with 2 wait cycles per
non-sequential flash fetch; the LPC11U68's flash at 72 MHz is not modelled
from documentation, so also run `--seq-wait 0.5` (an extra half cycle per
instruction fetched from flash) and report the two as a range. The audio
interrupt (22,050 a second, about 75 cycles each, some 2% at 72 MHz) is
not in the benchmark.

Estimate (2026-10-03, 6,000 frames of each game's recorded session,
average over frames 3,000-6,000, the same generated code on both consoles,
static frame skip on; milliseconds of CPU per emulated frame, budget
16.7 ms):

| Game | META 48 MHz (TBLEND) | META 72 MHz | Pokitto 1:1 | Pokitto 200x176 entier |
| --- | --- | --- | --- | --- |
| Altered Space | 2.54 | 1.87 | 2.03–2.48 | 2.10–2.55 |
| Double Dragon | 3.50 | 2.60 | 2.77–3.38 | 2.85–3.47 |
| Faceball 2000 | 3.38 | 2.49 | 2.51–3.10 | 2.55–3.13 |
| Link's Awakening | 3.55 | 2.63 | 2.76–3.37 | 2.83–3.45 |

The Pokitto ranges are the two flash bounds (`--seq-wait` 0 and 0.5). Its
display takes 6–13% of the frame in 1:1 and 8–17% enlarged, against 3–5%
for the META's TBLEND (whose SPI transfer runs by DMA and is not counted).
At the clock each runs by default (META 48, Pokitto 72 MHz) the Pokitto
needs 3–20% less time per frame; against a META at 72 MHz it needs 8–33%
more. These are estimates, not device measurements; the Pokitto build also
has 12 KiB less flash for compiled code than these META projects.

## Batch conversion (`compile.sh`)

`./compile.sh [rom dir] [output dir]` turns every `.gb`/`.gbc` of the
folder into games laid out the way the Gamebuino loader expects, one
folder per game under the output directory (default `./roms/meta/`), each
meant to be copied as is to the root of the SD card:

| File | Role |
| --- | --- |
| `<Game>/<Game>.bin` | firmware, named after the ROM with tags stripped (`Zerd (J) [!].gb` → `Zerd`) |
| `<Game>/ICON.BMP` | 32×32 menu icon |
| `<Game>/TITLESCREEN.BMP` | 80×64 title screen |
| `<Game>/<NAME>.SAV` | cartridge-RAM file, created once (battery or large RAM) |
| `<Game>/<NAME>.GB` | streamed ROM file (ROMs of 256 KiB and more) |
| `<Game>/.gbrmeta.json` | bookkeeping (ROM sha1, version, `PACK`); ignorable |
| `<Game>/build_flags` | optional project build options, one per line (`#` comments; e.g. `-D META_TBLEND=0` for a game whose stack is too tight — applied to `platformio.ini` `build_flags`, or `EXTRA_CFLAGS` on the Pokitto) |

The icons come from thumbnails.libretro.com (`Named_Titles`): the
catalogue index is cached in `<output dir>/.cache/icons/`, region and
revision tags are ignored for matching, `(World)` then `(USA, Europe)`
are preferred. A local PNG (`<rom dir>/icons/<Game>.png`) takes
precedence; a failure only means no icon. `NO_ICONS=1` disables
downloads. ImageMagick or Pillow converts the PNG to 24-bit BMPs.

There is no ROM size limit: ROMs that do not fit in flash stream from the
card (see above).

Every firmware embeds the version of the edition that produced it
(`gbrecomp-meta x.y.z` / `gbrecomp-pokitto x.y.z` string in the .bin). Each
console has its own version file, bumped independently: the META's lives in
`targets/meta/VERSION` (the release archive's CMake project and zip name take
it), the Pokitto's in `targets/consoles/pokitto/VERSION`. The recompiler
stamps the target console's (`GBL_GEN_VERSION`), `gbrecomp --meta-version` and
`gbrecomp --pokitto-version` print them. Bump a console's version when
the runtime, the frontend or the generated code change what its firmwares
do. `compile.sh` (per `TARGET`) and `tools/meta-compiler` compare it with the string
read back from each .bin (or `.gbrmeta.json` when the .bin has none): a firmware that is current
(same version, same ROM sha1) is kept as is; one produced by another
version is offered for rebuild. On a terminal the script is interactive:
it lists the ROMs with their state (new, current, vX → vY, ROM changed,
unknown version), asks which games to process (`1,3-5`, `a` = all), and
confirms version updates. Otherwise it processes everything that needs
work, keeping outdated firmwares unless `UPDATE=1` (`FORCE=1` redoes even
current ones; current ones only get missing icons).

- Precompute: one random-input run per core (`AUTO_SEEDS`), all in
  parallel at full speed for `AUTO_SECONDS` (20 s) of wall time: 3,000 to
  12,000 frames a second each, so 15 to 60 minutes of game per run. The
  stored input is cut at the last frame played, so a run replays exactly.
  With several ROMs in flight (`JOBS`), their random runs take the cores
  one ROM at a time (`flock`); generation and builds overlap. 20 s is the
  knee measured on the 25-game panel (16 runs, top 2,500 instructions by
  count, scored on four unseen random sessions): the interpreted share
  of 40-minute sessions is 2.36% at 5 s, 1.94% at 15 s, 1.70% at 20 s,
  1.69% at 30 s and 1.85% at 60 s, and on 8-minute sessions it stays at
  1.91% to 1.95% up to 20 s, then rises (2.28% at 60 s: long runs give
  late-game and attract loops more weight). From 8 to 16 runs gains
  little (1.72% → 1.70%).
- Several ROMs run in parallel (`JOBS`, default cores / 4), each with its
  log in `out/<game>/compile.log`. One ROM takes about 45 s on 16 cores;
  a batch takes at least 20 s per ROM (the random runs go one ROM at a time).
- Every run, random or played (`PLAY=1`), is kept in the ROM's profile
  store, `profiles/<gb|sg|gg|sms>/<game>/` at the repository root, shared by
  every target console (`.input`, `.cov`, `.prof`; old
  `<rom dir>/profiles/<sha1>/` stores are moved there), and reused by
  later runs. The store also holds `profile.json` (sessions, ROM map,
  builds, gm0 cycle profiles) and `play.sh`: a recorded gm0 session
  (`gm0 --record-input` reads the buttons the game receives through the
  firmware's `m0input` descriptor in `gbl_core.c`, `sg1000.c`, `gg.c`),
  converted to `coverage-gm0-<date>.input` and replayed by the host harness
  (Sega harnesses read the same `--input` file format as the Game Boy one);
  `compile.sh` replays any session whose coverage is missing. A store with fewer seeded runs than `AUTO_SEEDS` (ROMs from
  the single-run version) gets the missing ones; the old run is kept.
  `AUTO_REDO=1` redoes the random runs, and `REPLAY=1` replays the played
  ones.

A cross-platform Node port lives in `tools/meta-compiler/`:
`node tools/meta-compiler/compile.mjs [rom dir] [output dir]`. Same output
layout, same state files and icon cache (it interchanges compiled games,
profiles and `.gbrmeta.json` with `compile.sh`), same environment
variables plus `--all` / `--update` / `--force` / `--no-icons` / `--play`
/ `--pack` flags. In a terminal it runs a fullscreen interface (ink): a checkable
list of the ROMs with their state, then a dashboard with the games and
their current phase on one pane, global counters and the activity log on
the other (`q` stops launching new games, Ctrl-C kills the running
builds). `ink` and `react` are installed with npm on first run; a
numbered-list fallback needs no dependency. `compile.sh` stays the
reference.

Unique instructions found by random runs:

| Runs | SML | R.C. Pro-Am | Tetris | Mystic Quest |
| --- | --- | --- | --- | --- |
| 1 × 10 min | 4,735 | 8,118 | 3,315 | 10,781 |
| 8 × 10 min | 5,262 | 8,302 | 3,372 | 10,974 |
| 1 × 30 min | 5,578 | 8,221 | 3,374 | 10,802 |
| 12 × 30 min | 5,803 | 8,329 | 3,559 | 10,986 |

Beyond the default, the gain is under 5%, while the flash holds 3,500 to
8,000 compiled instructions.

Mystic Quest (256 KiB, MBC2), with the resident set taken from a
10-minute random-input run:
- 88 KiB of banks 1+ stay in flash and 152 KiB are read from the card;
- over 10 minutes of play with another random input, 27 sectors were read,
  at most 3 in one frame (about 1 ms at an estimated 0.3 ms per sector).

Random input only reaches the name entry and the first fight, so these
figures say nothing yet about the rest of the game. The SD timings are
computed, not measured. `rom_stream` in the host output reports sectors read
per frame for any input.

## Build options

Every optimisation switch has its value per console in
`runtime/gbl_profile.h` (the META and Pokitto profiles): edit it to change
a console's default, or pass a `-D` flag for one build (`build_flags` in
`platformio.ini`, `EXTRA_CFLAGS=` for the Pokitto Makefile, `--cflag` for
`m0_estimate.py`, `CMAKE_C_FLAGS` for the host). Each switch is
independent; `gbl_options.h` stops the build if a profile leaves one out,
turns the Thumb-1 ones off on other compilers and resolves dependencies
(for example `GBL_RENDER_LINE_ASM` needs `GBL_PPU_ASM` and
`GBL_LINE_SIGNATURE`). The table gives the META values; the Pokitto
profile differs in `GBL_LOOKUP_CACHE` 256, `GBL_STREAM_SECTORS` 8 (both with
`POKITTO_RAM_BANKS`) and its own `POKITTO_LCD_ASM`, `POKITTO_AUDIO_ASM`,
`POKITTO_RAM_BANKS`. Each Pokitto switch turned off alone was built and
run (same picture in Link's Awakening).

| Define | Default | Effect |
| --- | --- | --- |
| `GBL_PPU_BATCH` | 8 | Scanlines per PPU event when no per-line STAT interrupt is enabled; 1 means one event per line. VRAM/OAM writes render pending lines first. |
| `GBL_LINE_SIGNATURE` | 1 | Signature of each line's inputs (registers, map row and tile versions, sprites), with per-row tile hashes cached |
| `META_LINE_SKIP` | 1 | Display rows whose signature did not change are neither rendered nor sent; changed rows are sent as runs of consecutive rows, each with its own address window. This is exact. |
| `GBL_FRAME_SIGNATURE` | 1 | Per-frame signature |
| `META_STATIC_SKIP` | 1 | Frames that cannot differ skip even the line checks; a change then shows up one frame late |
| `GBL_PPU_PAL32` | 0 | Converts pixels four at a time with a 2 KiB table (gm0's `pal32`); measured no faster |
| `GBL_GEN_OPTIMIZE_SIZE` | 1 | Compiles generated code with `-Os` |
| `GBL_SPIN` | 1 | Fast-forwards the wait loops the generator marked; 0 runs every iteration (identical results, for checking) |
| `GBL_INTERP_DELAY_LOOP` | 1 | Interpreter shortcut for `DEC A / JR NZ,-3` (not in coverage or trace builds) |
| `GBL_PPU_ASM` | 1 on Cortex-M0+ | Tile fetch and colour conversion in hand-written Thumb-1 (`gbl_ppu_asm.S`); the C loops are used on other targets |
| `GBL_PPU_RAMFUNC` | 0 on the META, 1 on the Pokitto | Runs those loops from SRAM (about 220 bytes, copied with `.data`), with no flash wait state: 3.9% of the CPU instead of 4.8% at 48 MHz on the META, where it faulted with the CPU overclocked |
| `GBL_CORE_ASM` | 1 on Cortex-M0+ | `gbl_rd`, `gbl_wr`, `gbl_push`, `gbl_pop`, `gbl_cp` and the OAM DMA copy in Thumb-1 (`gbl_core_asm.S`) |
| `GBL_TIMING_ASM` | 1 on Cortex-M0+ | PPU line state machine (`gbl_ppu_advance`), next event (`gbl_update_evt`), `gbl_sync`, the PPU register paths of `gbl_io_rd`/`gbl_io_wr` and the dispatch-cache hit of `gbl_lookup` in Thumb-1 (`gbl_timing_asm.S`) |
| `GBL_INTERP_ASM` | 1 on Cortex-M0+ (not in coverage or trace builds) | Interpreter loop and common opcodes in Thumb-1 (`gbl_interp_asm.S`); DAA, EI, RETI, HALT, STOP, PUSH/POP AF, SP arithmetic and `LD (nn),SP` go through the C `gbl_interp_op` |
| `GBL_RENDER_LINE_ASM` | follows `GBL_PPU_ASM` | `render_line` (sprite selection, line signature, frontend calls) in Thumb-1 (`gbl_ppu_asm.S`); needs `GBL_LINE_SIGNATURE` |
| `GBL_LOOKUP_CACHE` | 64 | Entries of the dispatch cache, a power of two (0 disables it) |
| `GBL_STREAM_SECTORS` | META: 2 with ROM and cartridge RAM on SD, 4 with >512 B of resident cartridge RAM, otherwise 8 | ROM sector cache (512 bytes plus metadata per entry). Donkey Kong Land over 3,000 frames: 764 reads with 4, 400 with 6, 217 with 8. Check the linked stack budget before increasing it. Pokitto defaults are separate. |
| `GBL_ERAM_ON_SD` | 1 above 2 KiB of cartridge RAM | Cartridge RAM kept in the save file behind `GBL_ERAM_CACHE_SECTORS` write-back sectors |
| `GBL_ROM_PACKED` | set by `--meta-pack` | Streamed ROM sectors LZSS-compressed in flash instead of on the SD card |
| `GBL_UNPACK_ASM` | 1 on Cortex-M0+ | Thumb-1 decoder for those sectors |
| `GBL_STACK_GUARD` | 1 | C stack check on entry to every compiled region |
| `META_SWAP_AB` | 1 | META A = Game Boy B, META B = Game Boy A |
| `META_SD_SPI_HZ` | 18000000 | SPI clock for SD sector reads (identification stays at 250 kHz): 17.5 MHz with the SERCOM on the PLL at 70. 24 and 35 MHz found no data token on a META. `META_SD_CRC` 1 checks each block's CRC16 (`meta_sd_bus.cpp`; a failed read halves the rate down to 12 MHz and reads again), for trying faster rates or other cards; 17.5 MHz read SML2 without a CRC error. `meta_sd_spi_hz`, `meta_sd_read_errors`, `gbl_save_reads` can be read (or the rate set) with `tools/meta-usb.py` |
| `META_BUTTONS_SPI_HZ` | 12000000 | SPI clock for the buttons' shift register, the library's. `META_BUTTONS_PL_US` (1) is the wait after CS low for the register's parallel load |
| `META_LCD_BGR` | 0 | BGR order in the ST7735's MADCTL; 0 on the META tested (with 1, red and blue were swapped) |
| `GBL_ERAM_CACHE_SECTORS` (window) | — | With the cartridge RAM on the card, `gbl_rd`/`gbl_wr` (C and assembly) read the newest cached sector directly, and write it once it is dirty, without `gbl_rd_slow` and `eram_sector`. Estimator on Super Mario Land 2 (2,400 frames into level 1, same pixel and state hashes): 593k → 497k cycles a frame, frames over the 60 Hz budget 455 → 27. Host, four sessions (539,000 frames): cartridge RAM writes / reads with 8 sectors 12,899 / 34,330, 9: 7,833 / 27,725, 10: 3,066 / 4,820, 12: 2,450 / 2,882 |
| `GBL_ERAM_CACHE_SECTORS` | 8 with the ROM and the cartridge RAM on the card (the ROM cache then gets 2), else 2 | Write-back cache of cartridge RAM kept in the save file (`GBL_ERAM_ON_SD`): Super Mario Land 2 uses its RAM as work RAM (about seven sectors); 2 sectors gave 157 card writes a second, 8 reduced them to about 4/s in the measured run. Exact LRU with no timestamp wrap; deferred flush occurs 120 frames after the last change |
| `META_USB_MODE` | 3 | 0 none, 1 serial, 2 SD, 3 serial and SD. The SD menu action appears only with bit 2. Serial runs while playing; SD takes the card exclusively until reset. Legacy `META_USB=0/1` maps to mode 0/3 when `META_USB_MODE` is unspecified. `compile.sh RELEASE=1` selects SD-only mode 2. |
| `META_USB` | derived | Legacy serial flag: 1200-baud touch for sam-ba and `tools/meta-usb.py` debugging during play. `META_USB_MODE` takes precedence when set. |
| `META_MERGE_MODES` | 1 (0 when streaming) | BLEND and DARK display modes (1 KiB table, a spare line); TBLEND then blends two pixels per lookup |
| `META_TBLEND` | 2 with a ROM cache or cartridge RAM on SD, otherwise 1 | TBLEND, the default mode, same picture in 1 and 2. 1 keeps the 15 held lines (600 bytes) so each is rendered only when it changes; 2 keeps one, in SMOOTH's buffers, and renders the 15 held lines on every drawn frame (frames that did not change at all are still skipped): 640 bytes less on Super Mario Land 2 (2,576 → 3,216 bytes free above `.bss`), CPU cost not measured on the device. Without `META_MERGE_MODES`, a 16-colour table (one lookup per pixel) instead of the 1 KiB one. 0 leaves TBLEND out (TILE instead), 64 bytes less than 2 |
| `META_STACK_RESERVE` | 640 (Pokitto 768) | C stack kept below `gbl_stack_limit`, for what generated code calls there unguarded. Static worst case on the META (frames and call graph of the SML2 firmware): a slow-path read that renders a line, about 390 bytes with the region's frame, plus the USB and DMA interrupts nested, about 220. In streamed builds the option menu (about 590 bytes) runs on the ROM cache's buffers, emptied afterwards: in Super Mario Land 2 every VBlank comes deep in the game's calls. The menu shows the stack never used since boot ("pile", bytes above `.bss`) |
| `META_LCD_12BPP` | 0 | ST7735 in RGB444 (COLMOD 03h): `M0_COLOR` gives 0x0RGB, each row is packed in Thumb-1 as it goes out (`meta_lcd_pack_asm.S`), 240 bytes per row instead of 320. meta-emu-sdl estimate on Super Mario Land: display waits 8.5% → 1.3% of the CPU, audio restarts 7 → 1 per minute |
| `META_AUDIO_DMA` | 0 | The output path, chosen at **build** time since 0.10.29 (the run-time switch of 0.10.28 and the menus' Audio line are gone: 0.5% of the CPU measured on Pokémon Yellow were not worth the DMA path's restart logic and descriptor history): 1 the samples go to the DAC by DMA (TC4-triggered DMAC channel 0, one descriptor per block, the next started by the channel's transfer-complete interrupt) instead of the 22 kHz TC4 interrupt. compile.sh `AUDIO_DMA=1` sets it. No linked descriptors: SAM D21 erratum 1.7.2 (silicon A to F; the META tested is D) — while a channel with linked descriptors runs, enabling another may fetch a wrong descriptor. With the audio chain linked and the display enabled at every Game Gear line, the DMAC wrote audio descriptors over unrelated RAM (HardFaults in Ristar, 4 in 3 minutes); with the channel numbers swapped the display's DMA also stopped mid-strip until the watchdog |
| `META_AUDIO_RING` | 512 | Samples of the audio ring, 2 bytes each (a DAC value: the DMA reads it directly), at least 512 (one emulated frame). Until 0.10.28 the two paths had rings of their own, chosen at build time: the interrupt's `META_AUDIO_IRQ_RING` (1024 one-byte samples, 512 on the Game Gear) and the DMA's (768 samples of 2 bytes) |
| `META_LCD_RUNS` | 1 | The display window reaches the last row: strips that follow the previous one go on as data of the same RAMWR, and the window is set again only for a strip that does not follow and after the bus was released (each frame's end, a card or pad access). Ristar (Game Gear, one window per line before), device, 2400 frames unpaced: 55.69 s → 53.15 s (-4.6%), frames over 25 ms 355 → 157. 0: a window per strip |
| `META_LCD_SPI_MHZ` | 48 | Display SPI clock at boot: 12, 24, 36 or 48. 48 is the maximum the clock tree gives (shown fine on a META), 35 with the CPU at 70 MHz, the default (reduced by `meta_clock.cpp`, re-clamped at every clock change — the menus' SPI ecran line of 0.10.28 is gone), 24 when the PLL does not lock. A panel that does not take it: rebuild with `-DMETA_LCD_SPI_MHZ=24` (or 36) in build_flags. A full frame 40,960 bytes: 13.7 ms at 24, 9.1 at 36, 6.8 at 48. The Arduino core capped every rate at 12 MHz, so firmwares built before 2026-10-03 sent the picture at 12. |
| `META_LCD_DMA` | 1 | Strips are sent by DMA through a queue: the DMA-complete interrupt programs the next window and starts the next strip, so rendering waits only when every buffer is in flight. At VBlank, if the bus is still busy, the button read is postponed by one frame (never two in a row). 0 writes pixels from the CPU. |
| `META_LCD_BUFFERS` | 2 with a ROM cache or cartridge RAM on SD, otherwise 3 | DMA strip buffers |
| `META_STRIP_LINES` | 2 | Number of rows per DMA strip, not an on/off flag. Saves 1,280 bytes with two buffers compared with 4; more transfers/IRQs. The menu slices its text bands to fit the scratch rows. |
| `META_DISPLAY_COMPACT` | 1 | GB META display layouts in flash and line-valid flags in an 18-byte bitmap. Set 0 for the original RAM arrays; pixels are unchanged. |
| `META_MAX_SKIP` | 3 | Maximum run of frames skipped for speed |

### GB META RAM options (0.10.16)

The three options are independent; default display settings use two-line
DMA strips and TBLEND, with compact metadata enabled. To enable all RAM savings,
put these lines in the game's `build_flags` and rebuild:

```
-DMETA_STRIP_LINES=2
-DMETA_TBLEND=0
-DMETA_DISPLAY_COMPACT=1
```

Set `META_STRIP_LINES=4` for four rows per DMA strip, `META_TBLEND=1`
to enable TBLEND, or `META_DISPLAY_COMPACT=0` to restore RAM metadata.
No additional menu buffer is allocated: it
renders vertical glyph slices into the existing scratch area. Compact
metadata preserves the image; turning off TBLEND selects TILE instead.
Short strips trade RAM for more transfers/interrupts. On the synthetic streamed
GB fixture, all three options save 2,384 bytes at the ARM GCC link; setting
`GBL_STREAM_SECTORS=4` then leaves 2,232 bytes for the stack with eight ERAM
sectors. This is a per-build RAM budget, not a device performance measurement.

Super Mario Land 2 (0.10.19, measured on a META in level 1): its
cartridge RAM is work RAM spread over about 5 KiB. With 8 sectors the
card saw up to 100 RAM reads and 49 writes a second; with 10, 3.4 and 2.
Smaller cache lines do not help (host, same 4 KiB: 256-byte lines halve the
traffic, 128 and less raise it; 10 × 512 bytes cut it by 86 % for reads,
76 % for writes). Its `build_flags`:

```
-D GBL_STREAM_SECTORS=3
-D GBL_ERAM_CACHE_SECTORS=10
-D GBL_LOOKUP_CACHE=32
```

1,856 bytes stay above `.bss` for the stack; 424 were never touched after
playing and opening the menu (`GBL_LOOKUP_CACHE` 32: +13 cycles a frame on
the estimator).

Since 0.10.17 streamed and SD-RAM builds default to `META_TBLEND=2`
(TBLEND with one held line), which gives back most of `META_TBLEND=0`'s
gain while keeping the picture. Super Mario Land 2 (2 ROM + 8 RAM
sectors), ARM GCC link, free RAM above `.bss`: 2,576 bytes with
`META_TBLEND=1`, 3,216 with 2, 3,280 with 0; with 2 and
`GBL_STREAM_SECTORS=3`, 2,684; with 4, 2,168. On the host harness
(four recorded sessions, 539,000 frames) the ROM cache reads per frame:
2.38 with 2 sectors, 1.95 with 3, 1.72 with 4, 0.63 with 8; frames reading
more than 10 sectors: 5,803 / 4,362 / 3,333 / 1,798 out of 144,870 in one
session. The ERAM writes are unchanged with 8 ways (7 ways: ×11).
These display options apply to the GB META frontend, not the Pokitto renderer
or the Sega frontends.

Reserve stack space before increasing `GBL_STREAM_SECTORS` (ROM cache) or
`GBL_ERAM_CACHE_SECTORS` (cartridge RAM cache). The fixed lower ROM bank 0
already bypasses the SD cache; the streamed MBC5 upper slot now also maps
bank 0 straight to its resident flash copy. Cartridge RAM sector 0 remains
in the LRU: it contains mutable data and is unrelated to that ROM bank.

`META_LCD_SPI_MHZ`, `META_SD_SPI_HZ`, `META_BUTTONS_SPI_HZ`, `META_LCD_DMA`
and `META_LCD_12BPP` are defined in `runtime/gbl_board.h` only: `gbl_profile.h`
includes it, and so does the profile the SG-1000/Game Gear generator writes,
so one file holds the META's board settings for all three firmwares.

The CPU/runtime optimization comparison kept identical output (SML host `run_hash`,
3,000 frames, two input scripts). The line-signature contract is checked on
every run: on SML 74–79% of lines were unchanged, with 0 false "unchanged"
among about 850k lines. It is also checked by the random-mutation part of
`meta_ppu_matches_per_pixel_reference`.

M0+ estimates on 1,600 SML frames:

| Configuration | Cycles per frame |
| --- | --- |
| No skipping, batched PPU, `-Os` | 396k |
| + pal32 | 397k |
| Static-frame skip only | 293k |
| Line skip only (exact) | 289k |
| Line skip + static-frame skip (default before the optimizations below) | 260k |

Optimizations measured with the same estimator (1,600 frames; SML with its
walk script, the other games with the autoplay input from `compile.sh`, the
same input for both columns; the ROMs are not in the repository):

| Game | Before | After | Change |
| --- | --- | --- | --- |
| Super Mario Land | 264,889 | 212,756 | −19.7% |
| Super R.C. Pro-Am | 394,604 | 257,643 | −34.7% |
| Batman: The Video Game | 366,461 | 297,278 | −18.9% |
| Burai Fighter Deluxe | 119,286 | 80,159 | −32.8% |
| Solar Striker | 334,739 | 245,722 | −26.6% |

The changes involved:

| Change | Main effect |
| --- | --- |
| Wait-loop fast-forward and the interpreter's DMA-wait shortcut | SML −10%, R.C. Pro-Am −26% (polling STAT and a RAM flag) |
| PPU state in one structure, branch-free STAT line, early return in `gbl_ppu_sync` | SML −6% |
| Cycles carried across labels | Generated code −13 to −19%, at +3.6 KiB of flash for the entry switches |
| Thumb-1 tile fetch and conversion, in SRAM | Loops −22 to −25% (about 22 cycles per tile and 27 per 8 pixels, instead of 30 and 35); frame −2.4% |
| No extraction copy when `SCX & 7 == 0` | Small; depends on the game |
| Cached earliest device event; no event update after scroll or palette writes | Batman −5% (raster interrupts) |

A second pass on the runtime, measured on the same generated code (SML,
R.C. Pro-Am, Batman: 212,756 / 257,643 / 297,278 cycles per frame before,
204,098 / 238,600 / 274,564 after, i.e. −4.1%, −7.4% and −7.6%):

| Change | Main effect |
| --- | --- |
| Assembly `gbl_push`, `gbl_pop`, `gbl_rd`, `gbl_wr` (`gbl_core_asm.S`): fast path with no stack frame (GCC saved 2 to 4 registers before testing) | −2 to −3.5% (push/pop), up to −1.7% (rd/wr) |
| Assembly fine-scroll conversion straight from the tile words (`gbl_ppu_convert_fine_asm`, about 32 cycles per 8 pixels instead of 40); sprite priority read from the same words | SML −1.8% |
| Dispatch cache of 16 entries in front of the binary search (`GBL_LOOKUP_CACHE`) | Batman −2% |
| Inline check of the next PPU deadline (`gbl_ppu_due`) instead of a call | Batman −2% |
| Interpreter reads opcode and operands directly when they share a mapped page | R.C. Pro-Am −1.8% |
| Timer and serial event cached apart from the PPU part | Neutral (kept) |

Splitting the PPU transition loop into small functions was tried and
dropped (+1.5 to +4%).

Third pass, gm0-style (tables, pointers, shortcuts), measured at −1.3% on
SML:
- the STAT interrupt sources are precomputed when STAT is written, and the
  LYC match when LY or LYC change;
- sprite selection uses two pointers that slide from one line to the next
  over a Y-sorted copy, instead of a binary search;
- scanline drawing is split: the light decision (sprites, signature) is
  separate from `draw_line`, whose large frame only lines actually drawn
  pay for.

A branchless write of IF was tried and reverted: it rewrote IF three times
per line (+0.5%).

Fourth pass (profile, gm0 comparison, one change at a time, each checked
against the state and picture hashes of five games):

| Change | Effect |
| --- | --- |
| `EI` followed by a loop head: the instruction after `EI` is emitted a second time on the `EI` path, with the delayed interrupt poll after it, instead of polling inside the loop body | The wait-for-flag loop after `EI` is fast-forwarded again: Solar Striker −26% |
| HRAM (`FF80-FFFE`) read and written directly by the assembly `gbl_rd`/`gbl_wr` on the unmapped-page path; `E000-EFFF` (echo RAM) mapped | Solar Striker −8% |
| Sprites drawn from 8-bit masks (`lo \| hi` opaque, clipped once; an 8-pixel window of the "taken" bitmap read from two bytes; eight fixed tests instead of a per-pixel loop); sprite palettes cached per OBP0/OBP1 | `draw_line` −15% on SML |
| Background fetched, converted and hashed only left of the window (gm0's `bgDrawLimitX`) | Lines with a window: only the visible background |

Tried and dropped:
- re-sorting the previous Y order of the OAM instead of sorting from OAM
  order: slower on SML, whose OAM is nearly in Y order;
- taking the start of mode 3 (dot 80) only when observable (made exact after
  a Batman divergence: an LYC edge pending after LCD on). A single PPU
  advance already processes several lines in one loop, so it gained nothing
  and cost STAT reads more;
- inlining the page lookup of `gbl_rd`/`gbl_wr` in generated code: with
  `-Os` GCC does not inline it and adds a call (+3%);
- a larger dispatch cache: `GBL_LOOKUP_CACHE` 128 and 256, measured with
  `m0_estimate.py` (120 frames, generated projects in `out/`) on the two
  ROMs where the interpreter runs most, change the frame by less than 0.1%
  (Kirby's Dream Land, 12.7% interpreted: 363,134 → 362,913 core cycles at
  128; Super Mario Land, 21.8%: 314,771 → 314,780 → 314,797 at 128 and 256).
  The misses are not capacity-driven, so the 64 entries stay.

A 24-byte array initializer in the sprite code had become a `memset` call:
it is now cleared with explicit stores. Every change keeps the state hash identical on five
games (compiled and interpreter-only runs). The assembly is checked with
the pixel hash under the Thumb emulator, against the C versions
(`GBL_PPU_ASM=0`, `GBL_CORE_ASM=0`).

Hand-written assembly pays off in the innermost PPU loops and the small
helpers called by generated code. The rest
of the time is spread across the runtime, the event machinery and the
generated code, where removing work (wait loops, events, flushes) gains
more.

Fifth pass: the event machinery and the per-line code in Thumb-1, with
the loops unrolled where the state allows it. Estimator, 1,600 frames,
generated code and input from `compile.sh` (`coverage-auto1.input` or
`auto.input`, the ROMs are not in the repository); the generated code is
newer than in the tables above, so only the two columns compare.
Each change below is measured against the step before it:

| Game | C runtime (`c9e1bfb`) | Assembly | Change |
| --- | --- | --- | --- |
| Super Mario Land | 137,117 | 101,144 | −26.2% |
| Super R.C. Pro-Am | 257,584 | 215,421 | −16.4% |
| Batman: The Video Game | 272,918 | 218,423 | −20.0% |
| Burai Fighter Deluxe | 77,235 | 37,069 | −52.0% |
| Solar Striker | 222,186 | 185,326 | −16.6% |

| Change | Main effect |
| --- | --- |
| `gbl_ppu_advance` in Thumb-1: a whole line is straight-line code; stage, mode and line start are stored once per call | SML −12.9%, Burai −28.8% |
| The line loop instantiated three times for the STAT line: no source (updates omitted), LYC only (only line changes checked), general | SML −4.1%, Burai −9.0% |
| Whole lines of a skipped frame only counted (about 9 cycles each), the window line moved once by the number of lines at or below WY | Burai −11.0%, SML −2.4% |
| `gbl_sync` with the timer inline (GCC saved nine registers before finding nothing had ticked); `gbl_update_evt` recomputing a dirty event itself | Batman −4.8% |
| The frame signature multiplies by `gbl_sig_mul` (the constant cost a 25-instruction shift/add chain per PPU register write) | Part of the line above |
| OAM DMA resolves its source page once and copies 16 bytes per LDM/STM iteration instead of 160 `gbl_rd` calls (about 4,800 cycles per DMA) | Burai −11.2%, SML −4.4% |
| `gbl_io_rd`/`gbl_io_wr` serve LY, STAT, scroll, palette and window registers in Thumb-1; `gbl_lookup` checks the dispatch cache without a frame | Batman −4.2%, R.C. Pro-Am −1.3% |
| `render_line` in Thumb-1 (sprites sorted as 16-bit keys `x << 8 \| index`), tile-hash loop and `gbl_cp` | −1% to −3% |

Ten more games (`tools/m0bench/panel.txt`: SD streaming, MBC2, raster
effects, many sprites, scrolling) give the same hashes with both runtimes.
With the direct bus paths below included, the change in average cycles
per frame on all fifteen ranges from −14% to −53% (median −19%); the time
freed is 3.9% to 11% of the 60 Hz frame budget (804k cycles at 48 MHz,
emulation only). Heavy frames gain less (p99 −6% to −23%). Frames over
the budget: Prehistorik Man 180 → 33 of 1,599, Donkey Kong Land 24 → 21,
R.C. Pro-Am 16 → 14, Pinball Dreams 6 → 3; in what remains the
interpreter dominates. `m0_estimate.py` prints these per-frame figures
(`--heavy N` profiles the frames over the budget only) and runs streaming
projects: the whole ROM stands in for the SD card (the SD transfer time is
not modelled), and the save file is kept in that region like the host's
in-memory one. (Earlier Zelda figures came from a benchmark that dropped
save-RAM writes: the game crashed into an `RST 38` loop.)

Unmapped accesses no longer go through `gbl_rd_slow`/`gbl_wr_slow`'s six
range tests: the Thumb-1 `gbl_wr` jumps straight to the MBC write,
`gbl_vram_wr` and `gbl_io_wr`, `gbl_rd` to `gbl_io_rd` and, for a streamed
ROM chunk, reads the last sector used; `map_rom_bank` rebuilds the pages
only when the bank changes. Zelda −7.8%, Prehistorik Man −4.3%.

The frames still over the budget spent 40% to 60% of their time in the
interpreter (code the recorded sessions never reached: loading,
transitions). Its loop is now Thumb-1 (`gbl_interp_asm.S`): event check,
fetch, cycles and a 256-entry table of halfword offsets, with the common
opcodes and the CB prefix in assembly and the rest through the C
`gbl_interp_op`, from which the assembly-handled cases are compiled out
(the whole change costs about 0.2 KiB of flash). A transfer to code
that is not compiled starts a new block in place instead of returning to
the dispatcher, with the same block boundaries (the SP bound of the
innermost `gbl_exec` is kept in `gbl_exec_sp0`). Interpreted blocks
averaged three instructions, so the return, the dispatcher loop and the
call cost as much as the loop itself. Every game run entirely
interpreted (`BENCH_INTERP_ONLY`, 800 frames) is 10% to 24% faster with
the same hashes; in normal runs R.C. Pro-Am −2.6%, F-1 Race −5.3%,
Prehistorik Man −3.3%, Donkey Kong Land p99 −15%.

The loop itself (event check, fetch, dispatch) then still took about two
thirds of the interpreter's time, some 66 cycles per instruction against
29 in the handlers (R.C. Pro-Am, `BENCH_INTERP_ONLY`). Now the cycle table
lives in a register, `gbr.cyc` is loaded once, operands are read only by
the handlers that need them, EI branches past the next event check
instead of a flag tested every instruction, and the 64-instruction block
limit is gone (blocks end at a transfer or an interrupt, the C loop too;
the instruction count is published at each event). The dispatch table
also decodes what handlers used to test: one handler per condition
(JR/JP/CALL/RET cc), per ALU operation and operand kind (register, (HL),
immediate, flags inline), register or (HL) for LD, INC, DEC and CB, and
LDH / LD (C) go straight to HRAM or `gbl_io_rd`/`gbl_io_wr`. Flash +0.4
KiB, RAM +4 bytes. Same pixel and state hashes on twelve games;
entirely interpreted (400 frames) −7% to −27% (R.C. Pro-Am, Faceball,
Prehistorik Man, DKL −23%); normal runs −0.1% to −3.9%, p99 up to −8%.
The register offsets and
`gbl_exec`'s bound now share one RAM structure (`gbl_interp_ctx`) that
`r8` points to, which removes two flash literal loads per transfer
(−0.1% to −0.6%).

Tried and dropped, on the same panel: remembering the address range
around the last transfer target with no compiled entry, to skip
`gbl_lookup` (±0.3% in normal runs, +2% interpreted: the range test on
every transfer cost about what it saved); the whole
interpreter in SRAM (no gain, 2.4 KiB of RAM); fast-forwarding interpreted
wait loops (traced on the host: the hottest interpreted loops are the OAM
DMA delay loop in HRAM, which the `DEC A / JR NZ` shortcut already skips,
and memory fills; the one real wait, Altered Space's LY loop, runs about
five iterations per frame).

Measure these with `BENCH_PIXEL_HASH` and `BENCH_STATE_HASH` off: hashing
every line and the machine state costs about 200k cycles per frame in the
benchmark and inflates the frames over the budget (R.C. Pro-Am 31 with
hashing, 1 without). Use the hashes for equivalence, a run without them
for cost. `--callers FUNC` counts a function's calls by caller (also in
the frames over the budget) and `--waits N` lists the functions with the
most flash wait cycles. `--frame-costs FILE` writes every frame's cycles: frames over
the budget near boot are mostly logos and screen transitions, so judge
gameplay on a long run (`--frames 12000`) past its first 3,000 frames.

Without hashing, the dispatcher (`gbl_lookup`, its miss path and
`gbl_exec`) took 3% to 8% of the frame, mostly binary searches after
cache misses: the 32-entry cache missed 24% (R.C. Pro-Am) to 61% (Zelda)
of the lookups, by capacity (other hash functions and 2- or 4-way sets
simulated on recorded lookup streams did not help; size did). Each
bank's dispatch table now carries an index (`gbl_bank_table.idx`, 50
bytes of flash per bank with code): the first entry of each 1 KiB block,
and a bit per 128-byte page holding an entry. A lookup in a page with
nothing compiled, typically the interpreter's transfers inside
uncompiled code, returns at once and is not cached, so it no longer
evicts entries (Faceball's misses drop by 90%); a miss searches one
1 KiB block (about 5 steps instead of 9). The cache has 64 entries
(+256 bytes; 128 entries gain another 0.5 point for 512 more bytes).

The generated PlatformIO project builds with its own copy of the Arduino
Zero variant (`platform/meta/variants/gbl_meta`, LGPL like the original)
without the hardware UART objects `Serial` and `Serial1`, which the META
never uses: −1,160 bytes of RAM and −1.8 KiB of flash. The firmware draws
the same screens in gamebuino-emulator. Most games had less free RAM
than the 3 KiB C stack budget plus the 768-byte interrupt headroom, so
the stack was getting less than 3 KiB.

Since 2026-10-03 the firmware is bare metal: `meta_startup.c` (vector
table, the Arduino Zero core's clock setup, SysTick) and `meta_spi.c`
(SERCOM4 configured as the core's SPI library did, same dividers) replace
the core, its SPI library and its USB stack; the USB serial status line
went with them. Donkey Kong Land (streamed, same code and options):
222,340 → 207,296 bytes of flash (−15.0 KiB) and 30,772 → 28,892 bytes of
RAM (−1,880); Tetris: −14.2 KiB and −1,884 bytes. The RAM goes to the
stream cache (`GBL_STREAM_SECTORS` 4 → 8), the flash to compiled code.
The core also capped SPI at 12 MHz: the bare-metal driver derives every
rate itself (gbl_board.h holds them), the display at 48, the card at 24
under the SD's 25 MHz limit, the buttons at 12.
Tetris and Donkey Kong Land boot and draw the same screens as before in
the `tools/gb-emu` fork; not yet run on a META.

Since the commit before the interpreter work (`c7fd04d`), same pixel and
state hashes on twelve games; per frame without hashing:

| Game | Before | Now | Change | p99 | Frames over the budget |
| --- | --- | --- | --- | --- | --- |
| Faceball 2000 | 178,042 | 162,808 | −8.6% | −15.4% | 32 → 7 |
| F-1 Race | 83,967 | 77,119 | −8.2% | −0.2% | 2 → 1 |
| Altered Space | 68,642 | 63,894 | −6.9% | −18.0% | 8 → 5 |
| Prehistorik Man | 417,143 | 391,875 | −6.1% | −10.8% | 21 → 5 |
| Double Dragon | 167,104 | 157,590 | −5.7% | −6.1% | 14 → 9 |
| Zelda | 250,420 | 236,620 | −5.5% | −4.3% | 0 → 0 |
| R.C. Pro-Am | 204,756 | 195,785 | −4.4% | −11.9% | 7 → 1 |
| Donkey Kong Land | 308,111 | 300,266 | −2.5% | −12.9% | 13 → 5 |
| Mystic Quest | 103,712 | 101,225 | −2.4% | −4.4% | 1 → 1 |
| Batman | 212,085 | 209,939 | −1.0% | −0.3% | 0 → 0 |
| Kid Icarus | 149,687 | 149,155 | −0.4% | −0.1% | 0 → 0 |
| Super Mario Land | 100,272 | 100,038 | −0.2% | −0.1% | 0 → 0 |

(Estimator cycles, 1,600 frames, `BENCH_LINE_SKIP` and
`BENCH_STATIC_SKIP`.)

More runtime code in SRAM does not pay: `gbl_rd`, `gbl_wr`, `gbl_push`
and `gbl_pop` there cost +2% to +4%, because generated code in flash
reaches them through long-branch veneers; the event and IO functions
(`gbl_update_evt`, `gbl_sync`, `gbl_io_rd`/`gbl_io_wr`, `gbl_lookup`)
gain nothing (±0.5%). The PPU loops gain because they run long without
calls back to flash.

The OAM DMA copy (`gbl_oam_dma_asm`, once a frame, about 400 cycles) has
nothing to gain from the SAMD21 DMAC. The audio interrupt had: TC4
interrupted 22,049 times a second, and its handler, entry and exit cost
about 65 cycles, some 3% of the CPU (the estimator does not model
interrupts). With `META_AUDIO_DMA=1` (the default until 0.10.25), each
TC4 wrap triggers a DMAC beat on channel 0 that copies the next ring
entry, already a 10-bit DAC value, to the DAC. Each frame's samples are
one block (two at the ring's wrap), queued; the channel's
transfer-complete interrupt starts the next, and when the queue runs dry
the DAC holds the last sample as the interrupt did; the next frame
restarts the channel after a third of the ring of it. Until 0.10.25 the
blocks were linked descriptors, which met SAM D21 erratum 1.7.2 on the
device (`META_AUDIO_DMA` in the table above). The samples played while the
channel runs are derived from `micros()` (same 48 MHz clock as TC4) and
only bound what can be queued. Measured against the interrupt, the
default since 0.10.26: 0.5% of the CPU on Pokémon Yellow, for about 540
bytes of RAM.

Divisions and ALU helpers (25 games, same hashes, −0.2% to −5.6% per
frame, Kid Icarus −5.6%, Adventure Island −3.5%, Tetris −3.3%):

- `gbl_spin` divided once per skipped wait (285 times a frame on DKL);
  the skip is usually a few periods (to the next PPU mode change), now
  counted by subtraction up to eight periods.
- The APU computed its phase increments with a 64-bit division (about
  1,000 cycles on the M0+) on every frequency write. `gbl_apu_set_rate`
  now keeps `floor(K / rate)`, and an increment is one or two 32-bit
  divisions: `floor(floor(K / rate) / d)` equals `floor(K / (d * rate))`
  (checked for every frequency and NR43 value at seven rates).
- Generated code called `gbl_add` .. `gbl_cp`, `gbl_inc`/`gbl_dec` and
  `gbl_addhl` whenever H was live, which liveness assumes at every region
  exit (a `DEC B / JR NZ` loop computed H each iteration). H is now
  inline: bit 4 (12 for `ADD HL`) of `a ^ v ^ result`, and the result's low
  nibble for INC/DEC (checked against the helpers for every input). About
  +2.7 KiB of generated code on R.C. Pro-Am, which `compile.sh` takes back
  from the least-run regions.

P1 (`0xFF00`) is kept as read in `gbl_joyp`, updated on P1 writes and
`gbl_set_buttons`; generated code reads that byte instead of calling
`gbl_io_rd` (games read P1 several times in a row to let it settle):
−0% to −1.7%, same hashes. Tried and dropped: testing the region's first
address before the entry `switch` (±0.5%); `gbl_ppu_advance` is a large
share only of light games (Burai Fighter 17% of 35k cycles a frame, under
1% of the budget).

The hottest interpreter handlers (LD r,r', LD r,(HL), LDH A,(n), LD r,n,
LD A,(HL+), INC/DEC r, JR cc not taken, ADD/SUB/AND/OR/XOR/CP, BIT) end
with a copy of the loop top instead of a branch to it; the colder
handlers sit before the loop so that all stay in branch range. +0.8 KiB
of flash; entirely interpreted −1.3% to −2.2%, normal runs −0.1% to
−0.4% (p99 up to −1.3%), same hashes.

Ten more games (`panel.txt`: software 3D, isometric, non-identity BGP,
RPG menus, beat 'em up) found three more things: the ALU helpers called by
generated code (`gbl_add`, `gbl_adc`, `gbl_sub`, `gbl_sbc`, `gbl_inc`,
`gbl_dec`) are Thumb-1 now like `gbl_cp` (every input checked by
`BENCH_ALU_CHECK`; −0.5% on Kid Icarus: GCC's were close, the cost is the
calls themselves), the dispatch cache has 32 entries (Faceball 2000 −2%,
frames over the budget 67 → 32; 128 bytes of RAM), and SMOOTH's line check
no longer mixes the display mode into each signature (a mode change voids
them all). On the 25 games the runtime is 9.9% to 52.8% faster than the C
runtime of `c9e1bfb` (median 21.7%), with the same hashes.

All fifteen games against the C runtime of `c9e1bfb`:

| Game | C runtime | Now | Change | p99 change | Frames over budget |
| --- | --- | --- | --- | --- | --- |
| Super Mario Land | 137,117 | 100,256 | −26.9% | −8.7% | 0 → 0 |
| Super R.C. Pro-Am | 257,584 | 207,752 | −19.3% | −20.6% | 16 → 7 |
| Batman | 272,918 | 212,739 | −22.1% | −14.0% | 0 → 0 |
| Burai Fighter Deluxe | 77,235 | 36,483 | −52.8% | −10.7% | 0 → 0 |
| Solar Striker | 222,186 | 186,201 | −16.2% | −6.9% | 0 → 0 |
| Tetris | 88,966 | 52,037 | −41.5% | −21.1% | 0 → 0 |
| Zelda: Link's Awakening | 296,849 | 244,204 | −17.7% | −11.5% | 1 → 0 |
| Donkey Kong Land | 363,432 | 307,845 | −15.3% | −24.0% | 24 → 14 |
| Mystic Quest | 148,991 | 104,404 | −29.9% | −16.6% | 1 → 1 |
| F-1 Race | 130,285 | 85,964 | −34.0% | −6.1% | 3 → 2 |
| Nemesis | 434,194 | 364,454 | −16.1% | −10.5% | 2 → 0 |
| Prehistorik Man | 521,876 | 417,977 | −19.9% | −26.7% | 180 → 21 |
| Ninja Gaiden Shadow | 180,462 | 138,512 | −23.2% | −14.2% | 0 → 0 |
| Pinball Dreams | 321,150 | 261,718 | −18.5% | −17.2% | 6 → 1 |
| Castlevania II | 192,351 | 160,186 | −16.7% | −9.6% | 1 → 0 |

(Cycles per frame, 1,600 frames, 3 KiB of C stack as on the META; frames
over budget out of 1,599.)

The C stack budget changes results. With the META's 3 KiB the stack
guard unwinds compiled calls, and where it does depends on every C
frame size; in fast builds the unwound code then takes interrupts at
other instruction boundaries, so two correct builds can end in different
(valid) states. Batman on the host ends in four different states with
256, 3, 2 and 1 KiB of stack. Equivalence checks of runtime changes
therefore use `BENCH_BIG_STACK` (128 KiB of emulated RAM, no unwinding);
the fifteen games give the C runtime's hashes there.

Every step keeps the pixel, signature and state hashes of the five games
equal to the C runtime's (`BENCH_PIXEL_HASH`, `BENCH_STATE_HASH`; see
Validation). The firmware grows by about 1.2 KiB of flash (the three
copies of the line loop are 0.7 KiB) and 56 bytes of RAM; `compile.sh`
fills the flash by bisection, so a game at the limit keeps slightly less
cold compiled code. Nothing here has been measured on a device.

A cache of decoded or converted tile rows does not fit: one 16-bit word per
tile row is 6 KiB, and the firmware leaves about 3 KiB of RAM, all of it C
stack. Within a line, though, the same tile often repeats (sky, ground,
blank text cells): 50% to 90% of the 8-pixel groups equal the previous one
on most of the 25 panel games, 14% on Prehistorik Man and 31% on Double
Dragon. The tile fetch keeps the last map entry and its decoded word
(11 cycles instead of 22 for a repeat, 25 otherwise). Each colour
conversion keeps the last group with its four output words (14 cycles
instead of 27; the fine-scroll loop copies the previous 16 bytes back,
24 instead of 32). Estimator, 1,600 frames, TBLEND display pipeline
(`BENCH_META_DISPLAY=6`): median −1.0% over the 25 games; Lemmings
−7.2%, Super Mario Land −4.6%, Ninja Gaiden −2.8%, Solar Striker −2.3%,
Batman −2.2%; p99 up to −8.5%; Adventure Island has 17 → 14 frames over the
budget. Games with few repeats lose a little: Prehistorik Man +0.7% (24 →
30 frames over the budget, though those frames are mostly ROM decompression),
Final Fantasy Legend +0.4%. Comparing tile data instead of map entries
(which also catches FFL's identical blank cells) did less on nine of the
games: median −1.3% against −1.6%.
Same pixel and state hashes on six games. SRAM +120 bytes (the loops run
from RAM).

## Validation

```bash
ctest --test-dir build -R meta --output-on-failure
```

- `meta_alu_helpers_match_desktop_runtime` compares every ALU/flag helper,
  exhaustively, with the desktop runtime.
- `meta_ppu_matches_per_pixel_reference` compares 300 random VRAM/OAM/register
  states against a per-pixel DMG reference. The states cover the window,
  8×16 sprites, flips, the ten-sprite limit and priorities.
- `meta_target_end_to_end` generates a synthetic ROM, profiles it,
  regenerates, and requires:
  - the precise build to match the interpreter bit for bit;
  - the fast build to reach identical WRAM results;
  - the generator to recognise the ROM's polling and delay loops, and
    `GBL_SPIN=0` to give the same frames, WRAM and machine state (registers
    and cycle counter at every VBlank) as the default build.
  - a streaming build of the same ROM (forced, with one data chunk in flash,
    one read through a 2-sector cache) to give the same state as the flash
    build;
  - a packed build (`--meta-pack`, 4 KiB raw, 2-sector cache) to give the
    same state as the flash build, with sectors actually decoded (the data
    tables are compressible, so back-references are exercised);
  - the packed build run with 0 to 1024 bytes of C stack (`--stack-bytes`)
    to finish (60 s timeout, catches the dispatcher livelock) with the
    interpreter's frames and WRAM, and 1 KiB to keep the full machine state.
- `m0_fat_finds_streamed_rom` builds FAT16 and FAT32 images with
  `mkfs.fat` and mtools, in the root and in a directory, behind a partition
  table, with a lower-case name and a file in four pieces. It requires the
  file read through `meta_fat.c` to be byte-identical, and a missing file or
  too many pieces to be reported.

The host prints `state_hash`, a hash of the registers, cycle counter, WRAM,
HRAM and IO at every VBlank, next to `run_hash`, which covers the pictures.

The assembly cannot run on the host. Build the estimator image with
`--cflag=-DBENCH_PIXEL_HASH` (optionally with `--cflag=-DGBL_PPU_ASM=0`):
it prints a hash of every rendered line and of every line signature.
`--cflag=-DBENCH_STATE_HASH` adds a hash of the registers, cycle counter,
WRAM, HRAM, IO and PPU timing state at every VBlank (compare with
`GBL_TIMING_ASM=0`, `GBL_CORE_ASM=0`, `GBL_RENDER_LINE_ASM=0`), and
`--cflag=-DBENCH_ALU_CHECK` checks every (A, v) pair of `gbl_cp` against
the C formulas (`alu errors: 0`). On five games (1,600 frames each,
window and 8×16 sprites included), C and assembly print the same hashes.
Compare runtimes with `--cflag=-DBENCH_BIG_STACK` (see above: with 3 KiB
of stack the result depends on C frame sizes). `-DBENCH_INTERP_ONLY` runs
every instruction through the interpreter (compare `GBL_INTERP_ASM=0` and
1), and `-DBENCH_FRAME_HASHES` with `--dump 0x10300000:<4 * frames>` keeps
the state hash of every frame to find where two builds part.
`m0_estimate.py --func NAME` lists every executed block of a function with
its cost.

For a real game, compare the builds on the host with the same input:

```bash
game_host --interp --frames 3000 --input run.input   # reference
game_host --frames 3000 --input run.input            # META build
```

A `--meta-precise` project must print the same `run_hash` as `--interp`. The
fast build may differ only on isolated frames, because interrupts are taken
at slightly different instruction boundaries. `--dump-frame N file.ppm`,
`--wav file.wav` and `--hash-every 1` help narrow down differences.

The firmware also runs in
[gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(load `.pio/build/meta/firmware.bin` in its page; keys: arrows, J = A,
K = B, U = MENU, I = HOME; sound is emulated: TC4 interrupts and DAC
writes are modelled and played through WebAudio; `headless.js` records
them to a WAV with its 5th argument; this needs a firmware built with
`-DMETA_AUDIO_DMA=0`, since the emulator does not run TC4-triggered DMA).
Use the fork in `tools/gb-emu/` (from upstream `9631729`, MIT; see its
`LISEZMOI.md`):
upstream could not execute code from SRAM, where our firmware runs its
PPU assembly (`GBL_PPU_RAMFUNC`) — firmwares crashed on the first
rendered frame — and had no SD card, so streamed games stopped on the
red « ROM introuvable » screen. The fork decodes SRAM lazily and
emulates a SPI-mode SD card backed by a raw FAT image or, built on the
fly, by a local directory (game saves are rewritten into the folder's
files). Streamed games boot and render there too. `headless.js` drives
it from Node and prints the dominant screen colors:
`node headless.js <firmware.bin> [ticks] [batches] <sd.img | directory>`;
the web page gains a file picker for the card image.

To estimate the cost on a Cortex-M0+ without hardware:

```bash
python3 -m pip install unicorn capstone pyelftools   # in a venv
python3 targets/meta/tools/m0bench/m0_estimate.py output/game_meta \
  --frames 600 --input run.input --hot-blocks 5
```

The estimator builds the runtime and generated code bare-metal, with the same
compiler and `-O2`, runs them under Unicorn, and applies the M0+ cycle model
plus flash wait states. It is an estimate for comparing variants, not a
hardware measurement.

## Status (Super Mario Land, measured on the host)

The input script is `targets/meta/examples/sml_walk.input`; the ROM is not part
of the repository.

| Check | Result |
| --- | --- |
| Covered instructions | 3,479, from 1,600 scripted frames |
| Interpreted after compilation | 4% of instructions |
| Precise build vs interpreter | Bit-identical over 3,000 frames (gameplay and attract demo) |
| Fast build vs interpreter | 1–2 frame hashes differ out of 3,000 |
| Frames vs desktop runtime | Same game states for the first 850 frames |
| Firmware size | 160.3 KiB flash (65%), 27.0 KiB RAM with ~5.8 KiB left for the stack |
| M0+ estimate | About 225 emulated frames/s on average with the default options (213k cycles per frame; 185 before the optimizations above) |

The desktop comparison shows a phase difference of one frame. Only the
first 850 frames were compared, because input timing makes the two runs
drift apart after that. The M0+ estimate was not measured on a device.

## Limitations

- DMG only. CGB-only ROMs are rejected, and CGB features are not emulated.
- Supported mappers: none, MBC1, MBC2, MBC3 (without RTC) and MBC5.
  Cartridge RAM lives in the META's 32 KiB, so large RAM carts may not fit.
  PlatformIO reports RAM use. Battery saves go to a `.SAV` file on the SD
  card (see above).
- Timing model:
  - mode 3 always lasts 172 dots;
  - scanlines are rasterized all at once;
  - OAM DMA completes instantly;
  - STOP acts as a NOP;
  - link-cable transfers complete with no partner;
  - sound is quantized to one frame.
- 144 lines onto 128: the default TBLEND merges 15 line pairs and drops
  line 143; streaming builds (TILE) drop 16 lines.
- ROMs of 256 KiB and more need their ROM file on the SD card (see above).
  The SD reader has not run on a device yet.
- Frames are skipped under load, at most `META_MAX_SKIP` in a row.
- In the `tools/gb-emu` fork, plain, packed and streamed builds boot and
  render (streamed ones from an emulated SD card); the upstream emulator
  needs `compile.mjs --emu` builds and cannot run streamed games. Buttons follow the Gamebuino library order (down,
  left, right, up, A, B, MENU, HOME), as the emulator does.
