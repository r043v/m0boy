# m0boy

**Game Boy, SG-1000, Game Gear, Master System, MSX, NES, PICO-8, Vectrex, PC
Engine, Mega Drive and SNES games compiled into native firmwares for small
handhelds (Gamebuino META, Pokitto, ESPboy, and, newer, Gamebuino AKA and
ODROID-GO) and into SDL2 programs for the PC.**

*Version française : [README.fr.md](README.fr.md).*

m0boy is not an emulator. It translates a game's own code into C, compiles
that C for the handheld's processor and links it with a compact runtime that
stands in for the original hardware (video, sound, timers, mappers). Code the
recompiler could not prove or never saw running falls back to a small
interpreter, so every game boots even when only part of it is compiled.

The result of a conversion is a folder to copy onto the console's SD card (or,
on the ESPboy, an image to flash).

| | Gamebuino META | Pokitto | ESPboy |
|---|---|---|---|
| Chip | SAMD21, Cortex-M0+ rated 48 MHz, run at 70 by default | LPC11U68, Cortex-M0+ rated 50 MHz, run at 72 by default | ESP8266, Xtensa 80/160 MHz |
| Memory | 32 KiB RAM, 240 KiB flash for the game | 36 KiB RAM, 228 KiB flash | 80 KiB RAM, 4 MiB flash |
| Screen | 160×128 | 220×176 | 128×128 |
| Storage | SD card | SD card | flash only |
| Runtime version | 0.10.56 | 0.10.40 | 0.3.11 |

Newer targets, built on the shared ESP32 core `targets/common/consoles/esp32/`
(PlatformIO), and the PC window:

| | Gamebuino AKA | ODROID-GO | PC |
|---|---|---|---|
| Chip | ESP32-S3, octal PSRAM, i80 LCD, SDMMC | ESP32-WROVER | SDL2 window |
| Memory | 8 MiB PSRAM, 3000 KiB game slot | 320 KiB RAM, 4 MiB PSRAM | the host's |
| Screen | 320×240 | 320×240 | the source's native resolution |
| Storage | SD card | SD card | the card as an `sd.img` file |
| Runtime version | 0.3.8 | 0.1.0 | 0.2.4 |

The AKA wiring follows the official Gamebuino_AKA_lib (`docs/AKA.fr.md`); it
can be exercised without hardware in `tools/AKA_Emulator` (a real ESP32-S3
emulator) and in gm0 (an experimental application machine). ODROID-GO is not
emulated: its device checks remain manual. Which system may generate for which
console comes from the capability matrix (`gbrecomp --list-systems`,
`--list-targets`).

## What runs where

An experimental PC Engine interpreter generates desktop harness projects
and PC (SDL2) window projects (`--system pce --target desktop|pc`,
`tools/pce/build_pc_binaries.sh` builds a Linux binary and a Windows
.exe with the HuCard embedded, plus `SDL2.dll` and a `README.txt`, under
`output/raiden-pc/`): the full HuC6280 (the mednafen core's
opcode table and cycle counts, the 65C02 set and the Hudson extensions),
the VDC, VCE, timer, pad and a reduced PSG. Raiden boots through its
HuCard menu to the exact title screen and, from a RUN input, plays: the
ship takes off from the carrier, stage 1 scrolls, squadrons attack with
visible fire, and the pad answers. Controls: arrows, X = I, Z = II,
Enter = RUN, Shift = SELECT; a gamepad works too. The CPU budget follows
the HuC6280's CSL/CSH clock switch (the bug that left the game
script-locked) and the `TST #imm,zp/abs` decode (the bug that aborted the
attract demo at its first 8-frame tick and left a RUN started during it
in a script-driven game with a dead pad). The attract demo now plays
like the reference and RUN on the title screen starts a game the pad
controls; the remaining open item is a few pixels of stage scroll
against the reference. The PC window and the headless
harness print the same run_hash, and the .exe's output matches the Linux
build's byte for byte. See [PC Engine notes](docs/PCE.fr.md).

An experimental Vectrex pipeline is available for the META (the system ROM
built into the generated firmware, `--bios`) and the PC target (BIOS read at
run time): the 6809 recompiled to C in front of the interpreter. On a META,
Berzerk's demo runs at 36 frames/s of 50 (measured; 19 with the interpreter
alone), the VIA now being the larger cost. See [Vectrex notes](docs/VECTREX.fr.md).

