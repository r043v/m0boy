#!/usr/bin/env python3
"""`m0recomp --target pokitto` on the synthetic ROM of test_meta_target_e2e.

The Pokitto build shares the runtime, the generated code and the frontend
with the Gamebuino META build (targets/common/platform + the Game Boy frontend in targets/meta/platform, the console
switches in m0_target.h and runtime/gbl_profile.h); this
checks what differs:
  * the generated project: the Pokitto drivers instead of the META ones,
    GBL_TARGET_POKITTO in gbl_config.h, a Makefile and pokitto.ld instead of
    PlatformIO files, and switching one directory between the two consoles
    leaves no driver of the other behind;
  * the host harness of a Pokitto project builds (no META display tool);
  * when an arm-none-eabi toolchain is available (PATH or PlatformIO's
    toolchain-gccarmnoneeabi), the firmware links below the loader at
    0x39000 with the LPC boot checksum in vector word 7 and the
    "gbrecomp-pokitto" version string compile.sh reads back.
Without the toolchain the firmware part is skipped (and said so).
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_meta_target_e2e import build_host, make_rom, run  # noqa: E402

POKITTO_DRIVERS = {"pokitto_startup.c", "pokitto_clock.c", "pokitto_lcd.c", "pokitto_lcd_asm.S",
                   "pokitto_display.c", "pokitto_audio.c", "pokitto_audio_asm.S", "pokitto_sd.c",
                   "pokitto_buttons.c", "pokitto_hw.h", "lpc11u6x.h"}
META_DRIVERS = {"meta_lcd.cpp", "gbl_display.c", "meta_audio.cpp", "meta_sd_bus.cpp", "meta_clock.cpp",
                "meta_dma.cpp"}
SHARED = {"gb_main.cpp", "m0_menu.cpp", "gbl_save.cpp", "m0_state.cpp", "gbl_stream.cpp",
          "m0_fat.c", "m0_sd.c", "gbl_palettes.c", "m0_target.h", "gbl_profile.h",
          "gbl_board.h"}


def toolchain() -> str | None:
    if shutil.which("arm-none-eabi-gcc"):
        return ""
    pio = Path.home() / ".platformio" / "packages" / "toolchain-gccarmnoneeabi" / "bin"
    if (pio / "arm-none-eabi-gcc").exists():
        return str(pio) + "/"
    return None


def generate(m0recomp: str, rom: Path, out: Path, target: str) -> None:
    run([m0recomp, str(rom), "--target", target, "--output-prefix", "e2e", "--meta-static", "--meta-thumb", "-o", str(out)])


def check_files(out: Path, pokitto: bool) -> None:
    src = {p.name for p in (out / "src").iterdir()}
    want, unwanted = (POKITTO_DRIVERS, META_DRIVERS) if pokitto else (META_DRIVERS, POKITTO_DRIVERS)
    missing = (want | SHARED) - src
    stale = unwanted & src
    if missing or stale:
        raise SystemExit("%s project: missing %s, left over %s" %
                         ("pokitto" if pokitto else "meta", sorted(missing), sorted(stale)))
    config = (out / "src" / "gbl_config.h").read_text()
    if ("#define GBL_TARGET_POKITTO 1" in config) != pokitto:
        raise SystemExit("GBL_TARGET_POKITTO in gbl_config.h does not match the target")
    project = {"Makefile", "pokitto.ld"} if pokitto else {"platformio.ini", "gamebuino_meta.ld"}
    other = {"platformio.ini", "gamebuino_meta.ld"} if pokitto else {"Makefile", "pokitto.ld"}
    names = {p.name for p in out.iterdir()}
    if project - names or other & names:
        raise SystemExit("project files: expected %s, found %s" % (sorted(project), sorted(names)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--m0recomp", required=True)
    ap.add_argument("--seed", type=int, default=1989)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="gbl_pokitto_"))
    try:
        rom = tmp / "pokitto_e2e.gb"
        rom.write_bytes(make_rom(args.seed))
        out = tmp / "project"

        generate(args.m0recomp, rom, out, "pokitto")
        check_files(out, pokitto=True)
        generate(args.m0recomp, rom, out, "meta")
        check_files(out, pokitto=False)
        generate(args.m0recomp, rom, out, "pokitto")
        check_files(out, pokitto=True)
        print("project files: ok (pokitto, meta, pokitto in one directory)")

        build = build_host(out)
        if not (build / "e2e_host").exists() or (build / "e2e_metaview").exists():
            raise SystemExit("host harness: e2e_host expected, no e2e_metaview")
        print("host harness: ok")

        tc = toolchain()
        if tc is None:
            print("SKIP firmware: no arm-none-eabi-gcc (PATH or ~/.platformio)")
            return 0
        compilers = ["gcc"] + (["clang"] if shutil.which("clang") and shutil.which("clang++") else [])
        for compiler in compilers:
            env = dict(os.environ, TOOLCHAIN=tc, FIRMWARE_COMPILER=compiler)
            proc = subprocess.run(["make", "-C", str(out), "-j4", "COMPILER=" + compiler],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=600)
            if proc.returncode != 0:
                sys.stdout.write(proc.stdout[-6000:])
                raise SystemExit(compiler + " firmware build failed")
            folder = "build-clang" if compiler == "clang" else "build"
            fw = (out / folder / "firmware.bin").read_bytes()
            if len(fw) > 0x39000:
                raise SystemExit("firmware is %d bytes, above the loader at 0x39000" % len(fw))
            words = struct.unpack_from("<8I", fw, 0)
            if sum(words) & 0xFFFFFFFF:
                raise SystemExit("vector checksum: the first eight words sum to %#x" % (sum(words) & 0xFFFFFFFF))
            sp, reset = words[0], words[1]
            if not (0x10000100 < sp <= 0x10008000) or not (reset & 1) or reset >= len(fw):
                raise SystemExit("vectors: SP %#x, reset %#x" % (sp, reset))
            if b"gbrecomp-pokitto " not in fw:
                raise SystemExit("version string gbrecomp-pokitto missing from the firmware")
            print("%s firmware: %d bytes, checksum and vectors ok" % (compiler, len(fw)))
        return 0
    finally:
        if args.keep:
            print("kept", tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
