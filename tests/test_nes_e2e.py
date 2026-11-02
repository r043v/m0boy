#!/usr/bin/env python3
"""NES end to end, on synthetic NROM cartridges.

1. The picture: one striped tile over a blue backdrop, sprite 0 over it,
   generated as an interpreter-only host project; two runs give the same
   run_hash, the capture shows the tile over the backdrop, and the panel
   renderer (160x128) equals the reference lines' box filter on every row
   with the same sprite-0 hits, every frame shown or one in two left out,
   and so does the Pokitto's plan (--rows 176, 240 -> 176).
2. The recompiler: a program that exercises the documented instructions
   (ADC/SBC, shifts, BIT, page-crossing indexed reads, (zp),Y and (zp,X),
   JSR/RTS, JMP through RAM, PHP/PLP, the NMI and RTI, OAM DMA, a routine
   copied to RAM and run by the interpreter).  A --meta-precise project
   must print the interpreter's run_hash, from the static walk alone and
   with the harness's own coverage profile; the fast mode must run.
3. The Pokitto: the project carries the Pokitto's drivers and no META file;
   with arm-none-eabi-gcc (PATH or PlatformIO's) the firmware links below
   the loader at 0x39000 with a valid vector checksum."""

import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GBRECOMP = ROOT / "build" / "bin" / "gbrecomp"

# reset at $8000: no NMI, backdrop $22, tile 1 at $2000, sprite 0 over it,
# rendering on, idle
PRG = bytes([
    0x78, 0xD8,                                     # SEI CLD
    0xA9, 0x00, 0x8D, 0x00, 0x20,                   # LDA #0 STA $2000
    0xA9, 0x3F, 0x8D, 0x06, 0x20,                   # LDA #$3F STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x22, 0x8D, 0x07, 0x20,                   # LDA #$22 STA $2007
    0xA9, 0x20, 0x8D, 0x06, 0x20,                   # LDA #$20 STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x01, 0x8D, 0x07, 0x20,                   # LDA #1  STA $2007
    0xA9, 0x00, 0x8D, 0x03, 0x20,                   # LDA #0  STA $2003: sprite 0 ...
    0xA9, 0x02, 0x8D, 0x04, 0x20,                   # y 2 (lines 3-10)
    0xA9, 0x01, 0x8D, 0x04, 0x20,                   # tile 1, over the background's tile 1
    0xA9, 0x00, 0x8D, 0x04, 0x20,                   # in front, palette 4
    0xA9, 0x04, 0x8D, 0x04, 0x20,                   # x 4: sprite 0 hits
    0xA9, 0x1E, 0x8D, 0x01, 0x20,                   # LDA #$1E STA $2001 (both layers, both left columns)
    0x4C, 0x46, 0x80,                               # JMP $8046 (the pad poll loop)
]).ljust(32768, b"\xEA")                            # NOP fill
# the idle loop polls the pads and marks RAM $10 while A is held: a run's
# hash follows its input, the recording round trips lean on that
PRG = PRG[:0x46] + bytes([
    0xA9, 0x01, 0x8D, 0x16, 0x40,                   # LDA #1 STA $4016: the pads latch
    0xA9, 0x00, 0x8D, 0x16, 0x40,                   # strobe off
    0xAD, 0x16, 0x40,                               # LDA $4016 (A first)
    0x29, 0x01,                                     # AND #$01
    0xF0, 0x03,                                     # not held: past the mark
    0x8D, 0x10, 0x00,                               # STA $0010
    0x4C, 0x46, 0x80,                               # again
]) + PRG[0x5D:]
# vectors at the end of the last 16 KiB bank
PRG = PRG[:-6] + bytes([0x00, 0x80, 0x00, 0x80, 0x00, 0x80])  # NMI, RESET, IRQ

CHR = bytearray(8192)
for r in range(8):                                    # tile 1: every row striped
    CHR[16 + r] = 0b01010101                          # lo plane
    CHR[24 + r] = 0b01010101                          # hi plane: colour 3
ROM = bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8) + PRG + bytes(CHR)

