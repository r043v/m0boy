#!/usr/bin/env python3
"""End-to-end check of `gbrecomp --target meta` on a synthetic ROM.

The ROM is generated here (seeded) and stresses what the compact emitter
optimizes: random ALU/CB/flag sequences whose full register and F state is
recorded in WRAM after every block, forward conditional skips, counted loops,
conditional CALL/RET, RST, a JP HL jump table, code in switchable banks, and
VBlank/timer interrupts that preserve AF while arriving in the middle of
flag-carrying code.

Flow: generate an interpreter-only project, record coverage with the host
profile build, regenerate compiled projects, then require that
  * the precise build (interrupt poll per instruction) matches the
    interpreter bit for bit (every frame and WRAM), and
  * the fast build (the Gamebuino META default) reaches the same WRAM
    results, with most instructions running as compiled code, and
  * fast-forwarding wait loops (polling, delay, HRAM DEC A) changes nothing:
    same frames, WRAM, registers and cycle counter as running every
    iteration, and
  * reading the ROM through the SD streaming cache (forced on this ROM), or
    from LZSS-packed flash (--meta-pack), changes nothing either, and
  * battery RAM kept on the card or in RAM gives the same state, and a
    second run reads back what the first one saved.
"""
from __future__ import annotations

import argparse
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

NINTENDO_LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
])

FRAMES = 260
RESULT_BYTES = 0x800  # WRAM C000-C7FF holds results; the stack lives at DFxx


class Asm:
    """Tiny single-bank assembler with 16-bit label fixups."""

    def __init__(self, base: int):
        self.base = base
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str]] = []

    @property
    def pc(self) -> int:
        return self.base + len(self.code)

    def label(self, name: str) -> None:
        self.labels[name] = self.pc

    def b(self, *data: int) -> None:
        self.code += bytes(d & 0xFF for d in data)

    def ref(self, op: int, name: str) -> None:
        self.b(op)
        self.fixups.append((len(self.code), name))
        self.b(0, 0)

    def dw(self, name: str) -> None:
        self.fixups.append((len(self.code), name))
        self.b(0, 0)

    def resolve(self, extern: dict[str, int]) -> bytes:
        for off, name in self.fixups:
            addr = self.labels.get(name, extern.get(name))
            if addr is None:
                raise KeyError(name)
            self.code[off] = addr & 0xFF
            self.code[off + 1] = addr >> 8
        return bytes(self.code)


REGS_NO_B = [1, 2, 3, 7]          # C D E A (loop bodies keep B as the counter)
REGS = [0, 1, 2, 3, 4, 5, 7]      # B C D E H L A


def rand_ops(rng: random.Random, n: int, regs: list[int], depth: int = 0) -> bytes:
    """Random straight-line/forward-branching code that keeps memory safe:
    (HL) is only used right after HL is pointed into C000-C0FF."""
    out = bytearray()
    for _ in range(n):
        k = rng.randrange(20)
        r, s = rng.choice(regs), rng.choice(regs)
        if k == 0:
            out += bytes([0x06 + 8 * r, rng.randrange(256)])                  # LD r,n
        elif k == 1 and not (r == 6 and s == 6):
            out += bytes([0x40 + 8 * r + s])                                   # LD r,r'
        elif k in (2, 3, 4):
            out += bytes([0x80 + 8 * rng.randrange(8) + s])                    # ALU r
        elif k == 5:
            out += bytes([0xC6 + 8 * rng.randrange(8), rng.randrange(256)])    # ALU n
        elif k == 6:
            out += bytes([0x04 + 8 * r + rng.randrange(2)])                    # INC/DEC r
        elif k == 7:
            out += bytes([0xCB, rng.randrange(0x40) & ~7 | r])                 # rot/shift
        elif k == 8:
            out += bytes([0xCB, 0x40 + rng.randrange(0xC0) & ~7 | r])          # BIT/RES/SET
        elif k == 9:
            out += bytes([rng.choice([0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F])])
        elif k == 10:
            lo = rng.randrange(0x40, 0x100)
            op = rng.choice([0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE, 0x34, 0x35,
                             0x7E, 0x77, 0x36])
            out += bytes([0x21, lo, 0xC0, op]) + (bytes([rng.randrange(256)]) if op == 0x36 else b"")
            if rng.randrange(2):
                out += bytes([0x21, lo, 0xC0, 0xCB, rng.randrange(0x100) & ~7 | 6])
        elif k == 11 and 0 not in regs:
            continue
        elif k == 11:
            out += bytes([rng.choice([0x03, 0x13, 0x0B, 0x1B, 0x09, 0x19, 0x29])])
        elif k == 12:
            pair = rng.choice([0xC5, 0xD5, 0xF5]) if 0 in regs else rng.choice([0xD5, 0xF5])
            out += bytes([pair, 0xF1 if rng.randrange(2) else pair - 4])        # PUSH/POP
        elif k == 13 and depth < 2:
            inner = rand_ops(rng, rng.randrange(1, 5), regs, depth + 1)
            out += bytes([rng.choice([0x20, 0x28, 0x30, 0x38]), len(inner)]) + inner
        elif k == 14:
            e = rng.randrange(1, 8) * 2
            out += bytes([0xE8, (-e) & 0xFF, 0xE8, e])  # ADD SP pair: dip below live stack first
        elif k == 15:
            out += bytes([0xF8, rng.randrange(256)]) if 4 in regs else b""     # LD HL,SP+e
        elif k == 16:
            a = 0xC080 + rng.randrange(0x40)
            out += bytes([rng.choice([0xEA, 0xFA]), a & 0xFF, a >> 8])        # LD (nn)
        elif k == 17:
            out += bytes([rng.choice([0xE0, 0xF0]), 0x80 + rng.randrange(0x0F)])  # LDH
        elif k == 18:
            out += bytes([0xCF])                                                # RST 08
        else:
            out += bytes([0x00])
    return bytes(out)


