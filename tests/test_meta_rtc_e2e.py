#!/usr/bin/env python3
"""MBC3 RTC: register semantics, machine-time advance and .SAV persistence.

Two synthetic MBC3+TIMER cartridges from the same builder:
- the writer writes S/M/H, latches, copies the latched registers to $C700;
- the reader latches (nothing written), copies the registers to $C700 and
  parks: with the harness's 60 frames a second, N frames put N/60 seconds
  on the counter (the platform clock reads 0 on the host, so only machine
  time advances).

Checks: precise and fast builds print the interpreter's run_hash (the
writer with --save marks the RTC dirty, the save is flushed at the end);
the writer's registers land in $C700; a second reader run against the
writer's .SAV finds the persisted H/M and 10 s more on S; the .SAV carries
the RTC magic.  Both cartridge RAM sizes are exercised (2 KiB: whole RAM
mapped, no cache; 8 KiB: cartridge RAM in the save file, sector cache)."""

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cmd, timeout=600):
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit("command timed out (livelock?): " + " ".join(cmd))
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout)
        raise SystemExit("command failed: " + " ".join(cmd))
    return proc.stdout


def parse(out):
    m = re.search(r"run_hash ([0-9a-f]+)", out)
    if not m:
        raise SystemExit("missing run_hash:\n" + out)
    return m.group(1)


def make_rom(writer: bool, ram_code: int) -> bytes:
    """Bank 0 program; --frames drive the RTC on the reader."""
    rom = bytearray(0x20000)  # 128 KiB (header 0x01)
    rom[0x0100] = 0x00  # nop
    rom[0x0101] = 0xC3  # jp $0150
    rom[0x0102:0x0104] = struct.pack("<H", 0x0150)

    pc = 0x0150
    code = bytearray()

    def emit(*b):
        code.extend(b)

    emit(0xF3, 0x31, 0xF0, 0xDF)          # di / ld sp,$DFF0
    emit(0x3E, 0x0A, 0xEA, 0x00, 0x00)    # RAM enable on
    if writer:
        emit(0x06, 8)                     # ld b,8: first register
        emit(0x21, 0x50, 0x02)            # ld hl,$0250: values S=10 M=20 H=3 DL=0 DH=0
        wloop = len(code)
        emit(0x78, 0xEA, 0x00, 0x40)      # ld a,b / ld ($4000),a: select the register
        emit(0x2A, 0xEA, 0x00, 0xA0)      # ld a,(hl+) / ld ($A000),a: write it
        emit(0x04, 0x78, 0xFE, 0x0D)      # inc b / ld a,b / cp 13
        emit(0x20)
        jr = len(code)
        emit(0)
        code[jr] = (wloop - (jr + 1)) & 0xFF
        # Copy the latched registers: select 8..12 one by one.
        emit(0x06, 8)                     # ld b,8
        emit(0x11, 0x00, 0xC7)            # ld de,$C700
        rloop = len(code)
        emit(0x78, 0xEA, 0x00, 0x40)      # select register b
        emit(0x2A)                        # ld a,(hl+): $A000 latched value
        emit(0x12)                        # ld (de),a
        emit(0x13, 0x04, 0x78, 0xFE, 0x0D)  # inc de / inc b / ld a,b / cp 13
        emit(0x20)
        jr = len(code)
        emit(0)
        code[jr] = (rloop - (jr + 1)) & 0xFF
    else:
        # 600 full frames (LY=$90 then LY!=$90), then latch (00 arms, 01
        # snapshots) and copy the five registers: the harness's frames put
        # 10 s on the counter before the snapshot.
        emit(0x21, 0x58, 0x02)            # ld hl,600
        outer = len(code)
        w1 = len(code)
        emit(0xF0, 0x44, 0xFE, 0x90)      # wait LY==$90
        emit(0x20); jr = len(code); emit(0); code[jr] = (w1 - (jr + 1)) & 0xFF
        w2 = len(code)
        emit(0xF0, 0x44, 0xFE, 0x90)      # wait LY!=$90
        emit(0x28); jr = len(code); emit(0); code[jr] = (w2 - (jr + 1)) & 0xFF
        emit(0x2B, 0x7D, 0xB4, 0x20)      # dec hl / ld a,l / or h / jr nz (dec hl keeps flags)
        jr = len(code)
        emit(0)
        code[jr] = (outer - (jr + 1)) & 0xFF
        emit(0xAF, 0xEA, 0x00, 0x60)      # xor a / ld ($6000),a
        emit(0x3E, 0x01, 0xEA, 0x00, 0x60)  # ld a,1 / ld ($6000),a
        emit(0x06, 8)                     # ld b,8
        emit(0x21, 0x00, 0xA0)            # ld hl,$A000
        emit(0x11, 0x00, 0xC7)            # ld de,$C700
        rloop = len(code)
        emit(0x78, 0xEA, 0x00, 0x40)      # select register b
        emit(0x7E)                        # ld a,(hl)
        emit(0x12, 0x13, 0x04, 0x78, 0xFE, 0x0D)  # ld (de),a / inc de / inc b / ld a,b / cp 13
        emit(0x20)
        jr = len(code)
        emit(0)
        code[jr] = (rloop - (jr + 1)) & 0xFF

    emit(0x3E, 0xAA, 0xEA, 0xC7, 0xF0)    # done marker at $F0C7
    emit(0xFB, 0x76, 0x18, 0xFD)          # ei / halt / jr -1: park
    rom[pc:pc + len(code)] = code

    if writer:
        rom[0x0250:0x0255] = bytes([10, 20, 3, 0, 0])

    rom[0x134:0x144] = b"GBL RTC E2E     "  # 16 raw title bytes
    rom[0x147] = 0x10   # MBC3 + timer + RAM + battery
    rom[0x148] = 0x01   # 64 KiB
    rom[0x149] = ram_code
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    return bytes(rom)