An experimental Mega Drive M68k recompiler profile generates META, desktop
harness and SDL2 PC projects. The PC project emits covered 68000 opcodes as C
and has an in-tree 68000 fallback, 64 KiB work RAM, VRAM and a Plane A tile
renderer. Sonic 1 reaches Green Hill and responds to movement and jump input in
the native C project. The PC frontend now accepts SDL gamepads, has gm0-style
display filters, and produces experimental Z80/PSG/YM2612 audio (SDL or WAV).
VDP features, FM accuracy and META compatibility remain incomplete. See
[Mega Drive notes](docs/MEGADRIVE.fr.md).

Build local Linux and Windows PC binaries from a locally supplied Sonic ROM
with `tools/megadrive/build_pc_binaries.sh [ROM] [OUTPUT_DIR]`. This requires
SDL2 development files; Windows cross-builds also require the x86-64 MinGW
toolchain and its SDL2 package. The Linux executable and Windows `.exe` plus
`SDL2.dll` are written under `output/sonic1-pc/` by default. These generated
ROM-containing files stay out of Git and release archives; see the Mega Drive
notes for controls and details.

```bash
tools/megadrive/build_pc_binaries.sh \
  'roms/md/Sonic The Hedgehog (USA, Europe).md' output/sonic1-pc
output/sonic1-pc/sonic1-linux
# On Windows, keep sonic1.exe and SDL2.dll in the same folder.
```

Use arrows or a gamepad D-pad/left stick to move. Keyboard X/Z/C or gamepad
B/A/X select Mega Drive A/B/C; Enter or gamepad Start starts. The image is
unfiltered by default. F8 cycles the
gm0-style display filters, F10 changes scaling, and F11 toggles fullscreen.
`--frames N --wav FILE` records 44.1 kHz stereo audio without opening a window.
The 68000 is recompiled to C (regions behind a dispatcher, MC68000 timings);
the YM2612 matches ymfm sample for sample in the tests, the PSG and Z80 share
its clock, and the VDP runs line by line with horizontal interrupts. The sound
has not been compared with a console (see the Mega Drive notes).

| Source | META | Pokitto | ESPboy |
|---|---|---|---|
| Game Boy (DMG) | yes | yes | yes |
| SG-1000 | yes | yes | yes |
| Game Gear | yes | yes | yes |
| Master System | yes | yes | yes |
| MSX1 (ASCII8/16, Konami and Konami SCC mappers; AY-3-8910 and SCC sound; experimental) | yes | yes | — |
| PICO-8 | yes | yes | yes |
| NES (NROM: 16/32 KiB PRG, 8 KiB CHR ROM; experimental) | yes | yes | — |
| PC Engine (HuCards, interpreter; experimental; PC window + harness, no console) | — | — | — |
| Vectrex (cartridges up to 32 KiB, system ROM built in; experimental) | yes | — | — |
| Mega Drive (bounded M68k colour-loop profile; experimental) | yes | — | — |

Capability matrix for the newer targets (from `gbrecomp --list-systems`;
experimental where the rows above say so):

| Source | PC | ODROID-GO | AKA |
|---|---|---|---|
| Game Boy, SG-1000, Game Gear, Master System | yes | yes | yes |
| MSX1, NES | yes | — | — |
| PICO-8, Vectrex, Mega Drive, PC Engine | yes | — | yes (ODROID: no) |
| SNES (experimental, desktop harness and PC window only) | yes | — | — |
| M0+ source (a META or Pokitto firmware, `--system meta`) | yes | — | yes (META only) |

SNES (`--system snes`): the 65816 is recompiled to C with a context walk, with
a per-line PPU/HDMA/IRQ, an SPC700 interpreter and BRR DSP, DSP-1, SuperFX and
SA-1 support in progress. State, proofs and open items, with their evidence
labels, are in `docs/SNES_STATUS_2026-10-31.md` and `docs/SNES.fr.md`.

