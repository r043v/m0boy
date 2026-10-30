# Working on m0boy

m0boy recompiles Game Boy ROMs into native firmwares for the Gamebuino META
(SAMD21) and the Pokitto (LPC11U68). Start with `README.md`, then
`targets/meta/README.md` (technical notes) and `docs/META_STATUS_2026-09-30.md`
(state, evidence labels, open items).

## Layout

- `recompiler/include/m0boy/`, `recompiler/src/system.cpp`: source CPU/system
  registry and console-independent byte reader. `--system` is the source
  console; `--target` is the destination. See `docs/MULTI_SYSTEM.fr.md`.
- `recompiler/src/cpu/sm83/`: SM83 decoder, independent of the GB cartridge
  parser. `recompiler/src/system/gameboy/`: GB cartridge, bank mapping and
  analysis. Legacy `recompiler/rom.h` and `recompiler/decoder.h` are forwarding
  includes. `recompiler/src/cpu/z80/`: Z80 disassembly, sharing encoding
  helpers with SM83 in `include/m0boy/cpu/encoding.h`.
  `recompiler/src/system/sg1000/`: unbanked cartridge inspection, Z80 -> C
  recompiler (`emitter.cpp`, regions as in the META backend) and project
  generation. `targets/sg1000/runtime/`: runtime shared by recompiled code and
  the fallback Z80 interpreter (event timing, lazy TMS9918A rendering, SN76489,
  `sg_gen.h`); `targets/sg1000/platform/`: META/Pokitto frontend.
- SG-1000 rule: a `--meta-precise` project must print the same `run_hash` as
  `--interp` (host harness), and `out_mismatch=0` (device renderer = reference).
- MSX (`--system msx`): the SG-1000 runtime built with `SG_MSX` (slots through a
  table of 8 KiB windows, PPI, AY-3-8910 in `msx_ay.c`, BIOS simulated in C by
  `OUT (n),A` traps in `msx.c`; layout rule in `targets/common/runtime/m0_msx_layout.h`).
  `tests/test_msx_e2e.py` (synthetic cartridge `tests/msx_fixture.py`). The SG-1000
  firmware must stay identical byte for byte. Notes: `docs/MSX.fr.md`.
- PICO-8 (`--system pico8`): `recompiler/src/system/pico8/p8c.c` compiles a
  cartridge's Lua (femto8's reader and compiler, `FEMTO8_DIR`) to C;
  `targets/pico8/runtime/` (values, tables, collector, drawing, sound, API in
  `p8_natives.def`), `targets/pico8/platform/p8_main.cpp` (META/Pokitto/ESPboy
  frontend),
  `targets/pico8/host/p8_host.c` (harness). Rule: the screen must equal
  femto8's (`tests/test_pico8_e2e.py` holds a recorded hash; compare Celeste
  against a femto8 built with scripted input and no sleeping when touching the
  runtime), also with `-DP8_GC_STRESS`. Notes: `docs/PICO8.fr.md`.
- `recompiler/src/system/console_project.cpp`: what the SG-1000, PICO-8 and
  NES generators write alike for a console (shared frontend, audio driver,
  `gbl_config.h`/`gbl_profile.h`, META and Pokitto build projects).
- `recompiler/`: `gbrecomp`, from gb-recompiled. The M0 backend is
  `recompiler/src/codegen/meta_emitter.cpp` (`--target meta`, `--target pokitto`).
- `targets/meta/runtime/`: the `gbl` runtime shared by both consoles; every
  optimisation switch, per console, is in `gbl_profile.h` (one switch, one
  define, independent).
- `targets/common/platform/`: the frontend every console shares (option
  menu, save states, FAT, SD protocol, pad decoding, rows to the panel in
  `m0_rows.h`); console facts in `m0_target.h`.  Common files are named
  `m0_*`, never `meta_*`.  Vertical plans (source lines onto panel rows)
  follow `m0_row_of`/`m0_row_first` in `targets/common/runtime/m0_scale.h`;
  idle loops (a jump to itself) are spent at once through
  `targets/common/runtime/m0_idle.h` (NES and Z80 runtimes); the Pokitto's
  pixel writes are one Thumb-1 macro in `pokitto_lcd_asm.S`, used by every
  core.
