#!/usr/bin/env python3
"""A GP32 native project (--coverage) builds and ends in the interpreter's
machine state, on a synthetic ARM and Thumb application.

usage: test_gp32_native.py M0RECOMP SOURCE_DIR
"""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

BASE = 0x0C000000


def application():
    """A loop on the pad register: a conditional branch, a computed jump
    through a table, a Thumb routine and a counter kept in SDRAM."""
    ldr = lambda rd, at, literal: 0xE59F0000 | rd << 12 | (literal - at - 8)
    jump = lambda op, at, to: op | ((to - at - 8) >> 2 & 0xFFFFFF)
    words = [
        ldr(1, 0x1C, 0x70),            # 1c ldr r1, =PEDAT
        0xE5910000,                    # 20 ldr r0, [r1]
        0xE3100C01,                    # 24 tst r0, #0x100
        jump(0x0A000000, 0x28, 0x34),  # 28 beq 34
        0xE2833001,                    # 2c add r3, r3, #1
        jump(0xEA000000, 0x30, 0x38),  # 30 b 38
        0xE2833002,                    # 34 add r3, r3, #2
        0xE2032001,                    # 38 and r2, r3, #1
        0xE79FF102,                    # 3c ldr pc, [pc, r2, lsl #2]
        0xE1A00000,                    # 40 nop
        BASE + 0x4C, BASE + 0x54,      # 44 the table
        0xE2833004,                    # 4c add r3, r3, #4
        jump(0xEA000000, 0x50, 0x58),  # 50 b 58
        0xE2833008,                    # 54 add r3, r3, #8
        ldr(4, 0x58, 0x74),            # 58 ldr r4, =thumb
        0xE1A0E00F,                    # 5c mov lr, pc
        0xE12FFF14,                    # 60 bx r4
        ldr(5, 0x64, 0x78),            # 64 ldr r5, =counter
        0xE5853000,                    # 68 str r3, [r5]
        jump(0xEA000000, 0x6C, 0x1C),  # 6c b 1c
        0x15600030, BASE + 0x7C + 1, BASE + 0x1000,
        0x47702001,                    # 7c movs r0, #1 ; bx lr
    ]
    size = 0x1C + 4 * len(words)
    head = [jump(0xEA000000, 0, 0x1C), BASE] + [BASE + size] * 5
    return struct.pack("<%dI" % (7 + len(words)), *head, *words)


def fxe(payload):
    """The FXE wrapper with a 4 byte key of zeros."""
    header = struct.pack("<4sII32s", b"fxe ", 32, 0x454, b"GP32 native").ljust(1116, b"\0")
    return header + struct.pack("<II", len(payload), 4) + b"\xff" * 4 + payload


def run(*command, **options):
    result = subprocess.run([str(item) for item in command], capture_output=True, text=True, **options)
    if result.returncode:
        sys.exit(f"{command}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    m0recomp, source = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        payload = application()
        image = payload + bytes(4)
        (work / "app.fxe").write_bytes(fxe(payload))
        code = work / "app.code"
        (work / "app.code.image.bin").write_bytes(image)
        # 34 (one side of the branch) is left out: the interpreter must run it.
        code.write_text("# deepeel gp32 code v1: offset length class mode\n"
                        f"base {BASE:08x}\nimage {len(image)} {hashlib.sha1(image).hexdigest()}\n"
                        "0000001c 00000018 natural arm\n00000038 0000000c natural arm\n"
                        "0000004c 00000024 static arm\n0000007c 00000004 natural thumb\n")
        project = work / "project"
        out = run(m0recomp, work / "app.fxe", "--target", "desktop", "--coverage", code, "-o", project,
                  "--meta-dir", source / "targets/meta")
        assert "GP32 native project" in out, out
        states = {}
        for name, directory in (("native", project), ("interpreter", project / "gp32/runtime")):
            build = work / name
            run("cmake", "-S", directory, "-B", build, "-DM0_GP32_LTO=OFF", env=environment)
            run("cmake", "--build", build, "--target", "m0_gp32_bench", "--parallel", "2", env=environment)
            bench = next(build.rglob("m0_gp32_bench"))
            out = run(bench, "--hle", "2000000", "--exec", work / "app.fxe", env=environment)
            states[name] = re.search(r"state=([0-9a-f]+)", out).group(1)
            if name == "native":
                share = float(re.search(r"interpreted=([0-9.]+)%", out).group(1))
                assert 0 < share < 25, out
        assert states["native"] == states["interpreter"], states
        # The profile guided hybrid: a low share leaves covered code, ARM and
        # Thumb, to the loop which runs it with the interpreter.
        runtime, counts = project / "gp32/runtime", work / "counts.txt"
        recompile = [sys.executable, "-P", "-B", runtime / "recompile.py", code, work / "app.code.image.bin"]

        def hybrid(name, *options):
            generated = work / f"{name}-aot/app_aot.c"
            generated.parent.mkdir()
            run(*recompile, generated, *options)
            build = work / name
            run("cmake", "-S", runtime, "-B", build, "-DM0_GP32_LTO=OFF", f"-DM0_GP32_AOT={generated}",
                env=environment)
            run("cmake", "--build", build, "--target", "m0_gp32_bench", "--parallel", "2", env=environment)
            bench = next(build.rglob("m0_gp32_bench"))
            return run(bench, "--hle", "2000000", "--exec", work / "app.fxe",
                       env=dict(environment, M0_GP32_COUNTS=str(counts)) if name == "count" else environment)

        hybrid("count", "--count")
        out = hybrid("whole", "--hybrid")
        assert re.search(r"state=([0-9a-f]+)", out).group(1) == states["interpreter"], out
        for share in ("1", "60", "100"):
            out = hybrid(f"hybrid{share}", "--hybrid", "--hot", counts, "--share", share)
            assert re.search(r"state=([0-9a-f]+)", out).group(1) == states["interpreter"], (share, out)
            assert float(re.search(r"interpreted=([0-9.]+)%", out).group(1)) < 25, (share, out)
        # A map without its image is refused.
        (work / "app.code.image.bin").unlink()
        failed = subprocess.run([str(m0recomp), str(work / "app.fxe"), "--target", "desktop", "--coverage",
                                 str(code), "-o", str(work / "refused"), "--meta-dir",
                                 str(source / "targets/meta")], capture_output=True, text=True)
        assert failed.returncode and "missing the covered image" in failed.stderr, failed.stderr
    print("ok")


if __name__ == "__main__":
    main()