META and Pokitto firmware `.bin` images are also explicit M0+ source systems:
`--system meta|pokitto --target pc` packages an integrated PC board emulator
derived from gm0 (MIT with its third-party notices retained). Reachable Thumb
instructions are translated to C; dynamically reached code, RAM execution and
modified flash fall back to the interpreter. The board model inherits gm0's
hardware coverage and limitations. AKA firmware execution is not available yet.
See [multi-system notes](docs/MULTI_SYSTEM.fr.md).

The `--target pc` SDL2 backend is available for Game Boy, SG-1000, Game Gear,
Master System, MSX, NES, PICO-8, PC Engine, Vectrex, Mega Drive and SNES (experimental). These projects share gamepad
and joystick hotplug, an unfiltered image by default, F8 display filters,
F10 scaling and F11 fullscreen. Each system keeps its own keyboard button
names and audio core. The NES PC window displays the same 160×128 panel image
as the META; the other PC cores use their own output geometry.

Input recording in the PC windows (NES, PC Engine, PICO-8): each core keeps
the dialect its harness reads with `--input`. NES and PC Engine write
"<frame> <buttons>" lines (names joined with `+`, one line for every frame
the game receives buttons on — the harnesses' scripts are momentary, held
for their one frame); the PICO-8 window writes `at:mask:len` segments. NES
and PICO-8 read environment variables (`M0_RECORD_INPUT=file` records,
`M0_INPUT=file` plays a script in the window); the PC Engine window takes
`--record-input file` and `--input file` on its command line. A window
close, the frame limit (`M0_FRAMES`/`--frames`) and the stuck-boot guard all
close the file; a menu restart or save-state load ends it there (the
harness replays from power-on, what follows could not replay). The harness
replays the file untouched: NES and PC Engine with `--input file`, PICO-8
with `--input-file file`.

MSX1 is checked against openMSX (VRAM of the title screens) and against the
host reference renderer; Gradius 2 (128 KiB, Konami SCC) held 60 game frames
a second on a META in a 90 s hands-on test (frame counter against the
firmware's own clock; displayed frames not measured). The 0.10.41 renderer
and audio pass skips the blank tile and pair blends and the per-sample audio
copies: over 3,600 gm0 frames of Gradius 2 that is 3.4 s less in the tile
routines and 0.8 s less in audio generation (estimates), with the screen and
game RAM identical to the previous build at the same game frame.
Notes: `docs/MSX.fr.md`.

NES: the 6502 code is recompiled to C (the interpreter for the rest), the
2C02 renders per line straight into the META's 160x128 (the 5-in-8 box
filter across, pairs of lines averaged down, 240 -> 128; the loops in
Thumb-1), the APU's four voices are rendered at the driver's 22,049 Hz.
Super Mario Bros. held 60 game frames a second on a META running and
jumping through 1-1 (frame counter read over USB, 2026-10-25; displayed
frames not measured). On the Pokitto the same PPU blends fewer lines
(240 -> 176, rows widened to 200 as for the Sega cores): Super Mario Bros.
runs through 1-1 in gm0 at 59 game frames a second, video frames skipped
to keep that pace (not yet tried on a Pokitto). Estimated cost of a frame
with every frame shown: 0.91 M cycles on the META (from 1.05 M) and 1.21 M
on the Pokitto at 48 MHz (from 1.36 M): runs of sky written as words, the
idle loop spent at once, the PPU's loops in SRAM on the Pokitto. NES now has
ten save-state slots on both handhelds; the host harness checks a resumed
run against an uninterrupted one. Firmware builds and size checks pass;
save/load on a device has not been checked.

Shared by the cores: one vertical plan (source lines onto panel rows,
`m0_scale.h`), one row sender (`m0_rows.h`), idle loops (a jump to itself)
spent at once on the 6502 and the Z80 (`m0_idle.h`), and on the Pokitto
one set of Thumb-1 pixel writes from SRAM for every core (PICO-8, SG-1000
and MSX wrote theirs in C).
Notes: `docs/MULTI_SYSTEM.fr.md` ("Recompilation 6502 (NES)", "Rendu NES
sur le panneau").

The handheld cores share an option menu (hold HOME; C on the Pokitto): resume,
restart, volume, speed, CPU clock, return to the
loader, plus each system's own lines (display modes and palettes on the
Game Boy, USB bootloader on the META). Save-state slots appear where supported.