# ---- a two-pass 6502 assembler, just what the exercise program needs ----
OPS = {
    "adc": {"imm": 0x69, "zp": 0x65, "zpx": 0x75, "abs": 0x6D, "absx": 0x7D, "absy": 0x79, "izx": 0x61, "izy": 0x71},
    "sbc": {"imm": 0xE9, "zp": 0xE5, "zpx": 0xF5, "abs": 0xED, "absx": 0xFD, "absy": 0xF9, "izx": 0xE1, "izy": 0xF1},
    "and": {"imm": 0x29, "zp": 0x25, "absx": 0x3D, "izy": 0x31},
    "ora": {"imm": 0x09, "zp": 0x05, "absy": 0x19, "izx": 0x01},
    "eor": {"imm": 0x49, "zp": 0x45, "zpx": 0x55, "abs": 0x4D},
    "cmp": {"imm": 0xC9, "zp": 0xC5, "absx": 0xDD, "izy": 0xD1},
    "cpx": {"imm": 0xE0, "zp": 0xE4}, "cpy": {"imm": 0xC0, "abs": 0xCC},
    "bit": {"zp": 0x24, "abs": 0x2C},
    "lda": {"imm": 0xA9, "zp": 0xA5, "zpx": 0xB5, "abs": 0xAD, "absx": 0xBD, "absy": 0xB9, "izx": 0xA1, "izy": 0xB1},
    "ldx": {"imm": 0xA2, "zp": 0xA6, "zpy": 0xB6, "abs": 0xAE, "absy": 0xBE},
    "ldy": {"imm": 0xA0, "zp": 0xA4, "zpx": 0xB4, "abs": 0xAC, "absx": 0xBC},
    "sta": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D, "absx": 0x9D, "absy": 0x99, "izx": 0x81, "izy": 0x91},
    "stx": {"zp": 0x86, "zpy": 0x96, "abs": 0x8E}, "sty": {"zp": 0x84, "zpx": 0x94, "abs": 0x8C},
    "asl": {"acc": 0x0A, "zp": 0x06, "absx": 0x1E}, "lsr": {"acc": 0x4A, "zp": 0x46, "zpx": 0x56},
    "rol": {"acc": 0x2A, "zp": 0x26, "abs": 0x2E}, "ror": {"acc": 0x6A, "zp": 0x66, "absx": 0x7E},
    "inc": {"zp": 0xE6, "zpx": 0xF6, "abs": 0xEE}, "dec": {"zp": 0xC6, "absx": 0xDE},
    "jmp": {"abs": 0x4C, "ind": 0x6C}, "jsr": {"abs": 0x20},
}
IMPLIED = {"inx": 0xE8, "iny": 0xC8, "dex": 0xCA, "dey": 0x88, "tax": 0xAA, "tay": 0xA8, "txa": 0x8A,
           "tya": 0x98, "tsx": 0xBA, "txs": 0x9A, "pha": 0x48, "php": 0x08, "pla": 0x68, "plp": 0x28,
           "clc": 0x18, "sec": 0x38, "cli": 0x58, "sei": 0x78, "clv": 0xB8, "cld": 0xD8, "sed": 0xF8,
           "rts": 0x60, "rti": 0x40, "nop": 0xEA}
BRANCH = {"bpl": 0x10, "bmi": 0x30, "bvc": 0x50, "bvs": 0x70, "bcc": 0x90, "bcs": 0xB0, "bne": 0xD0, "beq": 0xF0}


def assemble(source, origin=0x8000):
    lines = [l.split(";")[0].strip() for l in source.strip().splitlines()]
    labels = {}
    for final in (False, True):
        pc, out = origin, bytearray()

        def value(text):
            text = text.strip()
            if text.startswith("<"):
                return value(text[1:]) & 0xFF
            if text.startswith(">"):
                return value(text[1:]) >> 8
            if text.startswith("$"):
                return int(text[1:], 16)
            if text[0].isdigit():
                return int(text)
            return labels.get(text, 0) if not final else labels[text]

        for line in lines:
            if not line:
                continue
            if line.endswith(":"):
                labels[line[:-1]] = pc
                continue
            op, _, arg = line.partition(" ")
            arg = arg.replace(" ", "")
            if op == ".byte":
                code = bytes(value(v) for v in arg.split(","))
            elif op in IMPLIED:
                code = bytes([IMPLIED[op]])
            elif op in BRANCH:
                off = (value(arg) - (pc + 2)) if final else 0
                assert -128 <= off <= 127, line
                code = bytes([BRANCH[op], off & 0xFF])
            elif arg in ("", "a"):
                code = bytes([OPS[op]["acc"]])
            elif arg.startswith("#"):
                code = bytes([OPS[op]["imm"], value(arg[1:])])
            elif arg.startswith("(") and arg.endswith(",x)"):
                code = bytes([OPS[op]["izx"], value(arg[1:-3])])
            elif arg.startswith("(") and arg.endswith("),y"):
                code = bytes([OPS[op]["izy"], value(arg[1:-3])])
            elif arg.startswith("("):
                v = value(arg[1:-1])
                code = bytes([OPS[op]["ind"], v & 0xFF, v >> 8])
            else:
                base, _, index = arg.partition(",")
                v = value(base)
                zp = (base.startswith("$") and len(base) <= 3) and op not in ("jmp", "jsr")
                mode = ("zp" if zp else "abs") + index
                if mode not in OPS[op] and zp:
                    mode = "abs" + index
                if mode.startswith("zp"):
                    code = bytes([OPS[op][mode], v])
                else:
                    code = bytes([OPS[op][mode], v & 0xFF, v >> 8])
            out += code
            pc += len(code)
    return bytes(out), labels


