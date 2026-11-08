#!/usr/bin/env python3
"""Vectrex end to end, on a synthetic system ROM and synthetic cartridges.

1. A program that draws through the VIA as the BIOS does (DAC, sample and
   hold, the ramp under T1, the shift register's blanking, T2 frames, IFR
   polled: the spin detector), with JSR/BSR/RTS, PSHS/PULS/PULU, the
   indexed modes (auto increment, indirect, accumulator offsets, PC
   relative), 16-bit arithmetic, DAA, MUL, SEX, ABX, EXG/TFR (DP included),
   long branches and a direct page moved between the RAM and the VIA.
   A --meta-precise host project prints the interpreter's line (run_hash,
   registers but the flags, RAM hash) from the static walk alone and with
   the host's own coverage profile; the fast mode must draw the same
   picture's strokes count and run without an illegal opcode.
2. Random code (fixed seeds) after the header: the recompiled precise
   project against --interp, the same line.
3. The META project: the system ROM built in, src/vx_gen.c present, and
   with arm-none-eabi-gcc (PATH or PlatformIO's) vx_gen.c, the VIA (C and
   via_asm.S, whose offsets via.c asserts) and the dispatcher compile for
   the Cortex-M0+ (the firmware itself needs PlatformIO)."""

import os
import random
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
M0RECOMP = ROOT / "build" / "bin" / "m0recomp"

# ---- a two-pass 6809 assembler: the encodings the program uses ----
REL8 = {"bra": 0x20, "bhi": 0x22, "bls": 0x23, "bcc": 0x24, "bcs": 0x25, "bne": 0x26, "beq": 0x27,
        "bvc": 0x28, "bvs": 0x29, "bpl": 0x2A, "bmi": 0x2B, "bge": 0x2C, "blt": 0x2D, "bgt": 0x2E,
        "ble": 0x2F, "bsr": 0x8D}


def assemble(program, origin):
    """program: (label,) markers, raw byte lists, ("rel8", op, label),
    ("rel16", prefix_bytes, label), ("abs", prefix_bytes, label)"""
    labels, at = {}, origin
    for pas in range(2):
        out, at = bytearray(), origin
        for item in program:
            if isinstance(item, str):
                labels[item] = at
                continue
            if isinstance(item, (list, bytes)):
                out += bytes(item)
            elif item[0] == "rel8":
                target = labels.get(item[2], at + 2)
                off = target - (at + 2)
                assert pas == 0 or -128 <= off < 128, item
                out += bytes([REL8[item[1]], off & 0xFF])
            elif item[0] == "rel16":
                n = len(item[1]) + 2
                target = labels.get(item[2], at + n)
                out += bytes(item[1]) + ((target - (at + n)) & 0xFFFF).to_bytes(2, "big")
            elif item[0] == "abs":
                out += bytes(item[1]) + labels.get(item[2], 0).to_bytes(2, "big")
            at = origin + len(out)
    return bytes(out)


def header():
    # "g GCE", the music's pointer, no title line: the code at 14
    return b"g GCE 2026" + bytes([0x80, 0xFD, 0x0D, 0x00])