| Feature | META | Pokitto | ESPboy |
|---|---|---|---|
| Save states | GB, SG, GG, SMS, MSX, PICO-8, NES, Vectrex: file on the card | GB, SG, GG, SMS, MSX, PICO-8, NES: file on the card | GB, SG, GG, SMS, PICO-8: in flash |
| Battery saves | `.SAV` on the card | `.SAV` on the card | in flash, two alternating copies |
| ROM larger than the flash | read from the card, hot sectors kept in flash | read from the card | `rom.bin` in the 4 MiB flash |
| Compressed ROM sectors (LZSS) | yes | yes | yes |
| ROM cache sharing RAM with the C stack | yes | Game Boy | — (no card) |

How much of this was checked, and how, is written next to each claim below and
in the linked notes: **checked** means an automatic comparison or test,
**gm0** means measured in the [gm0](https://github.com/r043v/gm0) emulator,
**device** means measured on a console, **estimate** means the Cortex-M0+
cycle model (`targets/meta/tools/m0bench`).

## Tools

- `tools/meta-usb.py`: flash a firmware over USB and read the console's serial
  line (`monitor` subcommand: a printf terminal for any serial console, with
  reconnection; `--baud`, `--prefix` for the target's binutils; Windows, Linux
  and macOS). SAM-BA is only a protocol spoken over a plain USB-serial line.
- `M0_AKA_PERF` (off by default, no effect on the firmware): the native AKA
  M0+ core prints `[perf]` timings (emulation / sound / screen) on the serial
  line. It is a debug string, not a profiler.
- `tools/profile/` (per-game profiles, `m0search.py` play search),
  `tools/snes_sweep.py`, `tools/AKA_Emulator`, `tools/megadrive/`.

## Getting started

You need CMake 3.20+, Ninja, a host C/C++ compiler, Python 3, the zlib
development files, and [PlatformIO](https://platformio.org/) for the META and
the ESPboy. The Pokitto only needs `arm-none-eabi-gcc` (from `PATH`, otherwise
PlatformIO's). PICO-8 support needs a [femto8](https://github.com/benbaker76/femto8)
checkout next to this one (`FEMTO8_DIR`, default `../femto8`).

```bash
cmake -G Ninja -B build .
ninja -C build

# ROMs in roms/gb/, roms/sg/, roms/gg/, roms/sms/, roms/pico8/ then:
./compile.sh                    # asks for the console (META by default)
TARGET=pokitto ./compile.sh
TARGET=espboy ./compile.sh
TARGET=both ./compile.sh        # META and Pokitto
```

Each game lands in `roms/<console>/<system>/<Game>.zip`. Unzip it at the root
of the SD card and start it from the console's loader; keep the `.SAV`,
`.STA` or `.ST8` files that are already on the card, they hold your progress.
On the ESPboy, the zip holds `firmware.bin` (and `rom.bin` above 256 KiB) with
the `esptool.py` line in `FLASH.txt`.

Useful `compile.sh` switches:

| Switch | Effect |
|---|---|
| `RELEASE=1` | firmwares without the USB debug port (more RAM; reflash through the bootloader) |
| `UPDATE=1`, `FORCE=1` | rebuild games made by another version / rebuild everything |
| `SOURCE=1` | deliver the generated project, ready for `pio run`, instead of the firmware |
| `SRC_SYSTEM=gb\|sg\|gg\|sms\|pico8` | convert one system only |
| `PACK=0` | no compressed ROM sectors (Game Boy) |
| `CACHE_DYN=0` | fixed-size card cache instead of the adaptive one |
| `FIRMWARE_COMPILER=clang` | build the firmwares with Clang |
| `REPLAY=1`, `PLAY=1` | replay your recorded sessions / play one yourself before compiling |

The complete guide is in French: [docs/GUIDE.fr.md](docs/GUIDE.fr.md). The
technical notes are in [targets/meta/README.md](targets/meta/README.md).

Tests:

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

## How it works

### Compilation guided by play

Before compiling a game, `compile.sh` plays it by itself on the host: random
sessions, a session without input (logos, attract mode), and coverage-guided
explorations that keep a snapshot of every state where new code or data showed
up and play on from there. The budgets are set in frames of game time, with a
real-time cap as the guard: the random sessions aim for 1 080 000 frames each
(about five hours of game; the first seed three times more), the no-input
session for 108 000 frames (half an hour, long enough to sit through the
attract demos), and every session stops at 300 s of real time
(`AUTO_SECONDS`) — a conversion spends minutes per game, whatever the host's
speed. The sessions record what code ran, how often, and
which ROM sectors were read. They are kept per ROM in one store shared by
every target console (`profiles/<gb|sg|gg|sms>/<game>/`), so a later
conversion reuses them and a session you play yourself extends them. Each
store also gets `profile.json` (sessions, a map of the ROM's code and data,
the last build of each console, gm0 cycle profiles) and `play.sh`, which runs
the built firmware in [gm0](https://github.com/r043v/gm0). Every gm0 session
is recorded: gm0 notes the buttons the emulated game receives, frame by
frame, the session is replayed in the host harness, and its code joins the
profile for the next conversion; gm0 also gives a per-function cycle profile
(emulator estimates).

A static sweep completes them. It classifies every byte of the ROM (padding,
repeated patterns, pointer tables), then tries speculative runs of
instructions; a run counts as code only when it reaches an unconditional end
without an illegal opcode or an access to nonexistent hardware. The flash is
then filled with compiled code, most executed first, then the swept code the
sessions never reached.

Both mechanisms are shared by every source CPU with coverage: the generated
harnesses expose the explorer as `--explore FRAMES --random SEED`
(`targets/common/host/m0_explore_host.h`), and call/jump targets the
reachability walk cannot prove can be swept on purpose with `--pce-sweep`,
`--nes-sweep`, `--vectrex-sweep`, `--megadrive-sweep` or `--meta-sweep`
(discipline in `recompiler/include/m0boy/sweep.h`; the swept entries only run
under their own banking context, so the `run_hash` must not move). See
[multi-system notes](docs/MULTI_SYSTEM.fr.md).

The generated C keeps the source CPU's semantics: flags are computed only
where a later instruction reads them, cycles are added per straight run,
wait loops (polling a register or an interrupt flag) skip whole iterations up
to the next event. A precise build (`--meta-precise`) synchronises the
hardware before every instruction and is **checked** identical to the
interpreter: Super Mario Land on the Game Boy, all 68 SG-1000 cartridges and
all 225 Game Gear cartridges of a test collection.

### Any ROM size

- A ROM that fits stays in flash next to the runtime and the compiled code.
- Otherwise the least-read 512-byte sectors are compressed with LZSS in flash
  and decoded on demand by a Thumb-1 decoder; the hot ones stay raw.
- When that is still too large, the ROM is read from the SD card through a
  small cache, the sectors the sessions read most kept in flash. There is no
  size limit: Link's Awakening, Pokémon Yellow and Donkey Kong Land convert
  this way.
- Cartridge RAM larger than the console can spare (8 KiB, 32 KiB) lives in a
  `.SAV` file on the card behind a write-back cache.

On the META the card cache shares the free RAM with the C stack at run time:
a fixed part (the one the menu uses) plus blocks taken from the RAM under the
stack. A native call refused for lack of stack gives a block back at once; a
block is taken again only after several seconds without pressure and with a
margin. With 64-byte blocks, Super Mario Land 2's demo ran 13,326 frames in
150 s against 10,985 with the former fixed cache of 128-byte blocks
(**device**), and Ristar's card reads dropped from 9,001 to 4,131 (**device**).

What goes to flash, raw or packed, is chosen from the cost of each access,
not only from how often a sector is read: the host replays every recorded
session through a model of the console's cache (`tools/profile/m0romsim`,
ROM and cartridge RAM, code and data, every access counted) with costs
measured in gm0 (a card sector about 17,250 cycles, an LZSS decode about
11,400; estimates). Cache block copies are fixed-size unrolled Thumb-1 (16 to
512 bytes, every source alignment). Ristar on the META, the same 5,500 frames
replayed unpaced: 133.37 s → 130.82 s, frames over 25 ms 1,657 → 1,149, LZSS
decodes 2,119 → 21 (**device**, 0.10.34 → 0.10.36). On that run the simulated
plan does no better than ranking sectors by read count (130.84 s); the gain
comes from the access path and the measured costs. Details:
`docs/PROFILING_STOCKAGE_2026-10-22.fr.md`.

### Runtime

- One cycle counter drives everything; the video renders a line when the
  program can observe it, timers and interrupts raise events only when they
  become visible.
- The hot paths are Thumb-1 assembly: the Game Boy PPU line state machine and
  renderer, memory access, the interpreter, the dispatch cache, the Game Gear
  background, the SG-1000 tile loops, the LZSS decoder.
- The Cortex-M0+ has no divider: divisions were taken out of the hot paths.
- The META and Pokitto firmwares are bare metal (no Arduino core, no
  PokittoLib); the ESPboy starts on the Arduino core and drives the hardware
  itself.

### Game Boy

DMG games, mappers none, MBC1, MBC2, MBC3 (no clock) and MBC5. On the META
the 144 lines are blended into 128 (several modes, the default keeps text
legible); on the Pokitto the picture is 1:1 or enlarged to 200×176. The Game
Boy Color boot palettes are built in (hold a direction at boot, as on a GBC).

### SG-1000, Game Gear, Master System

The Z80 recompiler handles banked code (an instruction is a page and an
address; code returns to the dispatcher when the mapper moves its page), Sega
and Codemasters mappers, plus Super Game 150's six-window 8 KiB mapper.
The latter uses precise native dispatch; its old Sega-mapped profiles are
refreshed. See [mapper validation](docs/KOREAN150.fr.md).
The TMS9918A and the mode 4 VDP are drawn line by
line when the program touches them; the SG-1000 and Master System pictures are
filtered 8-to-5 onto the small screens. The Z80 core passes ZEXDOC and 66 of
67 ZEXALL tests (**checked**). In gm0 at 48 MHz, 67 of 68 SG-1000 games run at
full speed on the META and the Pokitto, and Alex Kidd in Miracle World,
Shinobi and Wonder Boy III at 97–99 % (**gm0**). No FM sound.

### PICO-8

A cartridge's Lua is compiled to C, one function per Lua function, on a
runtime with a garbage-collected heap and integer drawing and sound. The PC
side uses femto8's cartridge reader and Lua compiler. The picture of the
generated code is **checked** equal to femto8's, byte for byte, on a synthetic
cartridge (also with a collection at every allocation, and across a save
state) and on Celeste and its Practice Mod. Save states hold the heap's absolute
pointers, so a state only loads into the firmware that wrote it, recognised
by a hash of its code. Notes: [docs/PICO8.fr.md](docs/PICO8.fr.md).

```bash
build/bin/m0recomp celeste.p8.png --system pico8 --target meta -o out/celeste
pio run -d out/celeste
build/bin/m0recomp celeste.p8.png --system pico8 --target pokitto -o out/celeste-pokitto
cmake -G Ninja -S out/celeste-pokitto -B out/celeste-pokitto/build -DCMAKE_TOOLCHAIN_FILE=out/celeste-pokitto/arm-toolchain.cmake && ninja -C out/celeste-pokitto/build
```

### ESPboy

ESP8266 (ESP-12F, 4 MiB flash), 128×128 screen, no SD card. A ROM of up to
256 KiB is part of the firmware; a larger one keeps its first bank there and
the rest goes into `rom.bin`, flashed at `0x100000`. Battery saves (two
alternating copies) and save states live in the flash from `0x3C0000`. The
emulation runs on a 12 KiB stack in RAM, which gives back what the heap needs
to keep 4 KiB free. Checked by host tests (a mock flash included), Xtensa
builds and gm0 runs; the current version has not been validated on an ESPboy.
Status: [docs/ESPBOY_STATUS_2026-10-19.md](docs/ESPBOY_STATUS_2026-10-19.md).

## Without a console

[gm0](https://github.com/r043v/gm0) runs these firmwares: the META and the
Pokitto instruction by instruction, the ESPboy's own Xtensa code with its ROM
and SDK emulated by function. `gm0 Game.zip` takes a converted game as an SD
card; `EMU_INPUT` scripts buttons (`frame:keys:frames,...`).

## Limitations

- Game Boy Color-only games are rejected; CGB features are not emulated.
- Simplified Game Boy timing: mode 3 always lasts 172 dots, OAM DMA is
  instant, sound is quantised to a frame, no link cable.
- Code the sessions never reached runs in the interpreter, much slower: a
  session you record through that part fixes it.
- SG-1000: unbanked cartridges up to 48 KiB, NTSC, no SC-3000 keyboard.
  Master System: no FM sound. ESPboy: Game Gear cartridge RAM is 8 KiB.
- PICO-8: cartridges must fit the heap (18 KiB on the META and the Pokitto,
  22 KiB on the ESPboy).
- Frames are skipped when the console falls behind.
- Performance figures marked **estimate** or **gm0** are not device
  measurements.

## Repository layout

| Path | Content |
|---|---|
| `recompiler/` | `m0recomp` (also `gbrecomp`): Game Boy backend, Z80 recompiler (`src/system/sg1000/`), PICO-8 front end (`src/system/pico8/`), ROM storage planning |
| `targets/meta/runtime/` | the Game Boy runtime shared by the three consoles; per-console switches in `gbl_profile.h` |
| `targets/common/platform/` | the frontend every console shares (`m0_*`: menu, saves, states, SD card, FAT, pad) |
| `targets/meta/platform/` | the Game Boy frontend (`gb_main.cpp`, saves, streaming, display modes) |
| `targets/consoles/` | each console's bare-metal drivers and project templates |
| `targets/sg1000/`, `targets/gamegear/` | SG-1000 and Game Gear/Master System runtimes, frontends, host harnesses |
| `targets/pico8/` | PICO-8 runtime, frontend, host harness |
| `targets/common/` | ROM codecs, card cache, stack/cache pool, play explorer, shared platform (`platform/`), build toolchain |
| `targets/meta/host/`, `targets/meta/tools/` | host harness, cycle estimator |
| `compile.sh`, `tools/meta-compiler/` | batch conversion (shell, Node) |
| `tests/`, `targets/meta/tests/` | tests |
| `targets/common/consoles/esp32/`, `targets/consoles/{aka,odroid,pc}/` | shared ESP32 core and the AKA, ODROID-GO and PC drivers |
| `targets/{nes,pce,vectrex,megadrive,snes,msx,m0plus}/` | the other cores' runtimes and harnesses |
| `tools/` | `meta-usb.py`, profiles and play search, AKA emulator, sweeps |
| `docs/` | French guide, development notes |

## Written by AI agents

No line of what m0boy adds to its starting point was written by hand. The
code, tests, drivers and documentation were written by AI agents guided by
r043v, who tested on the consoles and made the choices. Claude (Claude Code)
wrote most of it; GLM (ZCode) wrote the web converter, the GBC palettes,
Pokitto fixes and the gm0 emulator; DeepSeek (OpenCode) worked on the buses
and merged the Sega backends' shared code. Figures for the first ten days
(29 September – 9 October 2026), from the transcripts and the git history:
Claude, 67 commits and 42,375 added lines (95 %); GLM, 26 commits and 2,207
lines (5 %).

m0boy started as a port of
[gb-recompiled](https://github.com/arcanite24/gb-recompiled) by arcanite24, a
Game Boy recompiler for desktop computers, and keeps its history (104
commits). The desktop runtime, ports and test folders that held converted
commercial ROMs were removed; the emulator m0boy grew alongside is now
[gm0](https://github.com/r043v/gm0).

## Contributors

- **r043v**: author and maintainer; tests on the consoles and makes the choices.
- **arcanite24**: [gb-recompiled](https://github.com/arcanite24/gb-recompiled),
  the starting point.
- **Jean-Charles LEBEAU (Jicehel)**: the Gamebuino AKA work. Their
  [fork](https://github.com/Jicehel-Aka/m0boy) and pull request #2 brought the
  AKA M0+ `[perf]` timings (now behind `M0_AKA_PERF`), the Mega Drive fixes
  (version read from the header, 24-bit SP/PC) and an AKA serial console idea,
  which led to the cross-platform `monitor` of `tools/meta-usb.py`.
- Other commit authors in the history: Brandon G. Neri, HDR, Majora,
  RedReaper1515, squach90.

## ROMs and rights

No ROM and no built firmware are provided, and none may be added to this
repository. Convert only games you own, or homebrews whose licence allows it.

## Licence

MIT, see [LICENSE](LICENSE). Based on
[gb-recompiled](https://github.com/arcanite24/gb-recompiled), © 2026
arcanite24, MIT licence. `tools/gb-emu` keeps its own MIT licence (© 2017
Andy O'Neill).