def record(a: Asm, slot: int) -> None:
    """Store A B C D E H L F at C100 + 8*slot without changing registers."""
    base = 0xC100 + 8 * slot
    a.b(0xF5, 0xE5)                  # push af, push hl
    a.b(0x21, base & 0xFF, base >> 8)
    a.b(0x77, 0x23)                  # ld (hl),a / inc hl
    for r in (0, 1, 2, 3):
        a.b(0x78 + r, 0x77, 0x23)    # ld a,r / ld (hl),a / inc hl
    a.b(0xE1, 0xE5)                  # pop hl, push hl (original HL)
    a.b(0x7C, 0xEA, (base + 5) & 0xFF, (base + 5) >> 8)   # ld a,h / ld (nn),a
    a.b(0x7D, 0xEA, (base + 6) & 0xFF, (base + 6) >> 8)   # ld a,l / ld (nn),a
    a.b(0xE1, 0xF1, 0xF5, 0xC5, 0xE5)  # pop hl, pop af, push af, push bc, push hl
    a.b(0xF8, 0x04)                   # ld hl,sp+4 -> saved AF
    a.b(0x7E)                         # ld a,(hl) = F
    a.b(0xEA, (base + 7) & 0xFF, (base + 7) >> 8)
    a.b(0xE1, 0xC1, 0xF1)             # pop hl, pop bc, pop af


