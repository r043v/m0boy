#!/usr/bin/env python3
"""Write a PlatformIO project running one GP32 FXE, GXB or ZPK on the Gamebuino AKA.

    make_aka.py APPLICATION OUTPUT_DIRECTORY [--fast-pages N,N,... | --page-profile FILE [--fast-count N]]
                [--aot app_aot.c] [--card NAME.SMC] [--no-iram]

The project embeds the application: it holds ROM data and must not be
published.  The guest's 8 MiB SDRAM is split into 16 KiB pages: --fast-pages
names those kept in internal memory, the PSRAM holds the others.
--page-profile chooses them instead: the --fast-count most used pages of a
list written by a PC build made with -DM0_GP32_RAM_PROFILE=ON and run with the
environment variable GP32_RAM_PROFILE naming the file.  The hot functions of
the runtime are linked into IRAM unless --no-iram is given.  --aot
builds the application recompiled by runtime/recompile.py (--hybrid) in place of
the plain interpreter.  --card names the SmartMedia image the firmware opens on
the SD card (root or one folder below), for applications that need one.
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TARGETS = HERE.parent.parent
RUNTIME = HERE.parent / "runtime"
AKA = TARGETS / "consoles" / "aka"


def word(data, at):
    return struct.unpack_from("<I", data, at)[0]


def decode(data):
    """The plain GXB payload of an FXE, as gp32_exec.c decodes it."""
    if data[:4] != b"fxe ":
        return data
    key_at = word(data, 8) + 16
    ro, rw = word(data, key_at - 8), word(data, key_at - 4)
    if not rw or rw > len(data) - key_at or ro > len(data) - key_at - rw:
        raise SystemExit("invalid FXE section sizes")
    out = bytearray(ro + rw)
    for i in range(rw):
        out[ro + i] = data[key_at + i] ^ 0xFF
    for i in range(ro):
        out[i] = data[key_at + rw + i] ^ out[ro + i % rw]
    return bytes(out)


def decode_zpk(data):
    """The plain GXB payload of a ZPK package, as gp32_exec.c decodes it, and
    the card directory the package installs into."""
    gxe_chunk, header = 0x175, 20
    at = gxe_chunk + header
    if data[gxe_chunk:gxe_chunk + 4] != b"GXE ":
        raise SystemExit("unsupported ZPK package layout")
    gxe = data[at:at + word(data, gxe_chunk + 16)]
    relative = word(gxe, 0x70)
    shift = relative - 0x410
    profile = word(gxe, 0x74 + relative + 44)
    if shift < 0 or profile not in (0x228, 0x328):
        raise SystemExit("unsupported ZPK encryption profile")
    prefix = profile - 0x228
    key_at = 0x5dc + shift + prefix
    key = gxe[key_at:key_at + 96]
    stride = word(gxe, 0x560 + shift)
    gxc = at + len(gxe)
    content = gxc + 2 * header
    length = word(data, gxc + header)
    if stride < 256 or len(key) != 96 or content + length > len(data):
        raise SystemExit("invalid ZPK executable section")
    out = bytearray(gxe[0x5dc + shift:key_at]) + data[content:content + length]
    for offset in range(0, len(out), stride):
        for i in range(min(256, len(out) - offset)):
            out[offset + i] ^= key[i % 96]
    field = data[0x6d:0x6d + 256]
    directory = field.split(b"\0")[0].decode("latin-1") if field.startswith(b"gp:\\") else ""
    return bytes(out), directory


def name83(name):
    stem, _, extension = name.upper().partition(".")
    if not (0 < len(stem) <= 8 and len(extension) <= 3 and (stem + extension).isalnum()):
        raise SystemExit("--card takes an 8.3 file name")
    return stem.ljust(8) + extension.ljust(3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("application", type=Path)
    parser.add_argument("output", type=Path)
    where = parser.add_mutually_exclusive_group()
    where.add_argument("--fast-pages", default="0,1,2,3")
    where.add_argument("--page-profile", type=Path)
    parser.add_argument("--fast-count", type=int, default=4)
    parser.add_argument("--no-iram", action="store_true")
    parser.add_argument("--aot", type=Path)
    parser.add_argument("--card")
    args = parser.parse_args()
    if args.page_profile:
        # "page uses shown": the pages on the screen have their own placement.
        uses = [(page, n) for page, n, shown in
                (map(int, line.split()) for line in args.page_profile.read_text().splitlines())
                if not shown]
        if args.fast_count < 1 or not uses:
            parser.error("--page-profile needs a non empty list and --fast-count of at least 1")
        uses.sort(key=lambda entry: (-entry[1], entry[0]))
        chosen = uses[:args.fast_count]
        share = sum(n for _, n in chosen) / sum(n for _, n in uses)
        print("fast pages: %s, %.1f %% of the profiled uses off the screen" % (
            ",".join(str(page) for page, _ in chosen), 100 * share))
        pages = sorted(page for page, _ in chosen)
    else:
        pages = sorted({int(v, 0) for v in args.fast_pages.split(",")})
    if not pages or pages[0] < 0 or pages[-1] >= 512:
        parser.error("--fast-pages takes page numbers from 0 to 511")

    out = args.output
    src = out / "src"
    (out / "boards").mkdir(parents=True, exist_ok=True)
    src.mkdir(exist_ok=True)
    for name in ("arm920t", "s3c2400", "s3c2400_bus", "gp32_hle", "gp32_exec", "gp32_smc"):
        shutil.copy(RUNTIME / (name + ".c"), src)
    for header in RUNTIME.glob("*.h"):
        shutil.copy(header, src)
    for path in (HERE / "gp32_aka.c", HERE / "gp32_blit.S", HERE / "gp32_blit.h",
                 HERE / "gp32_config.h", HERE / "gbl_config.h",
                 HERE / "gbl_profile.h", AKA / "aka_hw.cpp", AKA / "aka_hw.h",
                 AKA / "esp32_config.h", TARGETS / "common/consoles/esp32/esp32_hw.h",
                 TARGETS / "common/platform/m0_target.h", TARGETS / "common/platform/m0_lcd.h",
                 TARGETS / "common/platform/m0_menu.h", TARGETS / "common/runtime/m0_scale.h",
                 TARGETS / "common/runtime/m0_divide.h",
                 *((HERE / "gp32_aka_card.c", HERE / "gp32_aka_card.h",
                    TARGETS / "common/platform/m0_fat.c", TARGETS / "common/platform/m0_fat.h",
                    TARGETS / "common/platform/m0_sd.h",
                    TARGETS / "common/consoles/esp32/esp32_sd.cpp") if args.card else ())):
        if path.exists():
            shutil.copy(path, src)
    flags, sources = "", "+<*>"
    for old in src.glob("app_aot*.c"):
        old.unlink()
    if args.aot:
        # The generated files include arm920t.c to specialize the interpreter.
        stem = args.aot.name.removesuffix(".c")
        for part in [args.aot, *args.aot.parent.glob(stem + ".part[0-9][0-9][0-9].c")]:
            shutil.copy(part, src / part.name.replace(stem, "app_aot", 1))
        flags, sources = "-DM0_GP32_AOT", "+<*> -<arm920t.c>"
    if not args.no_iram:
        flags += " -DM0_GP32_IRAM"
    shutil.copy(AKA / "boards" / "gamebuino_aka.json", out / "boards")
    shutil.copy(AKA / "aka.csv.in", out / "aka.csv")
    shutil.copy(AKA / "aka_size.py.in", out / "aka_size.py")

    application = args.application.read_bytes()
    directory = ""
    if application[:4] == b"ZPK ":
        payload, directory = decode_zpk(application)
    else:
        payload = decode(application)
    defines = ""
    if directory:
        escaped = directory.replace("\\", "\\\\").replace('"', '\\"')
        defines += '#define GP32_PAYLOAD_DIRECTORY "%s"\n' % escaped
    if args.card:
        defines += '#define GP32_AKA_CARD_NAME83 "%s"\n' % name83(args.card)
        defines += '#define GP32_AKA_CARD_FILE "%s"\n' % args.card.upper()
        flags += " -DM0_GP32_PAGED_CARD"
    (out / "payload.bin").write_bytes(payload)
    (src / "gp32_payload.h").write_text(
        "#include <stddef.h>\n#include <stdint.h>\n"
        "extern const uint8_t gp32_payload[];\nextern const uint32_t gp32_payload_size;\n" + defines)
    (src / "gp32_payload.S").write_text(
        '    .section .rodata.gp32_payload,"a"\n    .balign 4\n'
        "    .global gp32_payload\ngp32_payload:\n"
        '    .incbin "payload.bin"\n1:\n    .balign 4\n'
        "    .global gp32_payload_size\ngp32_payload_size:\n    .4byte 1b - gp32_payload\n"
)
    (out / "platformio.ini").write_text(f"""[platformio]
boards_dir = boards

[env:aka]
platform = espressif32@6.9.0
board = gamebuino_aka
framework = arduino
board_build.f_cpu = 240000000L
board_upload.flash_size = 8MB
board_build.partitions = aka.csv
build_unflags = -Os
build_flags = -O2 -Wno-unused-label -mlongcalls -Isrc
    -DCORE_DEBUG_LEVEL=0
    -DM0_GP32_EXTERNAL_RAM -DM0_GP32_ONE_MACHINE -DM0_GP32_PC_TOOLS=0 -DM0_GP32_DIVIDE=1 {flags}
    -DM0_GP32_PAGED_RAM "-DM0_GP32_AKA_FAST_PAGES={",".join(map(str, pages))}"
build_src_filter = {sources}
extra_scripts = post:aka_size.py
""")
    print(f"{out}: pio run")


if __name__ == "__main__":
    sys.exit(main())
