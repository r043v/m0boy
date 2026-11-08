#!/usr/bin/env python3
"""PC Engine end to end, on synthetic HuCards.

1. The picture: one striped character over a green backdrop, generated as
   an interpreter host project; two runs give the same run_hash and the
   capture shows the character over the backdrop.  The same card as a
   --meta-precise project prints the interpreter's hash (a straight line
   and a polled idle: every instruction the same).
2. The recompiler: a program that exercises the HuC6280 beyond the boot
   (TAM bank switching between ROM pages, the T flag, decimal mode, the
   block moves, BSR, BBR/BBS, TST, TSB/TRB, RMB/SMB, the (ind) forms,
   jumps through pointers, a routine copied to WRAM, the timer interrupt
   with RTI, an undocumented opcode).  A --meta-precise project must
   print the interpreter's run_hash — from the static walk alone, then
   with the harness's own coverage profile (whose contexts and dynamic
   entries compile what the walk could not prove); the fast mode must run.
"""

import json
import re
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GBRECOMP = ROOT / "build" / "bin" / "gbrecomp"

# ---- a two-pass HuC6280 assembler, just what the exercise card needs ----

OPS = {
    "lda": {"imm": 0xA9, "zp": 0xA5, "zpx": 0xB5, "abs": 0xAD, "absx": 0xBD, "absy": 0xB9,
            "ind": 0xB2, "izx": 0xA1, "izy": 0xB1},
    "sta": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D, "absx": 0x9D, "absy": 0x99,
            "ind": 0x92, "izx": 0x81, "izy": 0x91},
    "ldx": {"imm": 0xA2, "zp": 0xA6, "zpy": 0xB6, "abs": 0xAE, "absy": 0xBE},
    "ldy": {"imm": 0xA0, "zp": 0xA4, "zpx": 0xB4, "abs": 0xAC, "absx": 0xBC},
    "stx": {"zp": 0x86, "zpy": 0x96, "abs": 0x8E},
    "sty": {"zp": 0x84, "zpx": 0x94, "abs": 0x8C},
    "stz": {"zp": 0x64, "zpx": 0x74, "abs": 0x9C, "absx": 0x9E},
    "adc": {"imm": 0x69, "zp": 0x65, "zpx": 0x75, "abs": 0x6D, "absx": 0x7D, "absy": 0x79,
            "ind": 0x72, "izx": 0x61, "izy": 0x71},
    "sbc": {"imm": 0xE9, "zp": 0xE5, "zpx": 0xF5, "abs": 0xED, "absx": 0xFD, "absy": 0xF9,
            "ind": 0xF2, "izx": 0xE1, "izy": 0xF1},
    "and": {"imm": 0x29, "zp": 0x25, "zpx": 0x35, "abs": 0x2D, "absx": 0x3D, "absy": 0x39,
            "ind": 0x32, "izx": 0x21, "izy": 0x31},
    "ora": {"imm": 0x09, "zp": 0x05, "zpx": 0x15, "abs": 0x0D, "absx": 0x1D, "absy": 0x19,
            "ind": 0x12, "izx": 0x01, "izy": 0x11},
    "eor": {"imm": 0x49, "zp": 0x45, "zpx": 0x55, "abs": 0x4D, "absx": 0x5D, "absy": 0x59,
            "ind": 0x52, "izx": 0x41, "izy": 0x51},
    "cmp": {"imm": 0xC9, "zp": 0xC5, "zpx": 0xD5, "abs": 0xCD, "absx": 0xDD, "absy": 0xD9,
            "ind": 0xD2, "izx": 0xC1, "izy": 0xD1},
    "cpx": {"imm": 0xE0, "zp": 0xE4, "abs": 0xEC},
    "cpy": {"imm": 0xC0, "zp": 0xC4, "abs": 0xCC},
    "bit": {"imm": 0x89, "zp": 0x24, "zpx": 0x34, "abs": 0x2C, "absx": 0x3C},
    "asl": {"a": 0x0A, "zp": 0x06, "zpx": 0x16, "abs": 0x0E, "absx": 0x1E},
    "lsr": {"a": 0x4A, "zp": 0x46, "zpx": 0x56, "abs": 0x4E, "absx": 0x5E},
    "rol": {"a": 0x2A, "zp": 0x26, "zpx": 0x36, "abs": 0x2E, "absx": 0x3E},
    "ror": {"a": 0x6A, "zp": 0x66, "zpx": 0x76, "abs": 0x6E, "absx": 0x7E},
    "inc": {"zp": 0xE6, "zpx": 0xF6, "abs": 0xEE, "absx": 0xFE},
    "dec": {"zp": 0xC6, "zpx": 0xD6, "abs": 0xCE, "absx": 0xDE},
    "tsb": {"zp": 0x04, "abs": 0x0C},
    "trb": {"zp": 0x14, "abs": 0x1C},
    "jmp": {"abs": 0x4C, "ind": 0x6C, "absx": 0x7C},
    "jsr": {"abs": 0x20},
}
IMPLIED = {"inx": 0xE8, "iny": 0xC8, "dex": 0xCA, "dey": 0x88, "ina": 0x1A, "dea": 0x3A,
           "tax": 0xAA, "tay": 0xA8, "txa": 0x8A, "tya": 0x98, "tsx": 0xBA, "txs": 0x9A,
           "pha": 0x48, "php": 0x08, "phx": 0xDA, "phy": 0x5A,
           "pla": 0x68, "plp": 0x28, "plx": 0xFA, "ply": 0x7A,
           "clc": 0x18, "sec": 0x38, "cli": 0x58, "sei": 0x78, "clv": 0xB8, "cld": 0xD8,
           "sed": 0xF8, "set": 0xF4, "cla": 0x62, "clx": 0x82, "cly": 0xC2,
           "sxy": 0x02, "sax": 0x22, "say": 0x42, "csl": 0x54, "csh": 0xD4,
           "nop": 0xEA, "rts": 0x60, "rti": 0x40, "st0": 0x03, "st1": 0x13, "st2": 0x23}
