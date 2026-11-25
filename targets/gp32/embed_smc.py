#!/usr/bin/env python3
"""Turn a SmartMedia image into a compact C include for standalone builds."""
import sys
import zlib
from pathlib import Path

source, destination = map(Path, sys.argv[1:3])
raw = source.read_bytes()
compressed = zlib.compress(raw, 9)
with destination.open("w", encoding="ascii") as out:
    out.write("#ifndef M0BOY_GP32_STANDALONE_ASSETS_H\n#define M0BOY_GP32_STANDALONE_ASSETS_H\n")
    out.write("#define GP32_EMBEDDED_SMC_SIZE %du\n" % len(raw))
    out.write("#define GP32_EMBEDDED_SMC_COMPRESSED_SIZE %du\n" % len(compressed))
    out.write("static const unsigned char gp32_embedded_smc_z[] = {\n")
    for start in range(0, len(compressed), 16):
        out.write(",".join("0x%02x" % b for b in compressed[start : start + 16]))
        out.write(",\n")
    out.write("};\n#endif\n")
