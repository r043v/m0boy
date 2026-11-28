#!/usr/bin/env python3
"""Pack an application, and optionally its card, to be embedded in the frontend.

Format: magic, NUL padded 64-byte file name, application size, card size, the
application, then the card as an LZMA "alone" stream (empty without a card).
"""
import argparse
from pathlib import Path
import struct
import lzma


def compress(card):
    packed = bytearray(lzma.compress(card, lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME))
    packed[5:13] = struct.pack("<Q", len(card))   # state the size instead of "unknown"
    return bytes(packed)


def pack(name, app, card):
    header = struct.pack("<8s64sII", b"M0G32PK3", name.encode("ascii"), len(app), len(card))
    return header + app + (compress(card) if card else b"")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--card", type=Path)
    parser.add_argument("--name", help="file name given to the application")
    args = parser.parse_args()
    name = args.name or args.app.name
    if not name.isascii() or len(name) > 63 or "/" in name or "\\" in name:
        parser.error("the application name must be a short ASCII file name")
    card = args.card.read_bytes() if args.card else b""
    args.output.write_bytes(pack(name, args.app.read_bytes(), card))