BRANCH = {"bra": 0x80, "bpl": 0x10, "bmi": 0x30, "bvc": 0x50, "bvs": 0x70,
          "bcc": 0x90, "bcs": 0xB0, "bne": 0xD0, "beq": 0xF0}
BLOCK = {"tii": 0x73, "tdd": 0xC3, "tin": 0xD3, "tia": 0xE3, "tai": 0xF3}


def assemble(source, origin, labels0=None, labels_only=False):
    lines = [l.split(";")[0].strip() for l in source.strip().splitlines()]
    labels = dict(labels0 or {})

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
        if "+" in text:
            base, _, off = text.partition("+")
            return (value(base) + value(off)) & 0xFFFF
        return labels.get(text, 0) if not final else labels[text]

    for final in (False, True):
        pc = origin
        out = bytearray()
        for line in lines:
            if not line:
                continue
            if line.endswith(":"):
                labels[line[:-1]] = pc
                continue
            op, _, arg = line.partition(" ")
            arg = arg.replace(" ", "")
            if op == ".org":
                pc = value(arg)
                out += b"\xFF" * (pc - origin - len(out))  # the gaps stay NOPs
                continue
            if op == ".byte":
                code = bytes(value(v) for v in arg.split(","))
            elif op == ".word":
                code = b"".join(bytes([value(v) & 0xFF, value(v) >> 8]) for v in arg.split(","))
            elif op in BRANCH:
                off = (value(arg) - (pc + 2)) if final else 0
                assert -128 <= off <= 127, line
                code = bytes([BRANCH[op], off & 0xFF])
            elif op == "bsr":
                off = (value(arg) - (pc + 2)) if final else 0
                assert -128 <= off <= 127, line
                code = bytes([0x44, off & 0xFF])
            elif op in ("tam", "tma"):
                code = bytes([0x53 if op == "tam" else 0x43, value(arg.lstrip("#"))])
            elif op == "tst":
                imm, _, rest = arg.partition(",")
                base = 0xA3 if rest.endswith(",x") else 0x83
                plain = rest[:-2] if rest.endswith(",x") else rest
                if plain.startswith("$") and len(plain) <= 3:
                    code = bytes([base, value(imm.lstrip("#")), value(plain)])
                else:
                    code = bytes([base + 0x10, value(imm.lstrip("#")), value(plain) & 0xFF, value(plain) >> 8])
            elif op.startswith(("bbr", "bbs")):
                bit = int(op[3])
                zp, _, target = arg.partition(",")
                off = (value(target) - (pc + 3)) if final else 0
                assert -128 <= off <= 127, line
                code = bytes([(0x0F if op.startswith("bbr") else 0x8F) + bit * 0x10, value(zp), off & 0xFF])
            elif op.startswith(("rmb", "smb")):
                code = bytes([(0x07 if op.startswith("rmb") else 0x87) + int(op[3]) * 0x10, value(arg)])
            elif op in BLOCK:
                src, dst, ln = (value(v) for v in arg.split(","))
                code = bytes([BLOCK[op]]) + b"".join(bytes([w & 0xFF, w >> 8]) for w in (src, dst, ln))
            elif op in IMPLIED:
                code = bytes([IMPLIED[op]])
            elif arg == "a":
                code = bytes([OPS[op]["a"]])
            elif arg.startswith("#"):
                code = bytes([OPS[op]["imm"], value(arg[1:])])
            elif arg.startswith("(") and arg.endswith(",x)") and op == "jmp":
                v = value(arg[1:-3])
                code = bytes([0x7C, v & 0xFF, v >> 8])
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
                zp = base.startswith("$") and len(base) <= 3 and op not in ("jmp", "jsr")
                mode = ("zp" if zp else "abs") + index
                if mode not in OPS[op] and zp:
                    mode = "abs" + index
                if mode.startswith("zp"):
                    code = bytes([OPS[op][mode], v])
                else:
                    code = bytes([OPS[op][mode], v & 0xFF, v >> 8])
            out += code
            pc += len(code)
        if final:
            return bytes(out), labels
        if labels_only:
            return None, labels