- `targets/meta/platform/`: the Game Boy frontend of its own (`gb_main.cpp`,
  `gbl_save`, `gbl_stream`, `gbl_display`, `gbl_palettes`).
- `targets/consoles/meta/`, `targets/consoles/pokitto/`, `targets/consoles/espboy/`: bare-metal
  drivers and project templates of each console.
- `targets/meta/host/`, `targets/meta/tools/m0bench/`: host harness and the
  Cortex-M0+ cycle estimator.
- `compile.sh`, `tools/meta-compiler/`: batch conversion (shell and Node).
- `tools/profile/`: the per-game profile store (`m0profile.py`, `play.sh`,
  `m0romsim.cpp`) and `m0search.py`, the generic play search shared by every
  core's harness (`--explore FRAMES --random SEED` coverage-guided sessions
  through `targets/common/host/m0_explore.h` + `m0_explore_host.h` — every
  coverage core has one: GB checkpoints at its runtime safe points, SG-1000
  plays whole segments; PICO-8 stays scripted — plus generated input
  scripts, `--input` replays, merged coverage for the generator). Bounded
  sweeps of call/jump targets the reachability walk cannot prove:
  `--pce-sweep`, `--nes-sweep`, `--vectrex-sweep`, `--megadrive-sweep`,
  `--meta-sweep`, discipline in `recompiler/include/m0boy/sweep.h`.