EXERCISE = """
reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$F0          ; ($20),Y and ($22,X) pointers
    sta $20
    lda #$02
    sta $21
    lda #$80
    sta $23
    lda #$E6          ; INC $1C / RTS, copied to RAM: interpreted
    sta $0500
    lda #$1C
    sta $0501
    lda #$60
    sta $0502
    lda #$80          ; NMI on
    sta $2000
    lda #$1E
    sta $2001
loop:
    inc $10
    lda $10
    clc
    adc $11
    sta $11
    sec
    sbc #$37
    eor $12
    sta $12
    rol a
    ror $13
    asl $14
    lsr $15
    rol $0315
    bit $12
    bvc novf
    inc $16
novf:
    bmi neg
    dec $0316,x
neg:
    ldx $10
    lda table,x       ; crosses a page once X passes $0F
    sta $0300,x
    ldy $11
    lda ($20),y       ; crosses when Y passes $0F
    adc $0300,y
    sta ($20),y
    lda ($22,x)
    and table,x
    ora $0310,y
    cmp ($20),y
    ror $0320,x
    asl $0330,x
    jsr sub
    php
    pla
    sta $17
    pha
    plp
    lda #<back        ; JMP through RAM
    sta $40
    lda #>back
    sta $41
    cpx $11
    cpy #$80
    bcc low
    inc $18
low:
    jmp ($0040)
back:
    dex
    iny
    tya
    tax
    tsx
    txs
    jsr $0500         ; interpreted, returns into compiled code
    lda $10
    and #$07
    bne loop
    lda $2002         ; a register read with a side effect
    sta $1D
    sed               ; decimal is ignored by the 2A03
    lda #$09
    adc #$01
    cld
    sta $1E
    jmp loop
sub:
    lda $12
    and #$0F
    ora #$40
    tay
    dec $19
    bne subdone
    inc $1A
subdone:
    rts
nmi:
    pha
    txa
    pha
    inc $1B
    lda #$03          ; OAM DMA from $0300: the CPU stalls
    sta $4014
    lda $1B
    sta $2005
    sta $2005
    pla
    tax
    pla
    rti
irq:
    rti
table:
"""


def exercise_rom():
    code, labels = assemble(EXERCISE)
    table = bytes((i * 37 + 11) & 0xFF for i in range(256))
    prg = (code + table).ljust(32768, b"\xEA")
    vectors = bytes([labels["nmi"] & 0xFF, labels["nmi"] >> 8, labels["reset"] & 0xFF, labels["reset"] >> 8,
                     labels["irq"] & 0xFF, labels["irq"] >> 8])
    prg = prg[:-6] + vectors
    return bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8) + prg + bytes(CHR)


def run(*args, ok=True):
    out = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (out.returncode == 0) != ok:
        print(out.stdout, out.stderr, sep="\n")
        raise SystemExit(f"unexpected return {out.returncode}: {' '.join(str(a) for a in args)}")
    return out.stdout + out.stderr


def run_hash(text):
    return [t for t in text.split() if t.startswith("run_hash=")][0]


def panel_ok(text, rows):
    """the harness's panel check: every row equal to the reference's"""
    fields = dict(t.split("=", 1) for t in text.split() if "=" in t)
    assert fields.get("out_mismatch") == "0" and fields.get("hit_mismatch") == "0", text
    assert int(fields["panel_rows"]) == rows, text


