#!/usr/bin/env python3
"""Turn a SmartMedia image into a compact C include for standalone builds."""
import argparse
import zlib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("smc", type=Path)
parser.add_argument("destination", type=Path)
parser.add_argument("--bios", type=Path)
parser.add_argument("--input", type=Path)
args = parser.parse_args()

assets = [("SMC", args.smc.read_bytes())]
if args.bios:
    assets.append(("BIOS", args.bios.read_bytes()))
boot_inputs = []
if args.input:
    events = []
    names = {"A": 1, "B": 2, "L": 4, "R": 8, "START": 16, "P": 16,
             "SELECT": 32, "UP": 64, "DOWN": 128, "LEFT": 256, "RIGHT": 512}
    for line in args.input.read_text(encoding="ascii").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        frame_text, command = line.split(":", 1)
        frame = int(frame_text.rstrip("fF"), 0)
        action = command[0] if command[0] in "+-=" else "tap"
        if action != "tap":
            command = command[1:]
        mask = 0
        for name in command.split("+"):
            mask |= names[name.strip().upper()]
        events.append((frame, action, mask))
    max_frame = max((event[0] for event in events), default=-1)
    held = 0
    event_index = 0
    boot_inputs = []
    ordered = sorted(events, key=lambda e: (e[0], {"tap": 0, "+": 1, "-": 2, "=": 3}[e[1]]))
    for frame in range(max_frame + 1):
        tap = 0
        while event_index < len(ordered) and ordered[event_index][0] <= frame:
            event_frame, action, mask = ordered[event_index]
            event_index += 1
            if event_frame < frame and action == "tap":
                continue
            if action == "tap":
                tap |= mask
            elif action == "+":
                held |= mask
            elif action == "-":
                held &= ~mask
            else:
                held = mask
        boot_inputs.append(tap | held)

with args.destination.open("w", encoding="ascii") as out:
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
    if args.input:
        out.write("#define GP32_EMBEDDED_BOOT_INPUT_COUNT %du\n" % len(boot_inputs))
        out.write("static const unsigned int gp32_embedded_boot_input[] = {\n")
        for start in range(0, len(boot_inputs), 16):
            out.write(",".join("0x%08xu" % value for value in boot_inputs[start : start + 16]))
            out.write(",\n")
        out.write("};\n")
    out.write("#endif\n")
