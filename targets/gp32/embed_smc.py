#!/usr/bin/env python3
"""Turn a SmartMedia image into a compact C include for standalone builds."""
import sys
import zlib
from pathlib import Path

source, destination = map(Path, sys.argv[1:3])
assets = [("SMC", source.read_bytes())]
if len(sys.argv) > 3:
    assets.append(("FXE", Path(sys.argv[3]).read_bytes()))
with destination.open("w", encoding="ascii") as out:
    out.write("#ifndef M0BOY_GP32_STANDALONE_ASSETS_H\n#define M0BOY_GP32_STANDALONE_ASSETS_H\n")
    for name, raw in assets:
        compressed = zlib.compress(raw, 9)
        out.write("#define GP32_EMBEDDED_%s_SIZE %du\n" % (name, len(raw)))
        out.write("#define GP32_EMBEDDED_%s_COMPRESSED_SIZE %du\n" % (name, len(compressed)))
        out.write("static const unsigned char gp32_embedded_%s_z[] = {\n" % name.lower())
        for start in range(0, len(compressed), 16):
            out.write(",".join("0x%02x" % b for b in compressed[start : start + 16]))
            out.write(",\n")
        out.write("};\n")
    out.write("#endif\n")