P = [
    "start",
    [0x10, 0xCE, 0xCB, 0xEA],             # LDS #$CBEA
    [0xCE, 0xCB, 0x80],                   # LDU #$CB80
    [0x86, 0xD0, 0x1F, 0x8B],             # LDA #$D0  TFR A,DP
    [0x86, 0x98, 0x97, 0x0B],             # ACR: T1 one shot on PB7, shift out under phi2
    [0x86, 0xCE, 0x97, 0x0C],             # PCR: not zeroing, blank under the shift register
    [0xCC, 0x00, 0x00, 0xFD, 0xC8, 0x82], # LDD #0 STD $C882
    [0x86, 0x7F, 0x97, 0x01, 0x86, 0x04, 0x97, 0x00, 0x0C, 0x00],  # brightness: ORA $7F (DAC $FF) on the mux's Z
    "frame",
    [0x86, 0x30, 0x97, 0x08, 0x86, 0x75, 0x97, 0x09],  # T2 = $7530
    ("abs", [0x8E], "lines"),             # LDX #lines
    [0x86, 0x04, 0xB7, 0xC8, 0x80],       # LDA #4 STA $C880
    "line",
    ("abs", [0xBD], "draw"),              # JSR draw
    [0x7A, 0xC8, 0x80],                   # DEC $C880
    ("rel8", "bne", "line"),
    ("rel8", "bsr", "mix"),
    "t2",
    [0x86, 0x20, 0x95, 0x0D],             # LDA #$20 BITA <$0D
    ("rel8", "beq", "t2"),
    [0x86, 0xCC, 0x97, 0x0C, 0x86, 0xCE, 0x97, 0x0C],  # zero the beam
    [0x7C, 0xC8, 0x81],                   # INC $C881
    ("rel16", [0x16], "frame"),           # LBRA frame
    # a stroke: (dy, dx) at X+, the ramp for T1's $40 cycles
    "draw",
    [0x34, 0x06],                         # PSHS A,B
    [0xA6, 0x80, 0x97, 0x01],             # LDA ,X+ STA <$01 (DAC: y)
    [0x0F, 0x00, 0x0C, 0x00],             # CLR <$00 INC <$00 (sample y)
    [0xA6, 0x80, 0xBB, 0xC8, 0x81, 0x97, 0x01],  # LDA ,X+ ADDA $C881 STA <$01 (x)
    [0x86, 0x40, 0x97, 0x04],             # T1 low latch
    [0x86, 0xFF, 0x97, 0x0A],             # shift register: lit
    [0x0F, 0x05],                         # CLR <$05: T1 runs, the ramp with it
    "t1",
    [0x86, 0x40, 0x95, 0x0D],             # LDA #$40 BITA <$0D
    ("rel8", "beq", "t1"),
    [0x0F, 0x0A],                         # CLR <$0A: blank
    [0x35, 0x86],                         # PULS A,B,PC
    # arithmetic with the direct page in the RAM
    "mix",
    [0x34, 0x7E],                         # PSHS A,B,DP,X,Y,U
    [0x86, 0xC8, 0x1F, 0x8B],             # LDA #$C8 TFR A,DP
    [0xDC, 0x82, 0xC3, 0x01, 0x23, 0xDD, 0x82],  # LDD <$82 ADDD #$0123 STD <$82
    [0x3D, 0x1D, 0x1E, 0x89],             # MUL SEX EXG A,B
    [0x8B, 0x19, 0x19],                   # ADDA #$19 DAA
    [0x10, 0x8E, 0xC8, 0x90],             # LDY #$C890
    ("abs", [0x8E], "table"),             # LDX #table
    [0xC6, 0x08],                         # LDB #8
    "copy",
    [0xA6, 0x80, 0x89, 0x03, 0xA7, 0xA0], # LDA ,X+ ADCA #3 STA ,Y+
    [0x5A],                               # DECB
    ("rel8", "bne", "copy"),
    [0x10, 0x8E, 0xC8, 0x90],             # LDY #$C890
    [0xA6, 0xA4, 0xE6, 0x21, 0xA6, 0xA5],  # LDA ,Y LDB 1,Y LDA B,Y
    [0xEC, 0xA1, 0x10, 0xAF, 0xC4],       # LDD ,Y++ STY ,U
    [0x10, 0xAE, 0xD4],                   # LDY [,U]
    [0x31, 0x3F, 0x3A, 0x30, 0x8C, 0x00], # LEAY -1,Y ABX LEAX 0,PCR
    [0x44, 0x56, 0x48, 0x59, 0x47, 0x40, 0x53, 0x4C, 0x4D],  # LSRA RORB ASLA ROLB ASRA NEGA COMB INCA TSTA
    [0x36, 0x16, 0x5F, 0x37, 0x16],       # PSHU X,B,A CLRB PULU A,B,X
    [0x0C, 0x84, 0x96, 0x84],             # INC <$84 LDA <$84
    [0xC2, 0x05, 0x88, 0x5A, 0x84, 0x7F, 0x8A, 0x01],  # SBCB #5 EORA #$5A ANDA #$7F ORA #1
    [0x10, 0x83, 0x40, 0x00],             # CMPD #$4000
    ("rel16", [0x10, 0x2D], "keep"),      # LBLT keep
    [0xCC, 0x00, 0x00, 0xDD, 0x82],       # LDD #0 STD <$82
    "keep",
    [0x8C, 0x12, 0x34],                   # CMPX #$1234
    ("rel8", "bgt", "big"),
    [0x1A, 0x01, 0x1C, 0xFE],             # ORCC #1 ANDCC #$FE
    "big",
    [0x35, 0xFE],                         # PULS A,B,DP,X,Y,U,PC
    "lines", [0x40, 0x00, 0x00, 0x40, 0xC0, 0x00, 0x00, 0xC0],
    "table", [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88],
]


