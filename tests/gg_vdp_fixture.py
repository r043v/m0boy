"""Synthetic Game Gear/Master System cartridge for the VDP end-to-end tests:
left-column masking (reg 0 bit 5), the line interrupt (reg 10 + the V counter
at the interrupt), the PAUSE button's NMI, and the TMS9918 modes 0-3 the SMS
VDP falls into when M4 (reg 0 bit 2) is cleared — Graphics I, Graphics II,
40-column text and multicolour, each held for 40 frames with tables the
modes read at different bases.

The tables are laid out so one set serves every mode: pattern table at
$0800 (reg 4 = 04), name table at $3800 (reg 2 = 0x0E: the same base in
mode 4 and in the TMS modes), colour table at $2000 (reg 3 = 0x80).
The frame IRQ re-programs the registers per phase; the NMI handler counts
the PAUSE presses; the line-IRQ handler records the V counter it sees.

No ROM or ROM-derived data: everything is assembled here."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from z80asm import Asm

SOURCE = r"""
org 0
    di
    im 1
    ld sp,0xdff0
    jp main

org 0x38
irq:
    push af
    push hl
    in a,(0xbf)            ; status: clears the frame and line flags
    ld (0xc010),a
    bit 7,a
    jr z,line_irq
    ld hl,(0xc000)         ; frame counter
    inc hl
    ld (0xc000),hl
    jr irq_end
line_irq:
    in a,(0x7e)            ; the V counter at the line interrupt
    ld (0xc011),a
    ld hl,(0xc004)         ; line interrupt counter
    inc hl
    ld (0xc004),hl
irq_end:
    pop hl
    pop af
    ei
    reti

org 0x66
nmi:
    push af
    ld hl,(0xc00a)         ; PAUSE presses
    inc hl
    ld (0xc00a),hl
    pop af
    retn

org 0x100
main:
    ld hl,regs_m4          ; mode 4, masking off, no line IRQ yet
    ld b,11
    ld c,0x80
regs0:
    ld a,(hl)
    out (0xbf),a
    ld a,c
    out (0xbf),a
    inc hl
    inc c
    djnz regs0
    ; CRAM: 64 bytes, colour 1 bright blue, the rest distinct
    ld a,0x00
    out (0xbf),a
    ld a,0xc0
    out (0xbf),a
    ld hl,palette
    ld b,64
    ld c,0xbe
    otir
    ; pattern table $0800 (reg 4 = 04): 256 tiles, 8 bytes each = $800 bytes
    ld a,0x00
    out (0xbf),a
    ld a,0x48              ; $0800 + $4000
    out (0xbf),a
    ld de,0x0800
fill_pat:
    ld a,e                 ; the row pattern is the low address byte
    out (0xbe),a
    inc de
    ld a,d
    cp 0x10                ; up to $1000
    jr nz,fill_pat
    ; name table $3800 (reg 2 = 0x0E): 32 x 28 entries, tile = column + 4*row
    ld a,0x00
    out (0xbf),a
    ld a,0x78              ; $3800 + $4000
    out (0xbf),a
    ld de,0x380
    ld a,0
names:
    out (0xbe),a           ; tile number: wraps through a distinctive sequence
    inc a
    dec de
    ld a,d
    or e
    jr nz,names
    ; colour table $2000 (reg 3 = 0x80): 32 bytes for Graphics I...
    ld a,0x00
    out (0xbf),a
    ld a,0x60              ; $2000 + $4000
    out (0xbf),a
    ld de,0x20
    ld a,0x21              ; foreground 2, background 1
fill_col:
    out (0xbe),a
    inc a
    dec de
    ld a,d
    or e
    jr nz,fill_col
    ; ... and $3FC0-$3FFF for Graphics II (reg 3 = 0xFF writes them at $2000
    ; with the full mask: fill $2000-$3FFF a second time, bytes = low address)
    ld a,0x00
    out (0xbf),a
    ld a,0x60
    out (0xbf),a
    ld de,0x200
fill_col2:
    ld a,e
    out (0xbe),a
    inc de
    ld a,d
    cp 0x40
    jr nz,fill_col2
    ei
loop:
    halt
    ld a,(0xc000)          ; the frame IRQ re-programmed the registers; the
    call phase             ; program only reacts to the phase changes here
    ld a,0x00              ; VRAM $3800: the counters rendered as tile numbers
    out (0xbf),a
    ld a,0x78
    out (0xbf),a
    ld a,(0xc004)          ; tile 0: line interrupts taken
    out (0xbe),a
    ld a,(0xc00a)          ; tile 1: PAUSE NMIs taken
    out (0xbe),a
    jr loop

; A = frame counter low: re-program the VDP registers per phase.  Every
; phase change is idempotent for the frames it covers.
phase:
    cp 20
    jr c,ph_m4             ; 0-19: mode 4, masking off
    cp 40
    jr c,ph_mask           ; 20-39: masking on
    cp 60
    jr c,ph_lirq           ; 40-59: line IRQ armed (line 64)
    cp 100
    jr c,ph_g1             ; 60-99: Graphics I
    cp 140
    jr c,ph_g2             ; 100-139: Graphics II
    cp 180
    jr c,ph_text           ; 140-179: 40-column text
    cp 220
    jr c,ph_multi          ; 180-219: multicolour
    ; 220+: mode 4, masking on, line IRQ still armed
    ld hl,regs_m4b
    jr regs_out
ph_m4:
    ld hl,regs_m4
    jr regs_out
ph_mask:
    ld hl,regs_mask
    jr regs_out
ph_lirq:
    ld hl,regs_lirq
    jr regs_out
ph_g1:
    ld hl,regs_g1
    jr regs_out
ph_g2:
    ld hl,regs_g2
    jr regs_out
ph_text:
    ld hl,regs_text
    jr regs_out
ph_multi:
    ld hl,regs_multi
regs_out:
    ld b,11
    ld c,0x80
out_regs:
    ld a,(hl)
    out (0xbf),a
    ld a,c
    out (0xbf),a
    inc hl
    inc c
    djnz out_regs
    ret

; register sets 0-10 (0: 16 blank|IE|... 1: 4/16K|display|frame IE)
; name $3800 (reg 2 = 0x0E), pattern $0800 (reg 4 = 0x04), colour $2000
regs_m4:
    db 0x04,0xe0,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x30
regs_mask:
    db 0x24,0xe0,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x30
regs_lirq:
    db 0x14,0xe0,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
regs_g1:
    db 0x00,0x60,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
regs_g2:
    db 0x02,0x60,0x0e,0xff,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
regs_text:
    db 0x00,0x70,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
regs_multi:
    db 0x00,0x68,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
regs_m4b:
    db 0x24,0xe0,0x0e,0x80,0x04,0xff,0xfb,0x01,0x00,0x00,0x40
palette:
    dw 0x000,0x0f0,0x0cc,0xf00,0x0ff,0xf0f,0xff0,0xfff,0x888,0x008,0x080,0x800,0x088,0x808,0x880,0x444
    dw 0x000,0x0af,0x5f0,0xf50,0x5ff,0xf5f,0xff5,0xaaa,0x111,0x226,0x262,0x622,0x266,0x626,0x662,0xccc
"""


def fixture():
    return bytes(Asm(0x10000).assemble(SOURCE))


if __name__ == '__main__':
    import sys
    open(sys.argv[1], 'wb').write(fixture())