def project(rom, out, *options):
    run(GBRECOMP, "--system", "nes", rom, "--target", "desktop", "--output-prefix", "synth", "-o", out, *options)
    build = out / "host" / "build"
    run("cmake", "-G", "Ninja", "-S", out / "host", "-B", build, "-DCMAKE_BUILD_TYPE=Release")
    run("ninja", "-C", build)
    return build / "synth_host"


def picture(work):
    rom = work / "synthetic.nes"
    rom.write_bytes(ROM)
    host = project(rom, work / "proj", "--nes-interp")
    out = run(host, "--frames", "60")
    panel_ok(out, 60 * 128)
    panel_ok(run(host, "--frames", "60", "--skip", "2"), 30 * 128)
    panel_ok(run(host, "--frames", "60", "--rows", "176"), 60 * 176)
    panel_ok(run(host, "--frames", "60", "--rows", "176", "--skip", "2"), 30 * 176)
    h1 = run_hash(out)
    h2 = run_hash(run(host, "--frames", "60"))
    assert h1 == h2 and h1 != "run_hash=00000000", f"unstable hash: {h1} {h2}"
    state = work / "nes.state"
    run(host, "--frames", "30", "--save-state", state)
    resumed = run(host, "--frames", "30", "--load-state", state)
    panel_ok(resumed, 30 * 128)
    assert run_hash(resumed) == h1, "NES state did not resume the same machine"

    run(host, "--frames", "60", "--ppm", work / "shot.ppm")
    body = (work / "shot.ppm").read_bytes().split(b"\n255\n", 1)[1]
    px = body[(6 * 256 + 128) * 3:(6 * 256 + 128) * 3 + 3]
    # the backdrop $22 through the blank tile 0: the emulator's blue
    assert abs(px[0] - 104) < 40 and abs(px[2] - 252) < 40, f"backdrop wrong: {tuple(px)}"
    px = body[(0 * 256 + 1) * 3:(0 * 256 + 1) * 3 + 3]
    # the tile sits at nametable entry 0 (screen columns 0-7); its pixel 1
    # is the stripe: colour index 3 of palette 0 = $00, grey — any
    # non-backdrop value proves the tile walk
    assert abs(int(px[0]) - 104) > 40 or abs(int(px[2]) - 252) > 40, f"tile pixel wrong: {tuple(px)}"
    return h1


def recompiler(work):
    rom = work / "exercise.nes"
    rom.write_bytes(exercise_rom())
    frames = ("--frames", "120")
    host = project(rom, work / "precise", "--meta-precise")
    reference = run_hash(run(host, *frames, "--interp"))
    out = run(host, *frames)
    panel_ok(out, 120 * 128)
    native = run_hash(out)
    out = run(host, *frames, "--rows", "176")
    panel_ok(out, 120 * 176)
    assert run_hash(out) == native, "the Pokitto's vertical plan changed the run"
    assert native == reference, f"precise static walk {native} != interpreter {reference}"
    state = work / "native.state"
    run(host, "--frames", "60", "--save-state", state)
    resumed = run(host, "--frames", "60", "--load-state", state)
    assert run_hash(resumed) == native, "native NES state did not resume precisely"
    coverage = work / "exercise.cov"
    run(host, *frames, "--coverage", coverage)
    assert " d" in coverage.read_text(), "no dynamic entry in the coverage file"
    host = project(rom, work / "profiled", "--meta-precise", "--coverage", coverage)
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise with coverage {native} != interpreter {reference}"
    host = project(rom, work / "fast", "--coverage", coverage)
    out = run(host, *frames)
    assert run_hash(out) != "run_hash=00000000", out

    # the sweep: the PRG's JSR/JMP targets outside the walk compile too
    # (more instructions than the walk alone) and the run does not move
    plain_text = run(GBRECOMP, "--system", "nes", rom, "--target", "desktop",
                     "--output-prefix", "synth", "-o", work / "precise", "--meta-precise")
    swept_text = run(GBRECOMP, "--system", "nes", rom, "--target", "desktop",
                     "--output-prefix", "synth", "-o", work / "sweep", "--meta-precise", "--nes-sweep")
    build = work / "sweep" / "host" / "build"
    run("cmake", "-G", "Ninja", "-S", work / "sweep" / "host", "-B", build, "-DCMAKE_BUILD_TYPE=Release")
    run("ninja", "-C", build)
    swept = run_hash(run(build / "synth_host", *frames))
    assert swept == reference, f"precise with sweep {swept} != interpreter {reference}"
    compiled = lambda t: int(re.search(r"(\d+) compiled", t).group(1))
    assert compiled(swept_text) > compiled(plain_text), \
        f"the sweep compiled nothing more ({compiled(swept_text)} vs {compiled(plain_text)})"

    # the explorer: two seeded runs discover the same addresses
    explore_locations = None
    for _ in range(2):
        cov = work / "explore.cov"
        run(host, "--explore", "900", "--random", "3", "--coverage", cov)
        found = [l for l in cov.read_text().splitlines() if not l.startswith("#")]
        assert len(found) > 10, f"the explorer discovered almost nothing ({len(found)})"
        assert explore_locations in (None, len(found)), "unstable explore coverage"
        explore_locations = len(found)
    return reference


