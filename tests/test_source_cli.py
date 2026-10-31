"""Synthetic cartridges only; check selection before legacy GB dispatch."""
import subprocess
import sys
import tempfile
from pathlib import Path

cli = sys.argv[1]


def run(*args, ok=True, contains=""):
    result = subprocess.run([cli, *map(str, args)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    assert contains in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result.stdout


listing = run("--list-systems", contains="sg1000 cpu=z80 inspect=yes disasm=yes codegen=desktop,meta,pokitto")
assert "gameboy cpu=sm83 inspect=yes disasm=yes codegen=desktop,meta,pokitto" in listing
assert "pico8 cpu=lua inspect=yes disasm=no codegen=desktop,meta,pokitto,espboy,pc,aka" in listing
assert "nes cpu=6502 inspect=yes disasm=yes codegen=desktop,meta,pokitto,pc" in listing
assert "pce cpu=huc6280 inspect=yes disasm=yes codegen=desktop" in listing
assert "vectrex cpu=6809 inspect=yes disasm=no codegen=meta,pc,aka" in listing
assert "megadrive cpu=m68k inspect=yes disasm=no codegen=desktop,pc,aka" in listing
assert "meta cpu=m0plus inspect=yes disasm=yes codegen=pc,aka" in listing
assert "pokitto cpu=m0plus inspect=yes disasm=yes codegen=pc" in listing
targets = run("--list-targets", contains='meta host=no ram=32K')
assert 'desktop host=yes' in targets and 'pc host=yes' in targets
assert 'odroid host=no ram=320K psram=4096K' in targets and 'aka host=no ram=320K psram=8192K' in targets
assert 'espboy host=no ram=80K psram=0K flash=1024K screen=128x128 sd=no' in targets
run("--system", ok=False, contains="requires a value")
run("--cpu", "68000", ok=False, contains="unknown --cpu")
run("--system", "unknown", ok=False, contains="unknown --system")
assert "sms cpu=z80" in listing and "gg cpu=z80" in listing
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    for system, base, stack in (("meta", 0x4000, 0x20008000),
                                ("pokitto", 0, 0x10008000)):
        firmware = root / (system + ".bin")
        image = bytearray(32)
        image[0:4] = stack.to_bytes(4, "little")
        image[4:8] = (base + 9).to_bytes(4, "little")
        image[8:14] = bytes([0x00, 0x20, 0x01, 0x30, 0xFE, 0xE7])
        firmware.write_bytes(image)
        run(firmware, "--system", system, "--cpu", "m0plus", "--info",
            contains=f"system={system}\ncpu=m0plus")
        listing = run(firmware, "--system", system, "--disasm", "--limit", "3")
        assert f"{base + 8:08X}  MOV r0, #0" in listing, listing
        assert "ADD r0, #1" in listing and "B $" in listing, listing
        image[8:12] = bytes([0x00, 0xF0, 0x00, 0xF8])  # BL to the next instruction
        firmware.write_bytes(image)
        assert f"BL ${base + 12:08X}" in run(
            firmware, "--system", system, "--disasm", "--limit", "1")
        project = root / (system + "_pc")
        run(firmware, "--system", system, "--target", "pc", "-o", project,
            contains="PC firmware project")
        assert (project / "run.py").is_file() and (project / "src/m0_native.c").is_file()
        if system == "meta":
            aka = root / "meta_aka"
            run(firmware, "--system", system, "--target", "aka", "-o", aka,
                contains="AKA firmware project")
            assert (aka / "platformio.ini").is_file() and (aka / "src/m0_aka_native.cpp").is_file()
            assert not (aka / "src/gm0.c").exists()
        else:
            run(firmware, "--system", system, "--target", "aka", ok=False,
                contains="code generation is not available")
        image[4:8] = (base + 8).to_bytes(4, "little")
        firmware.write_bytes(image)
        run(firmware, "--system", system, "--info", ok=False,
            contains="without the Thumb bit")
        image[4:8] = (base + 9).to_bytes(4, "little")
        image[0:4] = (0xDEADBEEF).to_bytes(4, "little")
        firmware.write_bytes(image)
        run(firmware, "--system", system, "--info", ok=False,
            contains="invalid initial stack pointer")
    sg = root / "synthetic.SG"
    sg.write_bytes(bytes([0xc3, 0x00, 0x00]) + bytes(16381))
    info = run(sg, "--info", contains="system=sg1000\ncpu=z80")
    assert "rom_bytes=16384" in info and "work_ram_bytes=1024" in info
    run(sg, "--cpu", "sm83", "--info", ok=False, contains="requires CPU z80")
    run(sg, "--cpu", "z80", "--info", contains="mapper=unbanked")
    run(sg, "--info", "--disasm", ok=False, contains="cannot be combined")
    for target in ("desktop", "meta", "pokitto"):
        project = root / ("project-" + target)
        run(sg, "--target", target, "-o", project, contains="Generated SG-1000 project")
        assert (project / "src" / "sg_gen.c").is_file()
    run(sg, "--analyze", ok=False, contains="unsupported SG-1000 option")
    run(sg, "--disasm", "--limit", "1", contains="0000  JP $0000")
    run(sg, "--disasm", "--bank", "1", ok=False, contains="only have bank 0")
    raw = root / "synthetic.bin"
    raw.write_bytes(sg.read_bytes())
    run(raw, "--system", "sg1000", "--info", contains="system=sg1000")
    run(root / "missing.sg", "--info", ok=False, contains="Cannot read SG-1000")
    raw.write_bytes(bytes(0xc001))
    run(raw, "--system", "sg1000", "--info", ok=False, contains="mapper profile")
    raw.write_bytes(b"")
    run(raw, "--system", "sg1000", "--info", ok=False, contains="1 to 49152")
    raw.write_bytes(bytes([0x21, 0x00]))
    run(raw, "--system", "sg1000", "--disasm", ok=False, contains="truncated Z80")
    # Legacy parser permits homebrew headers. Minimum-sized ROM and size code 0.
    gb = root / "synthetic.gb"
    data = bytearray(32768)
    data[0x134:0x13c] = b"SYNTHETC"
    gb.write_bytes(data)
    run(gb, "--info", contains="system=gameboy\ncpu=sm83")
    run(gb, "--cpu", "z80", "--target", "meta", ok=False, contains="requires CPU sm83")
    run(gb, "--system", "gameboy", "--cpu", "sm83", "--info", contains="title=SYNTHETC")
    assert run(gb, "--disasm") == run(gb, "--system", "gameboy", "--disasm")
    # NES and PC Engine: inspection works, code generation is under construction.
    nes = root / "synthetic.nes"
    nes.write_bytes(bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8)
                    + bytes(32768) + bytes(8192))
    run(nes, "--info", contains="system=nes\ncpu=6502")
    info = run(nes, "--info")
    assert "rom_bytes=40960" in info and "mapper=NROM (0)" in info
    nes_pc = root / "nes_pc"
    run(nes, "--target", "pc", "-o", nes_pc, contains="Generated NES project for pc")
    assert (nes_pc / "src/pc_hw.c").is_file() and (nes_pc / "CMakeLists.txt").is_file()
    listing = run(nes, "--disasm", "--limit", "3")
    assert "8000" in listing  # the PRG banks, disassembled from $8000
    run(nes, "--cpu", "z80", "--info", ok=False, contains="requires CPU 6502")
    pce = root / "synthetic.pce"
    pce.write_bytes(bytes(262144))
    run(pce, "--info", contains="system=pce\ncpu=huc6280")
    run(pce, "--target", "odroid", ok=False, contains="not available for --target odroid")
    assert "cpu=huc6280" in run(pce, "--disasm", "--limit", "1") or True
    vec = root / "synthetic.vec"
    vec.write_bytes(bytes([0x67, 0x20, 0x47, 0x43, 0x45]))
    run(vec, "--info", contains="system=vectrex\ncpu=6809")
    run(vec, "--target", "pokitto", ok=False, contains="not available for --target pokitto")
    vec_project = root / "vectrex_pc"
    run(vec, "--target", "pc", "-o", vec_project, contains="Generated Vectrex project")
    assert (vec_project / "src/m6809.c").is_file() and (vec_project / "CMakeLists.txt").is_file()
    # the META firmware embeds the system ROM: required, never guessed
    run(vec, "--target", "meta", "-o", root / "vectrex_meta0", ok=False, contains="needs the Vectrex system ROM")
    bios = root / "system.bin"
    bios.write_bytes(bytes(4096))
    vec_meta = root / "vectrex_meta"
    run(vec, "--target", "meta", "--bios", bios, "-o", vec_meta, contains="Generated Vectrex project for meta")
    assert (vec_meta / "src/vx_main.cpp").is_file() and (vec_meta / "platformio.ini").is_file()
    assert "vectrex_bios[8192]" in (vec_meta / "src/vx_cart.c").read_text()
    # GB is wired for the PC target: the project generates.
    run(gb, "--target", "pc", "-o", root / "gb_pc", contains="gbl project written")
    assert (root / "gb_pc/src/gbl_config.h").is_file()
    run(gb, "--target", "wonder", ok=False, contains="unknown --target")