EXERCISE = """
boot:
    sei
    csh
    cld
    ldx #$FF
    txs
    lda #$FF
    tam #$01        ; MPR0 = $FF: the I/O page
    lda #$F8
    tam #$02        ; MPR1 = $F8: the zero page, the stack
    lda #$01
    tam #$04        ; MPR2 = page 1: $4000-$5FFF
    lda #$02
    tam #$08        ; MPR3 = page 2: $6000-$7FFF
    lda #$00
    tam #$80        ; MPR7 = page 0 (as the reset leaves it)
    ; the timer: a tick every 127*1024 cycles, the TIQ unmasked
    lda #$7F
    sta $1400
    lda #$01
    sta $1401
    lda #$03
    sta $1402       ; the mask: the timer's bit only
    cli
main:
    ; -- binary and decimal arithmetic, the T flag
    cla
    clx
    cly
    lda #$07
    adc #$05
    sta $30
    sed
    lda #$58
    adc #$42
    sta $31
    cld
    lda #$90
    sbc #$45
    sta $33
    lda #$F0
    sta $40
    ldx #$05
    set             ; T = 1: the logic lands in the zero page at X
    lda #$0F
    and #$3C
    sta $34         ; A is back: $0F
    lda $40
    sta $35         ; zp[5] = $F0 & $3C
    php
    pla
    ora #$20
    pha
    plp             ; T set again through the stack, kept one instruction
    nop
    ; -- shifts, RMW, bit ops
    lda #$96
    sta $41
    asl $41
    lsr $41
    rol $41
    ror $41
    inc $41
    dec $41
    tsb $42
    trb $43
    smb2 $44
    rmb5 $44
    bbr2 $44, nobit
    inc $45
nobit:
    bbs2 $44, hasbit
    dec $45
hasbit:
    tst #$0F, $41
    tst #$F0, $41,x
    tst #$55, table0
    tst #$AA, table0,x
    ; -- the indexed and pointed forms
    ldx #$03
    lda table0,x
    sta $2030,x
    lda ($50),y
    adc $2038,y
    sta ($50),y
    ; -- the store through a pointer lands at pointer + Y, BIT sets N and V
    ; from the operand, a taken branch at the slice edge is counted as one
    lda #$80
    sta $54
    lda #$22
    sta $55
    ldy #$07
    lda #$C0
    sta ($54),y
    bit $2287
    bvc no_v
    inc $2290
no_v:
    bpl no_n
    inc $2291
no_n:
    ldy #$00
    lda ($52,x)
    and table0,y
    ora $2030,y
    cmp ($52,x)
    ; -- the block moves, then a checksum of what they copied
    tii table1, $2160, $0010
    tdd table1+$0F, $217F, $0008
    tin table1, $2180, $0004
    tia table1, $2190, $0006
    tai table1, $21A0, $0006
    lda $2160
    clc
    adc $217F
    adc $2180
    adc $2190
    adc $21A0
    sta $36
    ; -- the banked pages: MPR2 alternates between page 1 and page 3
    jsr bank1_sub
    inc $3E
    lda $3E
    lsr a
    bcs other_bank
    lda #$01
    bra map_bank
other_bank:
    lda #$03
map_bank:
    tam #$04
    jsr bank1_sub
    lda $4000
    sta $37
    jmp bank1_tail
back_from_jmp:
    ; -- a routine copied to WRAM: the interpreter runs it
    tii wram_src, $2200, $0003
    jsr $2200
    ; -- jumps through pointers: statically unreachable entries
    ldx #$00
    jmp (ptr_dyn)
back_from_dyn:
    ldx #$02
    jmp (jmtab,x)
back_from_tab:
    lda $2065
    sta $38
    ; -- the sweep's counter, then the quiet end
    inc $0F
    lda $0F
    cmp #$3C
    bcs sweep_done
    jmp main
sweep_done:
    sei
    lda #$04
    sta $1402       ; the timer's source out of the mask
    lda #$00
    sta $1401
idle:
    bra idle
    .byte $0B       ; undocumented: the interpreter NOPs it, the walk stops
after_illegal:
    lda #$5A
    sta $39
    bra idle
; -- the timer's handler: count, acknowledge, return (T survives)
timer_handler:
    inc $2012
    lda $1402
    rti
irq1_handler:
    rti
; -- the sweep's data
table0:
    .byte $11,$22,$33,$44,$55,$66,$77,$88
    .byte $99,$AA,$BB,$CC,$DD,$EE,$F0,$0F
table1:
    .byte $21,$43,$65,$87,$A9,$CB,$ED,$0F
    .byte $12,$34,$56,$78,$9A,$BC,$DE,$F1
ptr_dyn:
    .word dyn_helper
jmtab:
    .word dyn_helper, tab_helper
dyn_helper:
    eor #$FF
    sta $2065
    jmp back_from_dyn
tab_helper:
    eor #$3C
    sta $2066
    jmp back_from_tab
wram_src:
    inc $2011
    rts
"""

