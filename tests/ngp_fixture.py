"""Synthetic Neo Geo Pocket cartridge (512 KiB) for the runtime and
end-to-end tests: a TLCS-900H program hand-assembled from the encodings
cross-checked against real cartridge streams, a V-INT handler counting
frames in work RAM, K2GE setup (palettes, tiles, a scroll map the handler
advances), a wait loop the interrupts release, and the flash-save window
touched once.

No ROM or ROM-derived data: everything is assembled here."""

ENTRY = 0x200100
HANDLER = 0x200400


def _u24(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))


def _u16(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF))


def ld_xsp_imm(v):
    return bytes((0x47,)) + bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))


def ld_xwa_imm(v):
    return bytes((0x40,)) + bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))


def ld_mem_imm8(addr, imm):  # F2 (nn24) 00 imm8
    return bytes((0xF2,)) + _u24(addr) + bytes((0x00, imm & 0xFF))


def ldw_mem_imm16(addr, v):  # F2 (nn24) 02 imm16
    return bytes((0xF2,)) + _u24(addr) + bytes((0x02,)) + _u16(v)


def ld_mem_a(addr):  # F2 (nn24) 41: LD (mem),A
    return bytes((0xF2,)) + _u24(addr) + bytes((0x41,))


def incw_mem(addr):  # D2 (nn24) 61: incw 1,(mem)
    return bytes((0xD2,)) + _u24(addr) + bytes((0x61,))


def ld_a_mem(addr):  # C1 (nn16) 21: LD A,(nn16)
    return bytes((0xC1,)) + _u16(addr) + bytes((0x21,))


def cp_a_short(n):  # C9 DA+n: CP A,#n (0-7 encoded in the second byte)
    return bytes((0xC9, 0xD8 + n))


def jr_cc(off):  # 0x60+cc, 8-bit signed offset from the next instruction
    return bytes((0x60 + cc_code, ) if False else (0x66, off & 0xFF))


def fixture(mode=0x00):
    """The cartridge image: header + the program + tile data."""
    rom = bytearray(b"\xFF" * 0x80000)

    def poke(addr, data):
        rom[addr - 0x200000:addr - 0x200000 + len(data)] = data

    # ---- the header (0x30 bytes) ----
    rom[0x00:0x1C] = b"SNK License Cart - m0boy fixture\x00\x00\x00"[:0x1C]
    poke(0x20001C, _u24(ENTRY))            # the 24-bit entry point
    poke(0x200020, b"\x59\x00")            # catalog
    poke(0x200022, b"\x00\x00")            # sub-catalog
    rom[0x23] = mode                       # 0x00 mono, 0x10 colour
    rom[0x24:0x30] = b"M0BOYFIXTURE"

    # ---- the program ----
    code = bytearray()
    e = ENTRY

    def emit(*parts):
        nonlocal e
        for p in parts:
            poke(e, p)
            e += len(p)

    emit(ld_xsp_imm(0x006C00))
    # the V-INT vector: RAM slot 5 (0x6FB8 + 20), the handler's 32-bit address
    emit(ldw_mem_imm16(0x006FCC, HANDLER & 0xFFFF))
    emit(ld_mem_imm8(0x006FCE, (HANDLER >> 16) & 0xFF))
    # K2GE: both interrupts on, the frame length, the backdrop, a full window
    emit(ld_mem_imm8(0x008000, 0xC0))
    emit(ld_mem_imm8(0x008006, 0xC6))
    emit(ld_mem_imm8(0x008118, 0x80))
    emit(ld_mem_imm8(0x008005, 0xFF))
    emit(ld_mem_imm8(0x008004, 0xA0))
    # colour palettes in the scroll-1 palette-0 block (0x8280 + px*2): the
    # 12-bit entries are r in the low nibble, g then b — 1 white, 2 green,
    # 3 blue
    emit(ldw_mem_imm16(0x008282, 0x0FFF))
    emit(ldw_mem_imm16(0x008284, 0x00F0))
    emit(ldw_mem_imm16(0x008286, 0x0F00))
    # the compat grey ramp stays from reset; repaint compat entries 1-3 so a
    # mono boot shows the same shape in grey
    emit(ldw_mem_imm16(0x008382, 0x0FFF))
    emit(ldw_mem_imm16(0x008384, 0x0777))
    emit(ldw_mem_imm16(0x008386, 0x0333))
    # the scroll1 level LUT (0x8109-0x810B, palette 0 values 1-3): grey levels
    emit(ld_mem_imm8(0x008109, 0x01))
    emit(ld_mem_imm8(0x00810A, 0x02))
    emit(ld_mem_imm8(0x00810B, 0x03))
    # tiles (tile 0 stays blank, the empty map entries name it):
    # 1 = solid 0x55 (colour 1), 2 = solid 0xFF (colour 3), 3 = checker
    for i in range(16):
        emit(ld_mem_imm8(0x00A010 + i, 0x55))
    for i in range(16):
        emit(ld_mem_imm8(0x00A020 + i, 0xFF))
    for i in range(8):
        emit(ld_mem_imm8(0x00A030 + i * 2, 0xAA))
        emit(ld_mem_imm8(0x00A030 + i * 2 + 1, 0x55))
    # the scroll map, plane 1: a stripe pattern of tiles 1-3 on the first row
    for col in range(8):
        emit(ldw_mem_imm16(0x009000 + col * 4, 1 + (col % 3)))
        emit(ldw_mem_imm16(0x009002 + col * 4, 1 + ((col + 1) % 3)))
    # interrupts on, mask level 2 (the VBlank's level: the RETI restores it)
    emit(bytes((0x06, 0x02)))
    # wait until the frame counter reaches 3 (the interrupts release it),
    # then idle forever; the offsets are known before the bytes are emitted
    wait = e
    emit(ld_a_mem(0x004000))               # 4 bytes
    emit(cp_a_short(3))                    # 2 bytes
    jr_off = (wait - (e + 2)) & 0xFF       # jr LT -> wait
    emit(bytes((0x61, jr_off)))
    emit(bytes((0x68, 0xFE)))              # jr T,-2: idle

    # ---- the V-INT handler ----
    h = HANDLER
    poke(h, incw_mem(0x004000))            # the frame counter
    h += len(incw_mem(0x004000))
    poke(h, ld_a_mem(0x004000))            # A = the counter's low byte
    h += len(ld_a_mem(0x004000))
    poke(h, bytes((0xC9, 0xCC, 0x03)))     # and A,#3: cycle the visible tiles
    h += 3
    poke(h, ld_mem_a(0x009002))            # map entry 1 = the cycled tile
    h += len(ld_mem_a(0x009002))
    poke(h, bytes((0x07,)))                # reti
    return bytes(rom)


if __name__ == "__main__":
    import sys
    open(sys.argv[1] if len(sys.argv) > 1 else "fixture.ngp", "wb").write(fixture())
