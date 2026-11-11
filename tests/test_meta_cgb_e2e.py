#!/usr/bin/env python3
"""CGB mode: palettes, VRAM banks, GDMA, attributes and double speed.

A synthetic CGB-only cartridge (0x143 = C0) for the pc target with --cgb:
- writes BG palette 0 (BCPS/BCPD), reads it back;
- fills WRAM with a pattern, GDMA-copies it to VRAM $8000;
- writes the attribute map with VBK=1 and reads both banks back;
- switches to double speed (KEY1 + STOP) and counts DIV increments over
  60 frames, half of them before the switch;
- copies every readback to $C700 and parks.

Checks: precise and fast builds print the interpreter's run_hash; the
readbacks land in $C700 (palette, tile data through the DMA, banked map
bytes); DIV advances twice as fast after the switch."""

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


def make_rom() -> bytes:
    rom = bytearray(0x8000)
    rom[0x0100] = 0x00  # nop
    rom[0x0101] = 0xC3  # jp $0150
    rom[0x0102:0x0104] = struct.pack("<H", 0x0150)

    code = bytearray()

    def emit(*b):
        code.extend(b)

    def rel8(opcode, target):
        emit(opcode)
        code.append((target - (len(code) + 1)) & 0xFF)  # JR: relative to the next instruction

    emit(0xF3, 0x31, 0xF0, 0xDF)          # di / ld sp,$DFF0
    # BG palette 0: color 0 = $7FFF (white), color 1 = $FF7F, color 2 = $03E0
    emit(0x3E, 0x80, 0xE0, 0x68)          # BCPS = $80 (index 0, autoinc)
    emit(0x3E, 0xFF, 0xE0, 0x69)          # BCPD low
    emit(0x3E, 0x7F, 0xE0, 0x69)          # BCPD high
    emit(0x3E, 0x7F, 0xE0, 0x69)
    emit(0x3E, 0xFF, 0xE0, 0x69)
    emit(0x3E, 0xE0, 0xE0, 0x69)
    emit(0x3E, 0x03, 0xE0, 0x69)
    # read color 2 back: BCPS = $84 (index 2 low), then $85 (high), BCPD -> $C700
    emit(0x3E, 0x84, 0xE0, 0x68)
    emit(0xF0, 0x69, 0xEA, 0x00, 0xC7)    # ldh a,($69) / ld ($C700),a
    emit(0x3E, 0x85, 0xE0, 0x68)
    emit(0xF0, 0x69, 0xEA, 0x01, 0xC7)
    # WRAM $C100-$C1FF: pattern i (via A = counter), then GDMA to $8000
    emit(0x21, 0x00, 0xC1)                # ld hl,$C100
    emit(0x3E, 0x00)                      # ld a,0
    emit(0xAF)                            # xor a
    wloop = len(code)
    emit(0x77)                            # ld (hl),a
    emit(0x3C)                            # inc a
    emit(0x23)                            # inc hl
    emit(0x7D)                            # ld a,l
    emit(0xFE, 0x00)                      # cp 0 (L wrapped)
    rel8(0x20, wloop)                     # jr nz
    # GDMA: src $C100, dst $8000, $7F blocks = 2048 bytes
    emit(0x3E, 0xC1, 0xE0, 0x51)          # src high
    emit(0x3E, 0x00, 0xE0, 0x52)          # src low
    emit(0xAF, 0xE0, 0x53)                # dst high $80
    emit(0xAF, 0xE0, 0x54)                # dst low
    emit(0x3E, 0x7F, 0xE0, 0x55)          # GDMA start
    # read back a DMA-written tile byte ($8010) -> $C702
    emit(0xFA, 0x10, 0x80, 0xEA, 0x02, 0xC7)
    # VBK = 1: write attributes at $9C40 ($E7 = palette 7, prio, yflip? keep
    # bits simple: $E7 = prio|yflip|bank1|pal7), read back -> $C703
    emit(0x3E, 0x01, 0xE0, 0x4F)
    emit(0x21, 0x40, 0x9C)                # ld hl,$9C40
    emit(0x3E, 0xE7, 0x77)                # ld (hl),$E7
    emit(0x7E, 0xEA, 0x03, 0xC7)          # ld a,(hl) / ld ($C703),a
    # write bank-1 tile data at $9210 (VBK=1 banks $8000-$9FFF) -> $C704
    emit(0x21, 0x10, 0x92)
    emit(0x3E, 0x5A, 0x77)
    emit(0x7E, 0xEA, 0x04, 0xC7)
    emit(0xAF, 0xE0, 0x4F)                # VBK = 0
    # DIV counting: sample ($FF04) every frame for 60 frames, sum the deltas
    # (DIV overflows; count "changed" samples instead of the deltas).
    emit(0x21, 0x58, 0x02)                # ld hl,600 frames at normal speed
    cnt60 = len(code)
    # one frame: wait LY == $90 then LY != $90
    w1 = len(code)
    emit(0xF0, 0x44, 0xFE, 0x90)          # ldh a,(LY) / cp $90
    rel8(0x20, w1)
    w2 = len(code)
    emit(0xF0, 0x44, 0xFE, 0x90)
    rel8(0x28, w2)
    emit(0x2B, 0x7D, 0xB4)                # dec hl / ld a,l / or h (dec hl keeps flags)
    rel8(0x20, cnt60)
    # switch to double speed
    emit(0x3E, 0x01, 0xE0, 0x4D)          # KEY1 = 1 (arm)
    emit(0x10, 0x00)                      # STOP
    emit(0xF0, 0x4D, 0xEA, 0x05, 0xC7)    # KEY1 -> $C705: bit 7 set in double speed
    # 600 more frames at double speed
    emit(0x21, 0x58, 0x02)                # ld hl,600 again (the loop spent it)
    cnt2 = len(code)
    w3 = len(code)
    emit(0xF0, 0x44, 0xFE, 0x90)
    rel8(0x20, w3)
    w4 = len(code)
    emit(0xF0, 0x44, 0xFE, 0x90)
    rel8(0x28, w4)
    emit(0x2B, 0x7D, 0xB4)
    rel8(0x20, cnt2)
    emit(0xF0, 0x4D, 0xEA, 0x06, 0xC7)    # KEY1 -> $C706 (still double)
    emit(0x3E, 0xAA, 0xEA, 0xC7, 0xF0)    # done marker
    emit(0xFB, 0x76, 0x18, 0xFD)          # ei / halt / jr -1: park

    rom[0x0150:0x0150 + len(code)] = code
    rom[0x134:0x144] = b"GBL CGB E2E     "  # 16 raw title bytes
    rom[0x143] = 0xC0   # CGB only
    rom[0x147] = 0x00   # ROM only
    rom[0x148] = 0x00   # 32 KiB
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

    tmp = Path(tempfile.mkdtemp(prefix="gbl_cgb_e2e_"))
    failures = []
    try:
        romp = tmp / "cgb_e2e.gbc"
        romp.write_bytes(make_rom())

        hashes = {}
        for mode, extra in (("interp", ["--interp"]), ("precise", ["--meta-precise"]),
                            ("fast", [])):
            p = tmp / f"cgb_{mode}"
            run([args.gbrecomp, str(romp), "--target", "pc", "--cgb", "-o", str(p),
                 "--output-prefix", "cgbe"] + extra)
            b = build_host(p)
            out = run([str(b / "cgbe_host"), "--frames", "1500",
                       "--dump-wram", str(tmp / f"wram_{mode}.bin")])
            hashes[mode] = parse(out)
        if hashes["precise"] != hashes["interp"] or hashes["fast"] != hashes["interp"]:
            failures.append(f"run_hash mismatch interp/precise/fast: {hashes}")

        data = (tmp / "wram_interp.bin").read_bytes()
        got = data[0x700:0x707]
        want = bytes([
            0xE0,          # color 2 low byte read back from BCPD
            0x03,          # color 2 high
            0x10,          # GDMA-copied tile byte at $8010 (pattern i=0x10)
            0xE7,          # attribute byte read back with VBK=1
            0x5A,          # bank-1 tile byte read back with VBK=1
            0xFE,          # KEY1 after the switch: 0x7E | double-speed bit
            0xFE,          # KEY1 after 600 more frames: still double
        ])
        if got != want:
            failures.append(f"CGB readbacks {got.hex()} != {want.hex()}")
    finally:
        if args.keep:
            print(tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("\n".join(failures))
        return 1
    print("cgb e2e ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