BANK1 = """
bank1_sub:
    lda bank1_table,x
    adc $2060
    sta $2060
    lda bank1_table+$0F
    sta $2061
    inc $2062
    rts
bank1_tail:
    lda bank1_table+$01
    sta $2063
    tsx
    phx
    ply
    sta $2064
    jmp back_from_jmp
bank1_table:
    .byte $01,$03,$05,$07,$09,$0B,$0D,$0F
    .byte $11,$13,$15,$17,$19,$1B,$1D,$1F
    .org $4F00
dyn_helper:
    eor #$FF
    sta $2065
    jmp back_from_dyn
"""


def exercise_rom():
    _, labels = assemble(EXERCISE, 0xE000, labels_only=True)
    code1, labels1 = assemble(BANK1, 0x4000, labels)
    labels.update(labels1)
    code0, _ = assemble(EXERCISE, 0xE000, labels)
    rom = bytearray(0x8000)
    rom[0x0000:0x2000] = code0.ljust(0x2000, b"\xFF")  # page 0: MPR7 = $00 at reset
    rom[0x2000:0x4000] = code1.ljust(0x2000, b"\xFF")  # page 1: MPR2 = $01
    rom[0x4000:0x6000] = code1.ljust(0x2000, b"\xFF")  # page 3: MPR2 = $03, the same bytes
    rom[0x6000:0x8000] = code1.ljust(0x2000, b"\xFF")  # page 2: MPR3 = $02
    for addr, target in ((0x1FF6, labels["irq1_handler"]), (0x1FF8, labels["irq1_handler"]),
                         (0x1FFA, labels["timer_handler"]), (0x1FFC, labels["irq1_handler"]),
                         (0x1FFE, labels["boot"])):
        rom[addr] = target & 0xFF
        rom[addr + 1] = target >> 8
    return bytes(rom)