def toolchain():
    """the Pokitto firmware needs arm-none-eabi-gcc: PATH or PlatformIO's"""
    if shutil.which("arm-none-eabi-gcc"):
        return ""
    pio = Path.home() / ".platformio" / "packages" / "toolchain-gccarmnoneeabi" / "bin"
    return str(pio) if (pio / "arm-none-eabi-gcc").exists() else None


def pokitto(work):
    rom = work / "exercise.nes"
    out = work / "pokitto"
    run(GBRECOMP, "--system", "nes", rom, "--target", "pokitto", "--output-prefix", "synth", "-o", out)
    for name in ("CMakeLists.txt", "pokitto.ld", "checksum.py", "arm-toolchain.cmake", "src/nes_main.cpp",
                 "src/pokitto_lcd.c", "src/pokitto_lcd_asm.S", "src/pokitto_audio.c", "src/pokitto_sd.c",
                 "src/m0_state.cpp", "src/nes_gen.c"):
        assert (out / name).is_file(), name
    for name in ("platformio.ini", "src/meta_lcd.cpp", "src/meta_sd_bus.cpp"):
        assert not (out / name).exists(), name
    bin_dir = toolchain()
    if bin_dir is None:
        return "firmware skipped: no arm-none-eabi-gcc"
    env = dict(os.environ, PATH=bin_dir + os.pathsep + os.environ["PATH"]) if bin_dir else None
    for cmd in (["cmake", "-G", "Ninja", "-S", out, "-B", out / "build", "-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake"],
                ["ninja", "-C", out / "build"]):
        proc = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env)
        assert proc.returncode == 0, proc.stdout[-4000:] + proc.stderr[-4000:]
    fw = (out / "build" / "firmware.bin").read_bytes()
    assert len(fw) <= 0x39000, f"firmware is {len(fw)} bytes, above the loader at 0x39000"
    words = struct.unpack_from("<8I", fw, 0)
    assert sum(words) & 0xFFFFFFFF == 0, "vector checksum"
    assert 0x10000100 < words[0] <= 0x10008000 and words[1] & 1, "vectors"
    assert b"m0boy-nes-pokitto " in fw, "version string missing"
    return f"firmware {len(fw)} bytes"