- M0+ source (`--system meta --target pc`, `recompiler/src/system/m0plus/`,
  `targets/m0plus/runtime/`, `docs/MULTI_SYSTEM.fr.md`): Thumb -> C entries
  (`m0_native_glue.h` is their vocabulary over `M0Cpu`), `m0_cpu.c` as fallback,
  `m0_machine.c` (SysTick/NVIC), `samd21.c` (the META board), frontend
  `targets/m0plus/platform/m0_pc.c`; no gm0 in the generated project (Pokitto
  still uses gm0's copy). Rule: `m0_headless --frames N` prints the same
  `run_hash`/`image_hash`/`audio_hash` as `--interp`
  (`tests/test_m0plus_e2e.py`); the profile is `--profile prefix`
  (`.cov` for regeneration, `.mmio`, `.hot`). Against gm0 (Lapinou) the image
  and the DAC value sequence match; cycles differ slightly (no NVM wait model).
- `tools/gb-emu/`: gamebuino-emulator fork used for profiling runs.

## Build and test

Always use CMake with Ninja.

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

`meta_pokitto_target` also links the Pokitto firmware when
`arm-none-eabi-gcc` is available (PATH or PlatformIO's toolchain).

## Rules for generated behaviour

- A `--meta-precise` project must print the same `run_hash` as `--interp`
  for the same input; the PlatformIO build (`pio run -d <project>`) must
  still fit the RAM/Flash report.
- A change to the shared frontend must keep both consoles building. When it
  is meant to be META-neutral, compare `firmware.bin` with a build from the
  previous sources.
- Runtime changes touching dispatch, streaming or `--meta-pack` rely on the
  e2e packed build and its C-stack sweep (`--stack-bytes`, livelock timeout).
- Assembly paths cannot run on the host: compare them with the C versions on
  the estimator with `--cflag=-DBENCH_PIXEL_HASH`, and use
  `targets/meta/tools/m0bench/where.py` when an estimator run never finishes.
  SG-1000 renderer asm (`sg_render_asm.S`): build a firmware with
  `-DSG_SELFTEST` and read `sg_selftest_result` (1 = no differing line) in gm0;
  check both panel orders (`SG_PANEL_SWAP` 0 and 1).  Game Gear background
  asm (`gg_render_asm.S`): `-DGG_SELFTEST`, `gg_selftest_result`, likewise.
- Game Gear changes: `tests/test_gg_e2e.py` (synthetic banked cartridge,
  `tests/gg_fixture.py`) and, with ROMs, precise native = interpreter on the
  whole collection.
- Label `m0_estimate.py` numbers as estimates, never as device measurements.
- Bump `targets/meta/VERSION` (META) or `targets/consoles/pokitto/VERSION`
  when a change alters what that console's firmwares do.

To try firmwares without a console, use [gm0](https://github.com/r043v/gm0),
the META/Pokitto/ESPboy emulator kept in its own repository. For the ESPboy it
executes the firmware's own Xtensa code and emulates the chip's ROM and the
Espressif SDK by function: time there is an estimate. `ESPBOY_GM0=/path/to/gm0`
makes `tests/test_espboy_target.py` and `tests/test_sega_espboy.py` run each
built firmware in it (`ESPBOY_PIO` for the build).

- New output targets: pc (SDL2 window, the card as `sd.img`; the firmware
  builds and runs head first in CI — `tests/test_pc_target.py`, SDL2
  required), odroid (ODROID-GO, ESP32) and aka (Gamebuino AKA, ESP32-S3)
  share `targets/common/consoles/esp32/` and build through PlatformIO
  (`ODROID_PIO`/`AKA_PIO` env vars for the real build; `AKA_GM0` exercises
  the experimental AKA application machine with an ELF or matching symbols;
  ODROID is not emulated, device checks remain manual). Which system may generate
  for which console comes from the capability matrix in
  `recompiler/src/system.cpp` (`TargetFacts` vs `min_ram_bytes`) — see
  `--list-systems`/`--list-targets` and `docs/MULTI_SYSTEM.fr.md`.
- NES (`--target meta`, `--target pokitto` and the desktop harness,
  `recompiler/src/system/nes/generator.cpp`, runtime in
  `targets/nes/runtime/`): 6502 with cycles, per-line 2C02 with sprite-0
  and loopy v/t, NROM only. The 6502 code is recompiled to C
  (`recompiler/src/system/nes/emitter.cpp`, `nes_gen.h`; the dispatcher
  and its interpreter fallback in `6502.c` under `NES_NATIVE`;
  `--nes-interp` for an interpreter-only project); profiles come from the
  harness (`--interp --coverage f`, then `--coverage f`). Rule: a
  `--meta-precise` project prints the `--interp` run_hash, from the static
  walk and with a profile (`tests/test_nes_e2e.py`, synthetic cartridges);
  with a ROM, SMB precise = interpreter with an input script. Regions never
  tail-call (Thumb-1 GCC does not): every exit returns to the dispatcher.
  SMB must render identically through the harness before and after a
  runtime change (hashes + PPM captures), with `out_mismatch=0` and
  `hit_mismatch=0` (the 160x128 panel renderer against the reference
  lines, also with `--skip 2` and with other plans, `--rows 176` the
  Pokitto's);
  `ppu_asm.S` is compared with its C on the
  estimator (`BENCH_PIXEL_HASH`, `NES_NO_PPU_ASM`); APU changes: identical
  `--wav`. Vectrex (`--system vectrex`, `.vec`, `docs/VECTREX.fr.md`):
  `recompiler/src/system/vectrex/emitter.cpp` (6809 → C, `vx_gen.h`, the
  dispatcher in `m6809_run.c` under `VX_NATIVE`), the run loop as
  fallback and reference (`--interp`). Rule: a `--meta-precise` PC project
  prints the `--interp` line (`tests/test_vectrex_e2e.py`: synthetic BIOS,
  a VIA drawing program, random code) and, with ROMs, the same run_hash on
  the whole collection (profile from `--interp --coverage f`).  The ROMs
  and the system ROM stay in `output/`.  VIA changes: the same
  `--interp` run_hash on the collection, and `--wav`.  `via_asm.S`
  (Cortex-M0+): the C build (`-DVX_VIA_ASM=0`) and the asm build with
  `-DVX_DRAW_HASH` give the same `vx_draw_hash` in gm0 at the same game
  frame (`--until-guest`, input replayed with `--play-guest`). PC Engine (`--system pce`, `docs/PCE.fr.md`): the interpreter
  pipeline on the desktop harness and the PC's SDL2 window
  (`targets/pce/runtime/`, the HuC6280 cross-checked against the mednafen
  core; VDC, VCE, timer, pad, PSG; `tools/pce/build_pc_binaries.sh` for a
  Linux binary and a Windows .exe), `tests/test_pce_e2e.py` holds a
  synthetic cartridge; the PCE joins `tests/test_pc_target.py`; Raiden
  boots to its exact title screen and the .exe matches the Linux build
  byte for byte.  The sprite text's line-edge clipping is the known
  open item.  The HuC6280 → C recompiler follows the NES plan
  (`recompiler/src/system/pce/emitter.cpp`, `pce_gen.h`, the dispatcher
  in `huc6280.c` under `PCE_NATIVE`): an instruction instance is an
  address under one MPR context — the static walk proves contexts
  through `LDA #imm`/`TAM #mask` pairs, coverage carries the contexts it
  ran under, and the dispatcher enters an entry only when the machine's
  MPR bytes still match.  Rule: a `--meta-precise` project prints the
  `--interp` run_hash, from the static walk and with a profile
  (`tests/test_pce_e2e.py`, synthetic cartridges).  The console target
  waits on flash windowing for the 64 KiB of VRAM on the META.
  Mega Drive PC (`--system megadrive --target pc`, `docs/MEGADRIVE.fr.md`):
  `recompiler/src/system/megadrive/emitter.cpp` (68000 -> C regions, the
  dispatcher in the generated `md_full_code.c`), the interpreter and line
  scheduler in `targets/megadrive/runtime/md_full_cpu.c`; both share
  `md_m68k_ops.h` and the timings of `md_m68k_time.h`.  Rule: a project
  prints the `--interp` run_hash/image_hash/audio_hash, from the static walk
  and with a profile (`tests/test_megadrive_e2e.py`, random code from
  `tests/md_fixture.py`); with the ROM, Sonic native = interpreter with no
  fallback.  YM2612 changes: `megadrive_ym2612_matches_ymfm` (ymfm in
  `output/deps/ymfm` as a test oracle only) must stay exact.
  The AKA CORE v1.4 wiring follows the official Gamebuino_AKA_lib
  (`targets/consoles/aka/`, `docs/AKA.fr.md`).
  SNES (`--system snes`, `--target desktop|pc`, `docs/SNES.fr.md`):
  `recompiler/src/system/snes/` (the 65816 emitter with the context walk,
  the dispatcher in `targets/snes/runtime/snes_cpu.c` under
  `SNES_NATIVE`), the per-line PPU/HDMA/IRQ in `snes_ppu.c`, the SPC700
  interpreter and the C-simulated IPL boot in `snes_smp.c`, the BRR DSP
  in `snes_dsp.c`; the fingerprint recipe is `targets/snes/host/snes_hash.h`,
  shared by the harness and the SDL2 window; the whole-collection sweep
  (profile from the harness's `--interp`, the project regenerated with it,
  native == interp per ROM) is `tools/snes_sweep.py`.  Rule: a `--meta-precise`
  project prints the `--interp` run_hash/image_hash/audio_hash (the fast
  mode's regions stop at their labels: the boundary state is not a
  contract there); the SRAM, the A-RAM and the APU state are hashed.  A
  change to the PPU, the DMA or the APU keeps Bucket's recorded hashes
  (in docs/SNES.fr.md; the ROM stays in `output/snes/`) and the e2e's
  backdrop.

## Commits

- The author of a commit stays the session's git user (r043v); an agent that
  did the work signs the commit with its own name in a `Co-Authored-By:`
  trailer — `Co-Authored-By: GLM <noreply@z.ai>` for GLM/ZCode, its own name
  and address for any other agent (e.g.
  `Co-Authored-By: Claude <noreply@anthropic.com>`). Never commit agent work
  anonymously.

## Hygiene

- No ROMs, built firmwares or ROM-derived data in the repository or in a
  release archive (`targets/meta/package/make_package.sh` refuses them).
- Generated projects go to `out/` or `output/`, logs and captures to `logs/`
  (all ignored).
- Do not claim compatibility, accuracy or performance beyond the evidence
  actually collected.