def build_rom():
    # the program at $E000 (the reset vector points here; MPR7 = $00 at
    # reset, so $E000-$FFFF is the cartridge's page 0)
    prog = []
    at = 0xE000

    def emit(*bytes_):
        nonlocal at
        prog.append((at, list(bytes_)))
        at += len(bytes_)

    emit(0x78)                      # SEI
    emit(0xD4)                      # CSH
    emit(0xD8)                      # CLD
    emit(0xA2, 0xFF, 0x9A)          # LDX #$FF; TXS
    emit(0xA9, 0xFF, 0x53, 0x01)    # LDA #$FF; TAM #$01 (MPR0 = $FF, I/O)
    emit(0xA9, 0xF8, 0x53, 0x02)    # LDA #$F8; TAM #$02 (MPR1 = $F8, RAM)

    def vdc_select(reg):
        emit(0xA9, reg, 0x8D, 0x00, 0x00)          # LDA #reg; STA $0000

    def vdc_data(lo, hi):
        emit(0xA9, lo, 0x8D, 0x02, 0x00)           # STA $0002
        emit(0xA9, hi, 0x8D, 0x03, 0x00)           # STA $0003

    def mawr(addr):
        vdc_select(0x00)
        vdc_data(addr & 0xFF, addr >> 8)

    # the display: 256 px wide, 60 lines, BG on
    vdc_select(0x0B); vdc_data(0x1F, 0x04)         # HDR: HDW=31, HDE=4
    vdc_select(0x0A); vdc_data(0x02, 0x00)         # HSR: HSW=2, HDS=0
    vdc_select(0x0D); vdc_data(0x3B, 0x00)         # VDR: VDW=59
    vdc_select(0x0C); vdc_data(0x00, 0x00)         # VSR: VSW=0, VDS=0
    vdc_select(0x0E); vdc_data(0x00, 0x00)         # VCR
    vdc_select(0x09); vdc_data(0x10, 0x00)         # MWR: 64x32 BAT
    vdc_select(0x05); vdc_data(0x80, 0x00)         # CR: BG on

    # BAT (4,4) = character 1, palette 0: $0104
    mawr(0x0104)
    vdc_select(0x02); vdc_data(0x01, 0x00)

    # character 1's planes at words 16-31: plane 0 all ones (color 1)
    mawr(0x0010)
    vdc_select(0x02)
    for _ in range(8):
        vdc_data(0xFF, 0x00)
    mawr(0x0018)
    vdc_select(0x02)
    for _ in range(8):
        vdc_data(0x00, 0x00)

    # the VCE: backdrop (entry 0) green, entry 1 red
    emit(0xA9, 0x00, 0x8D, 0x02, 0x04)             # ctaddress = 0
    emit(0xA9, 0x00, 0x8D, 0x04, 0x04)             # green: G=4 -> $100
    emit(0xA9, 0x01, 0x8D, 0x05, 0x04)
    emit(0xA9, 0x01, 0x8D, 0x02, 0x04)             # ctaddress = 1
    emit(0xA9, 0x38, 0x8D, 0x04, 0x04)             # red: R=7 -> $038
    emit(0xA9, 0x00, 0x8D, 0x05, 0x04)

    # the idle loop polls the pad and marks RAM $2001 while I is held: a
    # run's fingerprint follows its input, the recording round trips lean
    # on that
    idle = at
    emit(0xA9, 0x00, 0x8D, 0x00, 0x10)             # LDA #0 STA $1000 (SEL: I..RUN nibble)
    emit(0xAD, 0x00, 0x10)                         # LDA $1000 (pressed = 0)
    emit(0x49, 0xFF)                               # EOR #$FF
    emit(0x29, 0x01)                               # AND #$01: I
    emit(0xF0, 0x03)                               # not held: past the mark
    emit(0x8D, 0x01, 0x20)                         # STA $2001 (WRAM, MPR1)
    emit(0x80, 0xED)                               # again

    rom = bytearray(0x8000)
    for base, bytes_ in prog:
        for i, b in enumerate(bytes_):
            rom[(base & 0x1FFF) + i] = b   # MPR7 = $00: $E000+ is file 0+
    assert at == idle + 19                         # the poll loop, then the RTI stub after it
    # the vectors in page 0: $FFF6 BRK/IRQ2, $FFF8 IRQ1, $FFFA TIQ,
    # $FFFC NMI, $FFFE RESET — the first four at a RTI stub
    rti = idle + 19
    rom[rti - 0xE000] = 0x40                       # RTI
    for addr in (0x1FF6, 0x1FF8, 0x1FFA, 0x1FFC):
        rom[addr] = rti & 0xFF
        rom[addr + 1] = rti >> 8
    rom[0x1FFE] = 0x00
    rom[0x1FFF] = 0xE0
    return bytes(rom)