def random_program(seed):
    """valid instructions at random, every addressing mode on index
    registers kept in the RAM, forward branches over the next instruction,
    balanced pushes and pulls, calls to random subroutines; in a loop"""
    rng = random.Random(seed)
    UN = [0, 3, 4, 6, 7, 8, 9, 10, 12, 13, 15]
    ALU = [0, 1, 2, 4, 5, 6, 8, 9, 10, 11]
    ram = lambda: rng.randrange(0xC900, 0xCA00)

    def postbyte(write):
        r = rng.randrange(3) << 5           # X, Y, U
        k = rng.randrange(6 if write else 12)  # a store never through a pointer
        if k == 0:
            return [r | rng.randrange(32)]  # 5-bit offset
        if k == 1:
            return [0x80 | r | rng.choice([0, 1, 2, 3])]  # ,R+ ,R++ ,-R ,--R
        if k == 2:
            return [0x80 | r | rng.choice([4, 5, 6, 11])]  # ,R B,R A,R D,R
        if k == 3:
            return [0x88 | r, rng.randrange(256)]
        if k == 4:
            return [0x89 | r, 0x00, rng.randrange(256)]
        if k == 5:
            return [0x8C, rng.randrange(256)]  # n8,PCR
        if k == 6:
            return [0x94 | r]                  # [,R]
        if k == 7:
            return [0x9F] + list(ram().to_bytes(2, "big"))  # [n16]
        return [0x80 | r | 4]

    def operand(mode, wide, write=False):
        if mode == 0:
            return [rng.randrange(256) for _ in range(2 if wide else 1)]
        if mode == 1:
            return [rng.randrange(256)]       # DP = $C8: the RAM
        if mode == 2:
            return postbyte(write)
        return list(ram().to_bytes(2, "big"))

    def one():
        k = rng.randrange(14)
        mode = rng.randrange(4)
        if k < 4:
            op = (0x80 if rng.randrange(2) == 0 else 0xC0) | mode << 4 | rng.choice(ALU)
            return [op] + operand(mode, False)
        if k == 4:
            op = rng.choice([0x83, 0xC3, 0x8C, 0xCC]) | mode << 4
            return [op] + operand(mode, True)
        if k == 5:
            pre = rng.choice([[0x10, 0x83], [0x10, 0x8C], [0x11, 0x83], [0x11, 0x8C]])
            return [pre[0], pre[1] | mode << 4] + operand(mode, True)
        if k == 6:
            if mode == 0:
                mode = 1
            return [rng.choice([0x97, 0xD7, 0xDD, 0x9F, 0xDF]) & 0xCF | mode << 4] + operand(mode, False, True)
        if k == 7:
            return [rng.choice([0x40, 0x50]) | rng.choice(UN)]
        if k == 8:
            m = rng.choice([1, 2, 3])
            return [(0x00, 0x60, 0x70)[m - 1] | rng.choice(UN)] + operand(m, False, True)
        if k == 9:
            return [rng.choice([0x19, 0x3D, 0x1D, 0x12, 0x3A])]
        if k == 10:
            # between the accumulators and CC, or the index registers
            # among themselves: they stay in the RAM
            a, b = rng.choice([8, 9, 10]), rng.choice([8, 9, 10])
            if rng.randrange(2):
                a, b = rng.choice([1, 2, 3]), rng.choice([1, 2, 3])
            return [rng.choice([0x1E, 0x1F]), a << 4 | b]
        if k == 11:
            return [rng.choice([0x1A, 0x1C]), rng.randrange(256) & 0xAF]  # never I nor F
        if k == 12:
            return [rng.choice([0x30, 0x31, 0x33]), rng.randrange(3) << 5 | rng.randrange(32)]  # LEAX/Y/U
        mode = mode or 1
        return [0x10, 0x8F | mode << 4] + operand(mode, False, True)  # STY

    def reset_index():
        return [0x8E, 0xC9, 0x40, 0x10, 0x8E, 0xC9, 0x80, 0xCE, 0xC9, 0xC0, 0x86, 0xC8, 0x1F, 0x8B]

    def block(n):
        out, pushed = bytearray(), []
        for _ in range(n):
            r = rng.randrange(20)
            if r == 0:
                ins = one()
                out += bytes([rng.choice(list(range(0x20, 0x30))), len(ins)]) + bytes(ins)
            elif r == 1:
                ins = one()
                out += bytes([0x10, rng.choice(list(range(0x22, 0x30))), 0, len(ins)]) + bytes(ins)
            elif r == 2 and len(pushed) < 3:
                m = rng.randrange(1, 0x80) & 0x7F
                pushed.append(m)
                out += bytes([0x34, m])
            elif r == 3:
                out += bytes(reset_index())
            else:
                out += bytes(one())
        for m in reversed(pushed):
            out += bytes([0x35, m])
        return out

    base = 14
    subs = [block(12) + bytes([0x39]) for _ in range(3)]
    main = bytearray([0x10, 0xCE, 0xCB, 0xEA]) + bytes(reset_index())
    loop = base + len(main)
    sub_at, calls = [], []
    for i in range(40):
        main += block(1 + rng.randrange(6)) if i % 4 else bytes(reset_index())
        if i % 5 == 4:
            calls.append(len(main))
            main += bytes([0xBD, 0, 0])      # JSR sub (patched)
    main += bytes([0x16, 0, 0])              # LBRA loop (patched)
    at = base + len(main)
    for sub in subs:
        sub_at.append(at)
        at += len(sub)
    for j, c in enumerate(calls):
        main[c + 1:c + 3] = sub_at[j % len(subs)].to_bytes(2, "big")
    off = (loop - (base + len(main))) & 0xFFFF
    main[-2:] = off.to_bytes(2, "big")
    return bytes(main) + b"".join(subs)