def unrom_rom():
    """A UxROM cartridge, 4 banks of 16 KiB and CHR-RAM: the fixed bank
    (bank 3, at $C000-$FFFF) uploads its tile to the CHR-RAM through $2007,
    calls a worker in the $8000 window under bank 1, then under bank 2 —
    the same address, two instructions — and idles for its NMI.  Each
    worker counts its own RAM cell and folds a read of its own bank's data
    at $8100; bank 2's also calls a helper in the fixed window and returns
    into banked code."""
    fixed, labels = assemble("""
reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$00
    sta $2000
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    lda #$21          ; the backdrop
    sta $2007
    lda #$16          ; colour 1: the tile's stripes
    sta $2007
    lda #$00
    sta $2006
    lda #$10
    sta $2006         ; the CHR-RAM's tile 1 (pattern $0010), 16 bytes through $2007
    ldx #$00
chr:
    lda tile,x
    sta $2007
    inx
    cpx #$10
    bne chr
    lda #$20
    sta $2006
    lda #$00
    sta $2006         ; the nametable's first entry: tile 1
    lda #$01
    sta $2007
    lda #$1E
    sta $2001         ; both layers on
    lda #$00
    sta $10
    sta $11
    sta $12
    sta $13
    sta $14
    sta $15
    lda #$80
    sta $2000         ; NMI on
main:
    lda #$01
    sta $FFF0         ; bank 1 into the $8000 window
    jsr $8000
    lda #$02
    sta $FFF0         ; bank 2: the same address, another worker
    jsr $8000
    jmp main
nmi:
    pha
    inc $12
    pla
    rti
irq:
    rti
tile:
    .byte $FF, $00, $FF, $00, $FF, $00, $FF, $00
    .byte $01, $00, $01, $00, $01, $00, $01, $00
""", origin=0xC000)
    w1, l1 = assemble("""
worker:
    inc $10
    lda $8100         ; bank 1's own data through the window
    sta $13
    rts
    .byte $AA, $BB, $CC, $DD
""")
    w2, l2 = assemble("""
worker:
    inc $11
    lda $8100         ; bank 2's data: the same fold, another bank
    sta $14
    jsr $C100         ; the fixed window's helper, and back
    rts
""")
    helper, lh = assemble("""
helper:
    inc $15
    rts
""", origin=0xC100)

    def bank(code, fill=b"\xEA"):
        assert len(code) <= 16384
        return code.ljust(16384, fill)

    b1, b2, b3 = bank(w1), bank(w2), bytearray(bank(fixed))
    b3[0x100:0x100 + len(helper)] = helper  # the helper at $C100, as bank 2 calls it
    # each bank's own data at $8100, where its worker folds a read
    b1 = bytearray(b1); b2 = bytearray(b2)
    b1[0x100:0x104] = bytes([0x40, 0x41, 0x42, 0x43])
    b2[0x100:0x104] = bytes([0x80, 0x81, 0x82, 0x83])
    vectors = bytes([labels["nmi"] & 0xFF, labels["nmi"] >> 8,
                     labels["reset"] & 0xFF, labels["reset"] >> 8,
                     labels["irq"] & 0xFF, labels["irq"] >> 8])
    b3 = bytes(b3[:-6]) + vectors
    # bank 0 is a filler the game never maps; workers land in banks 1 and 2
    return bytes([0x4E, 0x45, 0x53, 0x1A, 4, 0, 0x20, 0x00] + [0] * 8) + b"\xEA" * 16384 + b1 + b2 + b3


def unrom(work):
    """the banked cartridge: the picture, the banked coverage, and the
    recompiled code under its bank contexts (static and profiled) equal
    the interpreter"""
    rom = work / "unrom.nes"
    rom.write_bytes(unrom_rom())
    frames = ("--frames", "120")
    host = project(rom, work / "unrom-interp", "--nes-interp")
    out = run(host, *frames)
    panel_ok(out, 120 * 128)
    reference = run_hash(out)
    assert reference == run_hash(run(host, *frames)), "unstable run"
    run(host, *frames, "--ppm", work / "unrom.ppm")
    body = (work / "unrom.ppm").read_bytes().split(b"\n255\n", 1)[1]
    px = body[(0 * 256 + 6) * 3:(0 * 256 + 6) * 3 + 3]
    # the CHR-RAM tile's first row: colour 1 of palette 0 ($16, red)
    assert px[0] > 150 and px[1] < 100, f"the CHR-RAM tile did not show: {tuple(px)}"
    px = body[(1 * 256 + 6) * 3:(1 * 256 + 6) * 3 + 3]
    # its second row is colour 0: the backdrop ($21, blue)
    assert px[2] > 200 and px[0] < 100, f"the backdrop did not show: {tuple(px)}"

    coverage = work / "unrom.cov"
    run(host, *frames, "--coverage", coverage)
    text = coverage.read_text()
    for line in (":01 ", ":02 "):
        assert line in text, f"no banked coverage line '{line}...' in the file"
    assert " d" in text, "no dynamic entry in the coverage file"

    host = project(rom, work / "unrom-precise", "--meta-precise")
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise static walk {native} != interpreter {reference}"
    host = project(rom, work / "unrom-profiled", "--meta-precise", "--coverage", coverage)
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise with coverage {native} != interpreter {reference}"

    state = work / "unrom.state"
    run(host, "--frames", "60", "--save-state", state)
    resumed = run(host, "--frames", "60", "--load-state", state)
    assert run_hash(resumed) == native, "native UNROM state did not resume precisely"
    return reference


def main():
    work = Path(tempfile.mkdtemp(prefix="nes_e2e_"))
    h = picture(work)
    r = recompiler(work)
    u = unrom(work)
    p = pokitto(work)
    print("ok: NES e2e (picture stable", h + "; recompiled precise = interpreter", r + ", static and profiled; UNROM",
          u + "; Pokitto", p + ")")


if __name__ == "__main__":
    main()