def build_host(project):
    build = project / "host" / "build"
    run(["cmake", "-G", "Ninja", "-S", str(project / "host"), "-B", str(build)])
    run(["ninja", "-C", str(build)])
    return build


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gbrecomp", required=True)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="gbl_rtc_e2e_"))
    failures = []
    try:
        for label, ram_code in (("eram2k", 0x01), ("eram8k", 0x02)):
            root = tmp / label
            root.mkdir(parents=True)
            sav = root / "cart.sav"

            wrom = tmp / f"rtc_w_{label}.gb"
            wrom.write_bytes(make_rom(writer=True, ram_code=ram_code))
            wp = tmp / f"rtc_w_{label}_proj"
            run([args.gbrecomp, str(wrom), "--target", "meta", "-o", str(wp),
                 "--output-prefix", "rtcw"])
            wb = build_host(wp)

            # The writer: registers written, latched, saved at the end.
            run([str(wb / "rtcw_host"), "--frames", "60", "--save", str(sav)])
            eram = 2048 if ram_code == 0x01 else 8192
            if not sav.exists() or len(sav.read_bytes()) != eram + 512:
                failures.append(f"{label}: .SAV missing or too small")
            else:
                blob = sav.read_bytes()
                if blob[eram + 5] != 0x59:  # RTC sector at GBL_ERAM_SIZE/512
                    failures.append(f"{label}: RTC magic not in the .SAV's RTC sector")

            rrom = tmp / f"rtc_r_{label}.gb"
            rrom.write_bytes(make_rom(writer=False, ram_code=ram_code))

            # Precise == interp == fast on a fresh reader: 600 frames, 10 s.
            hashes = {}
            for mode, extra in (("interp", ["--interp"]), ("precise", ["--meta-precise"]),
                                ("fast", [])):
                p = tmp / f"rtc_r_{label}_{mode}"
                run([args.gbrecomp, str(rrom), "--target", "meta", "-o", str(p),
                     "--output-prefix", "rtcr"] + extra)
                b = build_host(p)
                out = run([str(b / "rtcr_host"), "--frames", "700",
                           "--dump-wram", str(root / f"wram_{mode}.bin")])
                hashes[mode] = parse(out)
            if hashes["precise"] != hashes["interp"] or hashes["fast"] != hashes["interp"]:
                failures.append(f"{label}: run_hash mismatch interp/precise/fast: {hashes}")

            data = (root / "wram_interp.bin").read_bytes()
            got = data[0x700:0x705]
            want = bytes([10, 0, 0, 0, 0])  # S=10 (600 frames), M=0, H=0
            if got != want:
                failures.append(f"{label}: fresh reader registers {got.hex()} != {want.hex()}")

            # Persistence: the reader against the writer's .SAV keeps H=3, M=20.
            p = tmp / f"rtc_r_{label}_saved"
            run([args.gbrecomp, str(rrom), "--target", "meta", "-o", str(p),
                 "--output-prefix", "rtcr"])
            b = build_host(p)
            out = run([str(b / "rtcr_host"), "--frames", "700", "--save", str(sav),
                       "--dump-wram", str(root / "wram_saved.bin")])
            data = (root / "wram_saved.bin").read_bytes()
            got = data[0x700:0x705]
            want = bytes([21, 20, 3, 0, 0])  # 12011 s saved, +10 s of play
            if got != want:
                failures.append(f"{label}: persisted registers {got.hex()} != {want.hex()}")
    finally:
        if args.keep:
            print(tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("\n".join(failures))
        return 1
    print("rtc e2e ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