def run(*args, ok=True):
    out = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (out.returncode == 0) != ok:
        print(out.stdout, out.stderr, sep="\n")
        raise SystemExit(f"unexpected return {out.returncode}: {' '.join(str(a) for a in args)}")
    return out.stdout + out.stderr




def run_hash(text):
    return [t for t in text.split() if t.startswith("run_hash=")][0]


def generate(rom, out, *options):
    out = Path(out)
    text = run(GBRECOMP, "--system", "pce", rom, "--target", "desktop",
               "--output-prefix", "synth", "-o", out, *options)
    run("cmake", "-G", "Ninja", "-S", out / "host", "-B", out / "host" / "build",
        "-DCMAKE_BUILD_TYPE=Release")
    run("ninja", "-C", out / "host" / "build")
    return out / "host" / "build" / "synth_host", text


def project(rom, out, *options):
    return generate(rom, out, *options)[0]


def picture(work):
    rom = work / "synthetic.pce"
    rom.write_bytes(build_rom())
    host = project(rom, work / "interp", "--nes-interp")
    out = run(host, "--frames", "60")
    h1 = run_hash(out)
    h2 = run_hash(run(host, "--frames", "60"))
    assert h1 == h2 and h1 != "run_hash=00000000", f"unstable hash: {h1} {h2}"

    run(host, "--frames", "60", "--wav", work / "sound.wav")
    with wave.open(str(work / "sound.wav"), "rb") as sound:
        assert sound.getnchannels() == 2 and sound.getsampwidth() == 1
        assert sound.getframerate() == 11025 and sound.getnframes() > 0

    run(host, "--frames", "60", "--ppm", work / "shot.ppm")
    body = (work / "shot.ppm").read_bytes().split(b"\n255\n", 1)[1]
    W = 256

    def px(x, y):
        o = (y * W + x) * 3
        return tuple(body[o:o + 3])

    # the backdrop through the VCE's entry 0: green (G=4 -> 144)
    c = px(0, 0)
    assert abs(c[1] - 144) < 30 and c[0] < 40 and c[2] < 40, f"backdrop wrong: {c}"
    # the character at BAT (4,4): screen x 32-39, y 32-39, color 1 = red
    c = px(35, 35)
    assert abs(c[0] - 252) < 30 and c[1] < 40 and c[2] < 40, f"character wrong: {c}"

    # the same card, recompiled: a straight line and a polled idle, a
    # precise project runs the very same instructions
    precise = project(rom, work / "picture_native", "--meta-precise")
    h3 = run_hash(run(precise, "--frames", "60"))
    assert h3 == h1, f"precise picture {h3} != interpreter {h1}"
    return h1