def system_rom():
    """8 KiB at E000: reset to the cartridge's code, the interrupts to an RTI"""
    rom = bytearray([0x12] * 8192)        # NOP
    rom[0x1000:0x1003] = bytes([0x7E, 0x00, 0x0E])  # F000: JMP $000E
    rom[0x1010] = 0x3B                    # F010: RTI
    for v in range(0x1FF2, 0x1FFE, 2):
        rom[v:v + 2] = bytes([0xF0, 0x10])
    rom[0x1FFE:0x2000] = bytes([0xF0, 0x00])
    return bytes(rom)


def run(*args, ok=True):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if ok and r.returncode:
        raise SystemExit(f"{args[0]} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def build_pc(cart, bios, out, *extra):
    run(M0RECOMP, cart, "--target", "pc", "--bios", bios, "-o", out, *extra)
    run("cmake", "-G", "Ninja", "-S", out, "-B", out / "build")
    run("ninja", "-C", out / "build")
    return out / "build" / "vectrex_pc"


def line(host, bios, frames, *extra):
    """the host's last two lines, the flags left out (the recompiled code
    computes only those a later reader sees)"""
    out = run(host, "--bios", bios, "--frames", frames, *extra)
    text = "\n".join(l for l in out.splitlines() if not l.startswith("native "))
    return re.sub(r" cc=[0-9a-f]+", "", text)


def sweep_cart():
    """The entry jumps through a RAM pointer (JMP [$C002]): the walk stops
    there; the dead bytes' JSR extended ($BD) and JMP extended ($7E) point
    at the handler only --vectrex-sweep reaches (the handler spins on its
    BRA self, both sides identically)."""
    cart = bytearray(header())
    handler = 0x400

    def put(*ws):
        for w in ws:
            cart.extend(w.to_bytes(2, "big"))

    put(0xcc, handler)          # LDD #handler
    put(0xf7, 0xc002)           # STD $C002
    put(0x6e, 0x9f, 0xc002)     # JMP [$C002]: the walk cannot follow
    put(0xbd, handler)          # JSR extended handler (dead bytes)
    put(0x7e, handler)          # JMP extended handler (dead bytes)
    while len(cart) < handler:
        cart.append(0xff)
    cart.extend(bytes((0x20, 0xfe)))  # the handler: BRA self
    return bytes(cart)


def main():
    if not shutil.which("cmake") or not shutil.which("ninja"):
        print("cmake/ninja missing: skipped")
        return
    probe = subprocess.run(["pkg-config", "--exists", "sdl2"])
    if probe.returncode:
        print("SDL2 missing: skipped")
        return
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        bios = root / "system.bin"
        bios.write_bytes(system_rom())

        # 1. the drawing program
        code = assemble(P, 14)
        cart = root / "draw.vec"
        cart.write_bytes(header() + code)
        interp = build_pc(cart, bios, root / "draw_interp", "--interp")
        ref = line(interp, bios, 120)
        assert "illegal=0" in ref and "segments=0 " not in ref, ref
        cov = root / "draw.cov"
        run(interp, "--bios", bios, "--frames", 120, "--coverage", cov)
        static = build_pc(cart, bios, root / "draw_static", "--meta-precise")
        assert line(static, bios, 120) == ref, (line(static, bios, 120), ref)
        state = root / "vectrex.state"
        line(static, bios, 60, "--save-state", state)
        assert line(static, bios, 60, "--load-state", state) == ref
        assert line(static, bios, 120, "--interp") == ref
        played = build_pc(cart, bios, root / "draw_played", "--meta-precise", "--coverage", cov)
        assert line(played, bios, 120) == ref
        stats = run(played, "--bios", bios, "--frames", 120)
        calls, steps = map(int, re.search(r"native calls=(\d+) interpreted=(\d+)", stats).groups())
        assert calls >= 120 and steps < 50, stats
        fast = build_pc(cart, bios, root / "draw_fast", "--coverage", cov)
        out = run(fast, "--bios", bios, "--frames", 120)
        assert "illegal=0" in out, out
        print(f"drawing program: precise = interpreter ({ref.splitlines()[-1]}), "
              f"{calls} native calls, {steps} instructions interpreted")

        # 2. random code
        native = 0
        for seed in range(8):
            cart = root / f"random{seed}.vec"
            cart.write_bytes(header() + random_program(seed))
            host = build_pc(cart, bios, root / f"random{seed}", "--meta-precise")
            a, b = line(host, bios, 30), line(host, bios, 30, "--interp")
            assert a == b, (seed, a, b)
            stats = run(host, "--bios", bios, "--frames", 30)
            calls, steps = map(int, re.search(r"native calls=(\d+) interpreted=(\d+)", stats).groups())
            native += steps < 1000
            print(f"  seed {seed}: {calls} native calls, {steps} interpreted, "
                  + re.search(r"illegal=\d+", stats).group())
        assert native >= 6, native
        print(f"random code: 8 seeds, precise = interpreter ({native} of them native throughout)")

        # 2b. the sweep and the explorer: the JSR/JMP extended target the
        # walk cannot prove compiles too (more instructions, the run does
        # not move) and two seeded explorations discover the same
        # addresses
        cart0 = root / "sweep.vec"
        cart0.write_bytes(sweep_cart())
        plain_text = run(M0RECOMP, cart0, "--target", "pc", "--bios", bios,
                         "-o", root / "sweep_plain", "--meta-precise")
        swept_text = run(M0RECOMP, cart0, "--target", "pc", "--bios", bios,
                         "-o", root / "sweep_swept", "--meta-precise", "--vectrex-sweep")
        run("cmake", "-G", "Ninja", "-S", root / "sweep_plain", "-B", root / "sweep_plain" / "build")
        run("ninja", "-C", root / "sweep_plain" / "build")
        run("cmake", "-G", "Ninja", "-S", root / "sweep_swept", "-B", root / "sweep_swept" / "build")
        run("ninja", "-C", root / "sweep_swept" / "build")
        swept_host = root / "sweep_swept" / "build" / "vectrex_pc"
        plain_host = root / "sweep_plain" / "build" / "vectrex_pc"
        assert line(swept_host, bios, 30) == line(plain_host, bios, 30), "the sweep moved the run"
        compiled = lambda t: int(re.search(r"(\d+) compiled", t).group(1))
        assert compiled(swept_text) > compiled(plain_text), \
            f"the sweep compiled nothing more ({compiled(swept_text)} vs {compiled(plain_text)})"
        explore_locations = None
        for _ in range(2):
            ecov = root / "explore.cov"
            run(interp, "--bios", bios, "--frames", "900", "--random", "3", "--coverage", ecov)
            found = [l for l in ecov.read_text().splitlines() if not l.startswith("#")]
            assert len(found) > 5, f"the explorer discovered almost nothing ({len(found)})"
            assert explore_locations in (None, len(found)), "unstable explore coverage"
            explore_locations = len(found)
        print(f"sweep + explorer: {compiled(swept_text) - compiled(plain_text)} swept instructions, "
              f"{explore_locations} explored locations")

        # 3. the META project
        meta = root / "draw_meta"
        run(M0RECOMP, root / "draw.vec", "--target", "meta", "--bios", bios, "-o", meta, "--coverage", cov)
        assert (meta / "src" / "vx_gen.c").is_file()
        assert "#define VX_NATIVE 1" in (meta / "src" / "vx_config.h").read_text()
        assert "vectrex_bios[8192]" in (meta / "src" / "vx_cart.c").read_text()
        assert (meta / "src" / "m0_state.cpp").is_file()
        states = list(meta.glob("*.ST8"))
        assert len(states) == 1 and states[0].stat().st_size == 10 * 24 * 512
        gcc = shutil.which("arm-none-eabi-gcc")
        pio_gcc = Path.home() / ".platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc"
        if not gcc and pio_gcc.is_file():
            gcc = str(pio_gcc)
        if gcc:
            run(gcc, "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-Wall", "-Werror", "-c", "-I", meta / "src",
                meta / "src" / "vx_gen.c", "-o", root / "vx_gen.o")
            # the VIA in Thumb-1: via.c's _Static_asserts pin via_asm.S's offsets
            for name in ("via.c", "via_asm.S", "m6809_run.c"):
                run(gcc, "-mcpu=cortex-m0plus", "-mthumb", "-O2", "-Wall", "-Werror", "-c", "-I", meta / "src",
                    meta / "src" / name, "-o", root / (name + ".o"))
            print("META: vx_gen.c, via.c, via_asm.S and m6809_run.c compile for the Cortex-M0+")
        else:
            print("META: arm-none-eabi-gcc missing, the compile check skipped")


if __name__ == "__main__":
    main()