def make_rom(seed: int) -> bytes:
    rng = random.Random(seed)
    rom = bytearray(0x10000)
    banks = [Asm(0x0000)] + [Asm(0x4000) for _ in range(3)]
    a = banks[0]

    # Vectors.
    a.b(*([0x00] * 8))
    a.label("rst08")
    a.b(0x3C, 0xA9, 0xC9)            # inc a / xor c / ret
    a.b(*([0x00] * (0x40 - len(a.code))))
    a.ref(0xC3, "vblank")
    a.b(*([0x00] * (0x50 - len(a.code))))
    a.ref(0xC3, "timer")
    a.b(*([0x00] * (0x100 - len(a.code))))
    a.b(0x00)
    a.ref(0xC3, "main")
    a.b(*([0x00] * (0x150 - len(a.code))))

    # Main program.
    a.label("main")
    a.b(0xF3, 0x31, 0xF0, 0xDF)      # di / ld sp,$DFF0
    a.b(0xAF, 0x21, 0x00, 0xC0, 0x0E, 0x00)  # clear C000-C0FF: xor a / ld hl / ld c,0
    a.label("clr")
    a.b(0x22, 0x0D, 0x20, 0xFC)      # ld (hl+),a / dec c / jr nz
    a.b(0x21, 0x00, 0x98, 0x0E, 0x00)  # map: tile 1 on every other entry
    a.label("map")
    a.b(0x3E, 0x01, 0x22, 0x23, 0x0D, 0x20, 0xF9)
    a.b(0x3E, 0x05, 0xE0, 0xFF)      # IE = VBlank | Timer
    a.b(0x3E, 0x05, 0xE0, 0x07)      # TAC: enabled, 262144 Hz
    a.b(0xAF, 0xEA, 0xF0, 0xC7)      # iteration counter (outside random stores)
    # Battery RAM: copy A000-A03F (what a previous run saved) to C700.
    a.b(0x3E, 0x0A, 0xEA, 0x00, 0x00)  # ld a,$0A / ld ($0000),a: RAM on
    a.b(0x21, 0x00, 0xA0, 0x11, 0x00, 0xC7, 0x06, 0x40)  # ld hl,$A000 / ld de,$C700 / ld b,64
    a.b(0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA)  # ld a,(hl+) / ld (de),a / inc de / dec b / jr nz
    a.b(0xAF, 0xEA, 0x00, 0x00)        # xor a / ld ($0000),a: RAM off
    # HRAM routine at FFA0 (above the random LDH range): the OAM-DMA style
    # wait DEC A / JR NZ,-3 / RET, which only the interpreter runs.
    a.b(0x21, 0xA0, 0xFF, 0x36, 0x3D, 0x2C, 0x36, 0x20, 0x2C, 0x36, 0xFD, 0x2C, 0x36, 0xC9)
    a.b(0xFB)                        # ei
    a.label("outer")
    # Wait loops the generator fast-forwards (see detect_spin_loops); each
    # leaves registers and flags in a timing-independent state.
    a.b(0x01, 0x40, 0x01)            # ld bc,$0140
    a.b(0x0B, 0x78, 0xB1, 0x20, 0xFB)  # dec bc / ld a,b / or c / jr nz
    a.b(0x1E, 0x30, 0x1D, 0x20, 0xFD)  # ld e,$30 / dec e / jr nz
    a.b(0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA)  # ldh a,(LY) / cp $90 / jr nz
    a.b(0xF0, 0x41, 0xE6, 0x03, 0x20, 0xFA)  # ldh a,(STAT) / and 3 / jr nz
    a.b(0xF0, 0x90, 0x47)            # ldh a,($90) / ld b,a
    a.b(0xF0, 0x90, 0xB8, 0x28, 0xFB)  # ldh a,($90) / cp b / jr z: wait for VBlank
    a.b(0x3E, 0x28, 0xCD, 0xA0, 0xFF)  # ld a,$28 / call $FFA0
    # Data read from bank 2 (table at $7000), stored at C600 + iteration:
    # exercises ROM streaming, where bank 2 lives on the SD card.
    a.b(0xFA, 0xF0, 0xC7, 0x6F, 0x5F)  # ld a,(counter) / ld l,a / ld e,a
    a.b(0x26, 0x70, 0x16, 0xC6)        # ld h,$70 / ld d,$C6
    a.b(0x3E, 0x02, 0xEA, 0x00, 0x20)  # ld a,2 / ld ($2000),a
    a.b(0x7E, 0x12)                    # ld a,(hl) / ld (de),a
    a.b(0x3E, 0x03, 0xEA, 0x00, 0x20)  # same from bank 3, stored at C680 + iteration
    a.b(0x16, 0xC6, 0x7B, 0xC6, 0x80, 0x5F, 0x7E, 0x12)  # ld d,$C6 / ld a,e / add $80 / ld e,a / ld a,(hl) / ld (de),a
    a.b(0x3E, 0x01, 0xEA, 0x00, 0x20)  # ld a,1 / ld ($2000),a
    # Save: A000 + iteration = iteration, RAM enabled only around the write.
    a.b(0x3E, 0x0A, 0xEA, 0x00, 0x00)  # RAM on
    a.b(0xFA, 0xF0, 0xC7, 0x6F, 0x26, 0xA0, 0x77)  # ld a,(counter) / ld l,a / ld h,$A0 / ld (hl),a
    a.b(0xAF, 0xEA, 0x00, 0x00)        # RAM off (the moment a save is written back)
    a.b(0x06, 0x00, 0x3E, 0x5A, 0xB7)  # ld b,0 / ld a,$5A / or a
    nblocks = 40
    for i in range(nblocks):
        choice = i % 5
        if choice == 3 and i // 5 % 3 < 3:
            bank = 1 + (i // 5) % 3
            a.b(0x3E, bank, 0xEA, 0x00, 0x20)
            a.ref(0xCD, "bank%d_blk%d" % (bank, i))
            a.b(0x3E, 0x01, 0xEA, 0x00, 0x20)
        elif choice == 4:
            a.b(0x3E, rng.randrange(4))
            a.ref(0xCD, "dispatch")
        else:
            a.ref(0xCD, "blk%d" % i)
        record(a, i)
    a.b(0xFA, 0xF0, 0xC7, 0x3C, 0xEA, 0xF0, 0xC7)   # counter++
    a.b(0xFE, 60)                                   # 60 iterations
    a.ref(0xC2, "outer")
    a.b(0xF3, 0x3E, 0xAA, 0xEA, 0xFF, 0xC7)         # di / done marker
    a.label("park")
    a.b(0x76, 0x18, 0xFD)

    # Blocks: random code, counted loops, conditional calls.
    for i in range(nblocks):
        target = a
        if i % 5 == 3:
            target = banks[1 + (i // 5) % 3]
            target.label("bank%d_blk%d" % (1 + (i // 5) % 3, i))
        else:
            a.label("blk%d" % i)
        target.b(*rand_ops(rng, rng.randrange(8, 24), REGS))
        if rng.randrange(2):
            target.b(0x06, rng.randrange(1, 6))
            loop_pc = target.pc
            target.b(*rand_ops(rng, rng.randrange(2, 6), REGS_NO_B, depth=2))
            target.b(0x05, 0x20, (loop_pc - (target.pc + 3)) & 0xFF)  # dec b / jr nz,loop
        if target is a:
            a.ref(rng.choice([0xC4, 0xCC, 0xD4, 0xDC]), "sub%d" % (i % 6))
        target.b(*rand_ops(rng, rng.randrange(4, 12), REGS))
        target.b(0xC9)
    for j in range(6):
        a.label("sub%d" % j)
        a.b(*rand_ops(rng, rng.randrange(3, 8), REGS))
        a.b(rng.choice([0xC0, 0xC8, 0xD0, 0xD8]))
        a.b(*rand_ops(rng, rng.randrange(3, 8), REGS))
        a.b(0xC9)

    # Jump table: A = 0..3.
    a.label("dispatch")
    a.b(0x87, 0x5F, 0x16, 0x00)      # add a,a / ld e,a / ld d,0
    a.ref(0x21, "table")
    a.b(0x19, 0x2A, 0x66, 0x6F, 0xE9)  # add hl,de / ld a,(hl+) / ld h,(hl) / ld l,a / jp hl
    a.label("table")
    for t in range(4):
        a.dw("jt%d" % t)
    for t in range(4):
        a.label("jt%d" % t)
        a.b(*rand_ops(rng, rng.randrange(4, 10), REGS))
        a.b(0xC9)

    # Interrupt handlers preserve every register they touch.
    a.label("vblank")
    a.b(0xF5, 0xC5, 0xE5)
    a.b(0x21, 0x00, 0xC1, 0x01, 0x10, 0x80, 0x3E, 0x10)  # hl=C100 bc=8010 a=16
    a.b(0xF5)
    a.label("vcopy")                 # copy 16 result bytes to tile 1
    a.b(0x2A, 0x02, 0x03, 0xF1, 0x3D, 0xF5, 0x20, 0xF8)
    a.b(0xF1)
    a.b(0x21, 0x90, 0xFF, 0x34)      # inc (FF90)
    a.b(0xE1, 0xC1, 0xF1, 0xD9)
    a.label("timer")
    a.b(0xF5, 0xE5, 0x21, 0x91, 0xFF, 0x34, 0xE1, 0xF1, 0xD9)

    extern: dict[str, int] = {}
    for asm in banks:
        extern.update(asm.labels)
    for n, asm in enumerate(banks):
        code = asm.resolve(extern)
        start = n * 0x4000
        if len(code) > 0x4000:
            raise RuntimeError("bank %d overflow" % n)
        rom[start:start + len(code)] = code

    for bank in (2, 3):
        table = bank * 0x4000 + 0x3000
        if any(rom[table:table + 256]):
            raise RuntimeError("bank %d code reaches the data table" % bank)
        # Compressible but not uniform (a 23-byte motif with variations), so
        # a packed build decodes real back-references here.
        motif = [rng.randrange(256) for _ in range(23)]
        rom[table:table + 256] = bytes((motif[k % 23] + (k // 64) * bank) & 0xFF for k in range(256))
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x143] = b"GBL META E2E   "
    rom[0x147] = 0x03   # MBC1 + RAM + battery
    rom[0x148] = 0x01   # 64 KiB
    rom[0x149] = 0x02   # 8 KiB of cartridge RAM (kept on the card by default)
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    return bytes(rom)


def run(cmd: list[str], cwd: Path | None = None, timeout: int = 600) -> str:
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit("command timed out (livelock?): " + " ".join(cmd))
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout)
        raise SystemExit("command failed: " + " ".join(cmd))
    return proc.stdout


def build_host(project: Path, name: str = "build", cflags: str = "") -> Path:
    build = project / "host" / name
    run(["cmake", "-G", "Ninja", "-S", str(project / "host"), "-B", str(build),
         "-DCMAKE_C_FLAGS=" + cflags])
    run(["ninja", "-C", str(build)])
    return build


def deepeel_png(path: Path, chunks: dict[str, str]) -> None:
    """A minimal deepeel map PNG: a 1x1 image and zTXt chunks (deepeel_cov,
    deepeel_rom, ...), the container `gbrecomp --coverage` accepts."""
    import struct
    import zlib

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(b"\x00\x00\x00\x00"))
    for key, text in chunks.items():
        data += chunk(b"zTXt", key.encode() + b"\x00\x00" + zlib.compress(text.encode()))
    path.write_bytes(data + chunk(b"IEND", b""))


def parse(out: str) -> dict[str, str]:
    vals = {}
    for key in ("run_hash", "wram_hash", "interp_instrs"):
        m = re.search(key + r" ([0-9a-f]+)", out)
        if not m:
            raise SystemExit("missing %s in output:\n%s" % (key, out))
        vals[key] = m.group(1)
    m = re.search(r"state_hash ([0-9a-f]+)", out)
    vals["state_hash"] = m.group(1) if m else ""
    return vals


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gbrecomp", required=True)
    ap.add_argument("--seed", type=int, default=1989)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not shutil.which("cmake") or not shutil.which("ninja"):
        print("cmake and ninja are required")
        return 1

    tmp = Path(tempfile.mkdtemp(prefix="gbl_meta_e2e_"))
    try:
        rom = tmp / "meta_e2e.gb"
        rom.write_bytes(make_rom(args.seed))
        common = ["--output-prefix", "e2e"]

        boot = tmp / "boot"
        run([args.gbrecomp, str(rom), "--target", "meta", "-o", str(boot)] + common)
        bb = build_host(boot)
        cov = tmp / "coverage.txt"
        wram = tmp / "wram.bin"
        ref_out = run([str(bb / "e2e_profile"), "--interp", "--frames", str(FRAMES),
                       "--coverage", str(cov), "--wram-hash-bytes", str(RESULT_BYTES),
                       "--dump-wram", str(wram)])
        ref = parse(ref_out)
        # Checkpointing discoveries at CPU safe points must be deterministic,
        # and the profile must count all played frames across restored states.
        explored = tmp / "explored.cov"
        explore_args = [str(bb / "e2e_profile"), "--interp", "--frames", "1500",
                        "--random", "3", "--coverage", str(explored)]
        first = parse(run(explore_args))
        first_cov = explored.read_bytes()
        assert parse(run(explore_args)) == first
        assert explored.read_bytes() == first_cov
        data = wram.read_bytes()
        if data[0x7FF] != 0xAA or "sp dff0" not in ref_out:
            print(ref_out)
            raise SystemExit("synthetic program did not finish cleanly in the interpreter")

        results = {}
        for mode, extra in (("fast", []), ("thumb", ["--meta-thumb"]),
                            ("precise", ["--meta-precise"]),
                            ("thumb_precise", ["--meta-thumb", "--meta-precise"])):
            proj = tmp / mode
            out = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov),
                       "-o", str(proj)] + common + extra)
            m = re.search(r"compiled:\s+(\d+) instructions", out)
            if not m or int(m.group(1)) < 500:
                print(out)
                raise SystemExit("%s: too few compiled instructions" % mode)
            if mode == "fast":
                w = re.search(r"wait loops:\s+(\d+) polling, (\d+) delay", out)
                if not w or int(w.group(1)) < 3 or int(w.group(2)) < 2:
                    print(out)
                    raise SystemExit("fast: the wait loops were not recognised")
            if mode == "thumb":
                ir = re.search(r"register IR:\s+(\d+) blocks, (\d+) Thumb blocks", out)
                if not ir or int(ir.group(2)) == 0:
                    raise SystemExit("synthetic ROM did not exercise generated Thumb blocks")
            hb = build_host(proj)
            results[mode] = parse(run([str(hb / "e2e_host"), "--frames", str(FRAMES),
                                       "--wram-hash-bytes", str(RESULT_BYTES)]))
            # The interpreter embedded in each build must agree with the profile run.
            interp = parse(run([str(hb / "e2e_host"), "--interp", "--frames", str(FRAMES),
                                "--wram-hash-bytes", str(RESULT_BYTES)]))
            if interp["run_hash"] != ref["run_hash"]:
                raise SystemExit("%s: interpreter run differs from profile run" % mode)

        # --meta-sweep: every bank's CALL/JP abs targets offered as
        # speculative entries compile within the budget, the run unchanged
        plain_text = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov),
                          "-o", str(tmp / "plain")] + common)
        swept_text = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov),
                          "-o", str(tmp / "swept"), "--meta-sweep"] + common)
        count = lambda text: int(re.search(r"compiled:\s+(\d+)", text).group(1))
        assert count(swept_text) >= count(plain_text), (count(plain_text), count(swept_text))
        swept_build = build_host(tmp / "swept")
        swept = parse(run([str(swept_build / "e2e_host"), "--frames", str(FRAMES),
                           "--wram-hash-bytes", str(RESULT_BYTES)]))
        assert swept["run_hash"] == ref["run_hash"], "--meta-sweep moved the run"

        # a deepeel map PNG as --coverage: its deepeel_cov chunk is read like
        # the .cov (same compiled set), its deepeel_rom chunk becomes the ROM
        # read profile when streaming
        png = tmp / "deepeel.map.png"
        deepeel_png(png, {"deepeel_cov": cov.read_text(), "deepeel_rom": "10 1000\n20 5\n"})
        png_text = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(png),
                        "-o", str(tmp / "png")] + common)
        assert count(png_text) == count(plain_text), (count(plain_text), count(png_text))
        png_stream = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(png),
                          "--meta-stream", "on", "-o", str(tmp / "png_stream")] + common)
        assert "ROM read profile:   deepeel_rom of deepeel.map.png" in png_stream, png_stream

        # Runtime options: with batching, the frame/line signatures and -Os
        # disabled (and the pal32 table enabled) the fast build must behave
        # identically (this ROM only
        # touches VRAM from its VBlank handler), and the static-frame policy
        # may never leave a wrong picture on screen for more than one frame.
        plain = build_host(tmp / "fast", "build-plain",
                           "-DGBL_PPU_BATCH=1 -DGBL_FRAME_SIGNATURE=0 -DGBL_LINE_SIGNATURE=0 "
                           "-DGBL_GEN_OPTIMIZE_SIZE=0 -DGBL_PPU_PAL32=1")
        results["plain"] = parse(run([str(plain / "e2e_host"), "--frames", str(FRAMES),
                                      "--wram-hash-bytes", str(RESULT_BYTES)]))
        # Wait-loop fast-forward must be invisible: same frames, WRAM, and
        # machine state (registers and cycle counter) at every VBlank.
        nospin = build_host(tmp / "fast", "build-nospin", "-DGBL_SPIN=0 -DGBL_INTERP_DELAY_LOOP=0")
        results["nospin"] = parse(run([str(nospin / "e2e_host"), "--frames", str(FRAMES),
                                       "--wram-hash-bytes", str(RESULT_BYTES)]))
        # ROM streaming, forced on this small ROM: a first streaming project
        # records the ROM data reads, the second keeps the hottest 4 KiB chunk
        # in flash and reads the others through a 2-sector cache.  Same
        # results.
        s1 = tmp / "stream1"
        run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov), "--meta-stream", "on",
             "-o", str(s1)] + common)
        prof = tmp / "romdata.prof"
        run([str(build_host(s1) / "e2e_profile"), "--frames", str(FRAMES), "--rom-profile-out", str(prof)])
        s2 = tmp / "stream2"
        gen = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov), "--meta-stream", "on",
                   "--rom-profile", str(prof), "--meta-rom-resident-kib", "4", "-o", str(s2)] + common)
        stream_out = run([str(build_host(s2, "build", "-DGBL_STREAM_SECTORS=2") / "e2e_host"),
                          "--frames", str(FRAMES), "--wram-hash-bytes", str(RESULT_BYTES)])
        results["stream"] = parse(stream_out)
        sm = re.search(r"rom_stream sectors_read (\d+)", stream_out)
        # Same, with the streamed sectors LZSS-packed in flash (--meta-pack).
        s3 = tmp / "packed"
        pgen = run([args.gbrecomp, str(rom), "--target", "meta", "--coverage", str(cov), "--meta-pack",
                    "--rom-profile", str(prof), "--meta-rom-resident-kib", "4", "-o", str(s3)] + common)
        packed_bin = str(build_host(s3, "build", "-DGBL_STREAM_SECTORS=2") / "e2e_host")
        packed_out = run([packed_bin,
                          "--frames", str(FRAMES), "--wram-hash-bytes", str(RESULT_BYTES)])
        results["packed"] = parse(packed_out)
        pm = re.search(r"rom_stream sectors_read (\d+)", packed_out)
        # A large, mostly incompressible cartridge cannot fit wholly packed.
        # It must combine raw chunks, packed flash and actual SD reads.
        hybrid_rom = tmp / "hybrid.gb"
        import random
        data = bytearray(rom.read_bytes())
        data.extend(random.Random(12345).randbytes(0x80000 - len(data)))
        data[0x148] = 4
        data[0x14D] = (-sum(data[0x134:0x14D]) - 25) & 0xff
        hybrid_rom.write_bytes(data)
        hybrid_prof = tmp / "hybrid.prof"
        lines = [l for l in prof.read_text().splitlines()
                 if not l.strip() or l.startswith('#') or int(l.split()[0], 16) // 16 != 15]
        lines += ['%x 1' % (c * 16) for c in range(16, 128)]
        hybrid_prof.write_text('\n'.join(lines) + '\n')
        hybrid = tmp / "hybrid"
        run([args.gbrecomp, str(hybrid_rom), '--target', 'meta', '--coverage', str(cov), '--meta-pack',
             '--meta-precise', '--rom-profile', str(hybrid_prof), '--meta-rom-resident-kib', '4', '-o', str(hybrid)] + common)
        config = (hybrid / 'src/gbl_config.h').read_text()
        assert '#define GBL_ROM_SD 1' in config and '#define GBL_ROM_PACKED 1' in config
        hybrid_bin = str(build_host(hybrid, 'build', '-DGBL_STREAM_SECTORS=2') / 'e2e_host')
        for flags in ([], ['--interp']):
            out = run([hybrid_bin, '--frames', str(FRAMES), '--wram-hash-bytes', str(RESULT_BYTES)] + flags)
            result = parse(out)
            assert result['run_hash'] == ref['run_hash'] and result['wram_hash'] == ref['wram_hash']
            sources = re.search(r'rom_sources sd_reads (\d+) flash_unpacked (\d+)', out)
            assert sources and int(sources[1]) and int(sources[2]), out
        for stack in (0, 64, 1024):
            result = parse(run([hybrid_bin, '--frames', str(FRAMES), '--stack-bytes', str(stack),
                                '--wram-hash-bytes', str(RESULT_BYTES)], timeout=60))
            assert result['run_hash'] == ref['run_hash'] and result['wram_hash'] == ref['wram_hash']
        # C stack exhaustion: compiled regions unwind and the dispatcher
        # resumes them. Small budgets (bytes) include the ones where a
        # region is entered with too little room for its own guard, which
        # used to spin forever; zero budget must fall back to the interpreter.
        # Frames and WRAM must match the interpreter everywhere, and a 1 KiB
        # stack must not even change the full machine state.
        stack_runs = {n: parse(run([packed_bin, "--frames", str(FRAMES), "--stack-bytes", str(n),
                                    "--wram-hash-bytes", str(RESULT_BYTES)], timeout=60))
                      for n in (0, 64, 80, 128, 192, 256, 1024)}
        # Cartridge RAM: on the card (default for 8 KiB) or in RAM, same
        # machine state; and a second run reads back what the first saved.
        ramb = build_host(tmp / "fast", "build-eram-ram", "-DGBL_ERAM_ON_SD=0")
        results["eram_ram"] = parse(run([str(ramb / "e2e_host"), "--frames", str(FRAMES),
                                         "--wram-hash-bytes", str(RESULT_BYTES)]))
        save = tmp / "e2e.sav"
        save.write_bytes(b"\xff" * 8192)
        fast_host = str(tmp / "fast" / "host" / "build" / "e2e_host")
        boot_copy = []
        for n in range(2):
            dump = tmp / ("wram_save%d.bin" % n)
            run([fast_host, "--frames", str(FRAMES), "--save", str(save), "--dump-wram", str(dump)])
            boot_copy.append(dump.read_bytes()[0x700:0x740])
        saved = save.read_bytes()
        static_out = run([str(tmp / "fast" / "host" / "build" / "e2e_host"), "--frames", str(FRAMES),
                          "--static-skip-check"])
        # Save states: save after frame N, play M frames, load, play them
        # again: the same machine at the end (cartridge RAM on the card and
        # in RAM, interpreted and compiled).
        state_runs = {}
        for label, binary, extra in (("card interp", fast_host, ["--interp"]),
                                     ("card fast", fast_host, []),
                                     ("ram interp", str(ramb / "e2e_host"), ["--interp"])):
            for n, m in ((40, 90), (150, 60)):
                out = run([binary] + extra + ["--state-test", str(n), str(m)], timeout=60)
                state_runs["%s %d+%d" % (label, n, m)] = "state_test ok" in out
        m = re.search(r"static_skip skipped (\d+) stale (\d+) stale_run_max (\d+)", static_out)

        failures = []
        for name, ok in state_runs.items():
            if not ok:
                failures.append("save state not restored exactly (%s)" % name)
        if results["plain"]["run_hash"] != results["fast"]["run_hash"] or \
                results["plain"]["wram_hash"] != results["fast"]["wram_hash"]:
            failures.append("disabling the runtime options changed the fast build's output")
        lm = re.search(r"line_skip unchanged (\d+) of (\d+) wrong (\d+)", static_out)
        if not lm or int(lm.group(3)) != 0 or int(lm.group(1)) == 0:
            failures.append("line signatures not exact: %s" % (lm.group(0) if lm else static_out))
        if not m or int(m.group(3)) > 1:
            failures.append("static-frame policy left a stale picture: %s" % (m.group(0) if m else static_out))
        if any(results["nospin"][k] != results["fast"][k] for k in ("run_hash", "wram_hash", "state_hash")):
            failures.append("wait-loop fast-forward changed the fast build's state")
        if any(results["packed"][k] != results["fast"][k] for k in ("run_hash", "wram_hash", "state_hash")):
            failures.append("the packed-ROM build differs from the flash build")
        for n, r in stack_runs.items():
            if r["run_hash"] != ref["run_hash"] or r["wram_hash"] != ref["wram_hash"]:
                failures.append("a %d-byte C stack changes the frames or WRAM" % n)
        if stack_runs[1024]["state_hash"] != results["packed"]["state_hash"]:
            failures.append("a 1 KiB C stack changes the machine state")
        if "ROM packed:" not in pgen or not pm or int(pm.group(1)) == 0:
            failures.append("packed ROM not exercised: %s / %s" % (
                re.findall(r"ROM packed:.*|--meta-pack:.*", pgen), pm.group(0) if pm else packed_out))
        if any(results["stream"][k] != results["fast"][k] for k in ("run_hash", "wram_hash", "state_hash")):
            failures.append("the ROM streaming build differs from the flash build")
        if "ROM streaming:      4 KiB" not in gen or not sm or int(sm.group(1)) == 0:
            failures.append("ROM streaming not exercised: %s / %s" % (
                re.findall(r"ROM streaming:.*", gen), sm.group(0) if sm else stream_out))
        if any(results["eram_ram"][k] != results["fast"][k] for k in ("run_hash", "wram_hash", "state_hash")):
            failures.append("cartridge RAM on the card and in RAM give different results")
        expected = bytes(range(60)) + b"\xff" * 4
        if saved[:64] != expected or boot_copy[0] != b"\xff" * 64 or boot_copy[1] != expected:
            failures.append("battery save not kept: file %s, boot copies %s / %s" % (
                saved[:64].hex(), boot_copy[0].hex(), boot_copy[1].hex()))
        for key in ("run_hash", "wram_hash", "state_hash"):
            if results["thumb"][key] != results["fast"][key]:
                raise SystemExit("register IR changed fast " + key)
            if results["thumb_precise"][key] != results["precise"][key]:
                raise SystemExit("register IR changed precise " + key)
        if results["precise"]["run_hash"] != ref["run_hash"]:
            failures.append("precise frames differ from the interpreter")
        if results["precise"]["wram_hash"] != ref["wram_hash"]:
            failures.append("precise WRAM differs from the interpreter")
        if results["fast"]["wram_hash"] != ref["wram_hash"]:
            failures.append("fast WRAM results differ from the interpreter")
        ref_instrs = int(ref["interp_instrs"])
        fast_instrs = int(results["fast"]["interp_instrs"])
        if fast_instrs * 10 > ref_instrs:
            failures.append("fast build still interprets %d of %d instructions" % (fast_instrs, ref_instrs))
        if failures:
            print("\n".join(failures))
            print("reference", ref, "results", results, "kept in", tmp)
            args.keep = True
            return 1
        print("meta target e2e: precise build bit-exact, fast build WRAM-exact "
              "(%d/%d instructions interpreted)" % (fast_instrs, ref_instrs))
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("artifacts:", tmp)


if __name__ == "__main__":
    sys.exit(main())