def recompiler(work):
    rom = work / "exercise.pce"
    rom.write_bytes(exercise_rom())
    frames = ("--frames", "120")
    reference_host = project(rom, work / "interp", "--nes-interp")
    reference = run_hash(run(reference_host, *frames))
    assert reference != "run_hash=00000000"

    # the static walk alone
    host = project(rom, work / "precise", "--meta-precise")
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise static walk {native} != interpreter {reference}"

    # the profile: contexts and dynamic entries; what they add must not
    # change the run
    coverage = work / "exercise.cov"
    run(reference_host, *frames, "--coverage", coverage)
    text = coverage.read_text()
    assert " d" in text, "no dynamic entry in the coverage file"
    assert any(len(t) == 16 for line in text.splitlines() if not line.startswith("#")
               for t in line.split()[1:] if t != "d"), "no MPR context in the coverage file"
    host, budget_text = generate(rom, work / "profiled", "--meta-precise",
                                 "--coverage", coverage, "--meta-code-budget", "98304")
    assert int(re.search(r"(\d+) compiled", budget_text).group(1)) <= 4096
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise with coverage {native} != interpreter {reference}"

    # the fast mode runs (its slices may overrun; only the game must hold)
    host = project(rom, work / "fast", "--coverage", coverage,
                   "--meta-code-budget", "98304")
    out = run(host, *frames)
    assert run_hash(out) != "run_hash=00000000", out
    assert run_hash(run(host, *frames)) == run_hash(out), "unstable fast hash"

    # the sweep: the card's JSR/JMP targets outside the walk compile too
    # (more instructions than the walk alone) and the run does not move
    _, plain_text = generate(rom, work / "precise", "--meta-precise")
    swept_host, swept_text = generate(rom, work / "sweep", "--meta-precise", "--pce-sweep")
    swept = run_hash(run(swept_host, *frames))
    assert swept == reference, f"precise with sweep {swept} != interpreter {reference}"
    compiled = lambda t: int(re.search(r"(\d+) compiled", t).group(1))
    assert compiled(swept_text) > compiled(plain_text), \
        f"the sweep compiled nothing more ({compiled(swept_text)} vs {compiled(plain_text)})"

    # the retired explorer (deepeel explores): two seeded play runs still
    # discover the same addresses, and the m0search plan (explore, then the
    # idle session with no touch) merges
    explore_locations = None
    for _ in range(2):
        cov = work / "explore.cov"
        run(reference_host, "--frames", "900", "--random", "3", "--coverage", cov)
        found = [l for l in cov.read_text().splitlines() if not l.startswith("#")]
        assert len(found) > 10, f"the explorer discovered almost nothing ({len(found)})"
        assert explore_locations in (None, len(found)), "unstable explore coverage"
        explore_locations = len(found)
    search = work / "search"
    run(sys.executable, str(ROOT / "tools" / "profile" / "m0search.py"), "run",
        "--system", "pce", "--harness", reference_host, "-o", search,
        "--sessions", "1", "--frames", "300", "--idle-frames", "300", "--seed", "5")
    sessions = json.loads((search / "search.json").read_text())["sessions"]
    assert [s["name"] for s in sessions] == ["explore", "idle"], sessions
    assert (search / "coverage.merged").exists(), "the m0search plan merged nothing"
    return reference


def main():
    work = Path(tempfile.mkdtemp(prefix="pce_e2e_"))
    h = picture(work)
    r = recompiler(work)
    print("ok: PC Engine e2e (picture stable", h + "; recompiled precise = interpreter",
          r + ", static and profiled)")


if __name__ == "__main__":
    main()
