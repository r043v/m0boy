"""Size a generated META project using its linked flash and SRAM boundaries."""

import argparse
from pathlib import Path
import subprocess
import zlib

FLASH_LIMIT = 245760
STACK_RESERVE = 2048
SECTOR_SIZE = 512
CACHE_SLOT_SIZE = 516


def write_array(out, name, ctype, values, width=16):
    out.write(f"extern const {ctype} {name}[] = {{\n")
    for i in range(0, len(values), width):
        out.write("    " + ", ".join(str(value) for value in values[i:i + width]) + ",\n")
    out.write("};\n")


def write_config(project, slots, rom, resident):
    src = project / "src"
    config = f"#pragma once\n#define WS_ROM_CACHE_SLOTS {slots}\n"
    if rom:
        config += "#define WS_FLASH_ROM 1\n"
    (src / "ws_memory_config.h").write_text(config)
    with (src / "ws_flash_rom.cpp").open("w") as out:
        if rom:
            out.write("#include <stdint.h>\n")
            out.write(f"extern const uint32_t ws_flash_rom_size = {len(rom)};\n")
            out.write(f"extern const uint32_t ws_flash_crc = {zlib.crc32(rom)}u;\n")
            index = [65535] * (len(rom) // SECTOR_SIZE)
            for slot, sector in enumerate(resident):
                index[sector] = slot
            write_array(out, "ws_flash_map", "uint16_t", index)
            payload = b"".join(rom[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE] for i in resident)
            write_array(out, "ws_flash_bytes", "uint8_t", payload or b"\0")


def build(project):
    subprocess.run(["pio", "run", "-d", str(project)], check=True)
    firmware = project / ".pio/build/meta/firmware"
    symbols = subprocess.check_output(["nm", str(firmware.with_suffix(".elf"))], text=True)
    limits = {}
    for line in symbols.splitlines():
        fields = line.split()
        if fields and fields[-1] in {"__HeapLimit", "__StackLimit", "__StackTop"}:
            limits[fields[-1]] = int(fields[0], 16)
    return (
        firmware.with_suffix(".bin").stat().st_size,
        limits["__StackLimit"] - limits["__HeapLimit"],
        limits["__StackTop"] - limits["__HeapLimit"],
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes() if args.rom else b""
    if args.rom and (not rom or len(rom) > 16 * 1024 * 1024 or len(rom) % 65536):
        parser.error("invalid WonderSwan ROM size")
    sectors = len(rom) // SECTOR_SIZE
    reads = [int(line) for line in args.trace.read_text().splitlines()] if args.trace else [0] * sectors
    if len(reads) != sectors or any(count < 0 for count in reads):
        parser.error("ROM trace must have one nonnegative count per sector")
    ranked = sorted(range(sectors), key=lambda sector: (-reads[sector], sector))

    write_config(args.project, 1, rom, [])
    _, free_ram, _ = build(args.project)
    slots = 1 + free_ram // CACHE_SLOT_SIZE
    if slots < 1:
        raise SystemExit("META SRAM budget exceeded by the runtime")

    # The cache size can change the generated lookup code, so measure
    # flash with the final slot count before filling it with ROM.
    write_config(args.project, slots, rom, [])
    flash, free_ram, stack_room = build(args.project)
    count = min(sectors, (FLASH_LIMIT - flash) // SECTOR_SIZE)
    if count:
        write_config(args.project, slots, rom, ranked[:count])
        flash, free_ram, stack_room = build(args.project)
    if flash > FLASH_LIMIT or free_ram < 0 or stack_room < STACK_RESERVE:
        raise SystemExit("META memory budget exceeded after packing")
    print(f"ROM flash: {count * SECTOR_SIZE} bytes, firmware: {flash}/{FLASH_LIMIT} bytes")
    print(f"SD cache: {slots} sectors, stack room: {stack_room} bytes")
    if sum(reads):
        hits = sum(reads[i] for i in ranked[:count])
        print(f"Flash coverage on the ROM trace: {100 * hits / sum(reads):.2f}%")


if __name__ == "__main__":
    main()
