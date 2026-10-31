#!/usr/bin/env python3
"""Debug a running META firmware over its USB serial port (META_USB, meta_usb.c).

  meta-usb.py info                          firmware's answer to "V#"
  meta-usb.py read  --elf E SYMBOL [BYTES]  memory by symbol (or 0x address)
  meta-usb.py write --elf E SYMBOL VALUE    a 32-bit word (--byte: a byte), RAM only
  meta-usb.py prof  --elf E [--seconds 5]   PC samples (SysTick, meta_usb_pc) by function
  meta-usb.py press --elf E KEYS [--ms 150] hold buttons: down,left,right,up,a,b,menu,home
  meta-usb.py fault --elf E                 the last fault (kept across the automatic
                                            restart that follows it), with addr2line
  meta-usb.py eram  --elf E [--watch]       cartridge RAM read-table divergences
                                            (firmware built with -DGBL_DEBUG_ERAM_MAPS=1)
  meta-usb.py reset --elf E                 what the run before the last restart left
                                            (firmware built with -DMETA_DEBUG_RESET=1)
  meta-usb.py restart --elf E               buttons before each RAM enable write (game
                                            restart; firmware built with -DGBL_DEBUG_RESTART=1)
  meta-usb.py monitor [--baud N] [--log F]  any serial console as a printf terminal: bytes that are
                                            not a sam-ba answer (AKA's Serial.printf, any
                                            firmware's text) shown as lines; reconnects
  meta-usb.py boot                          1200-baud touch: the console goes to sam-ba
  meta-usb.py flash FIRMWARE.bin            boot, then bossac at 0x4000, then reset

The firmware answers sam-ba's commands (R, W, O, V), so reads go through the
same protocol as the bootloader's.  Profiling samples the PC SysTick interrupts
every millisecond into a ring of 32 (meta_usb_prof); the host drains it
every 20 ms (about 50 USB requests a second) and maps the PCs to the ELF's
functions.

The link is a plain serial line: sam-ba is only the request/answer protocol the META
firmware speaks on it (R, W, O, V), so `read`, `write`, `prof`... work on any
firmware that answers it, through its USB CDC or a USB-TTL adapter on a UART
(--port, --baud), with the ELF read by the target's own binutils (--prefix or
M0_TOOL_PREFIX).  Firmwares that only print (AKA's Serial.printf) use `monitor`.

Linux, macOS and Windows: ports come from pyserial's USB listing (no /dev path
assumed), tools are looked up with their .exe suffix, PATH, then PlatformIO's
packages.  `monitor` needs no ELF and no META: without --port it takes the META
firmware's port, else the only USB serial port present (several: it lists them).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import glob
import os
import shutil
import struct
import subprocess
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required (pip install pyserial)")

VID, APP_PID, BOOT_PID = 0x2341, 0x804D, 0x004D
BAUD = 115200          # --baud (a USB CDC ignores it; a real UART, e.g. a USB-TTL adapter, does not)
PREFIX = "arm-none-eabi-"  # --prefix: binutils of the target (xtensa-esp32s3-elf-, riscv32-esp-elf-, ...)
BUTTONS = {"down": 0x01, "left": 0x02, "right": 0x04, "up": 0x08, "a": 0x10, "b": 0x20, "menu": 0x40, "home": 0x80}


def find_port(pid: int, wait: float = 0.0) -> str | None:
    end = time.time() + wait
    while True:
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid == pid:
                return p.device
        if time.time() >= end:
            return None
        time.sleep(0.2)


class Console:
    def __init__(self, port: str | None):
        port = port or find_port(APP_PID)
        if not port:
            sys.exit("no META firmware with USB found (2341:804d)")
        self.s = serial.Serial(port, BAUD, timeout=1)
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def version(self) -> str:
        self.s.write(b"V#")
        time.sleep(0.1)
        return self.s.read(self.s.in_waiting or 1).decode(errors="replace").strip()

    def read(self, addr: int, n: int) -> bytes:
        self.s.write(b"R%08X,%08X#" % (addr, n))
        data = b""
        end = time.time() + 2
        while len(data) < n and time.time() < end:
            data += self.s.read(n - len(data))
        if len(data) != n:
            raise IOError("short read at 0x%08x (%d of %d bytes)" % (addr, len(data), n))
        return data

    def write(self, addr: int, value: int, size: int = 4) -> None:
        self.s.write(b"%s%08X,%08X#" % (b"W" if size == 4 else b"O", addr, value))
        self.s.flush()


EXE = ".exe" if os.name == "nt" else ""


def pio_dir() -> str:
    return os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")


def toolchain(tool: str) -> str:
    name = PREFIX + tool
    found = shutil.which(name)
    if found:
        return found
    for path in glob.glob(os.path.join(pio_dir(), "packages", "toolchain-*", "bin", name + EXE)):
        return path
    sys.exit(name + " not found (PATH or PlatformIO's toolchain)")


def symbols(elf: str) -> dict[str, tuple[int, int]]:
    out = subprocess.run([toolchain("nm"), "-S", "-C", elf], stdout=subprocess.PIPE, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        f = line.split(None, 3)
        if len(f) == 4:
            table[f[3]] = (int(f[0], 16), int(f[1], 16))
        elif len(f) == 3:
            table[f[2]] = (int(f[0], 16), 0)
    return table


def address(arg: str, elf: str | None) -> tuple[int, int]:
    if arg.lower().startswith("0x"):
        return int(arg, 16), 4
    if not elf:
        sys.exit("--elf is needed to use a symbol name")
    table = symbols(elf)
    if arg not in table:
        sys.exit("symbol not found: " + arg)
    return table[arg][0], table[arg][1] or 4


def functions(elf: str) -> tuple[list[int], list[str]]:
    out = subprocess.run([toolchain("nm"), "-n", "-C", elf], stdout=subprocess.PIPE, text=True, check=True).stdout
    starts, names = [], []
    for line in out.splitlines():
        f = line.split(None, 2)
        # code in flash, and the routines copied to SRAM (named in .data)
        if len(f) == 3 and (f[1] in "tTwW" or (f[1] in "dD" and int(f[0], 16) >= 0x20000000)):
            starts.append(int(f[0], 16) & ~1)
            names.append(f[2])
    return starts, names


def cmd_prof(c: Console, elf: str, seconds: float, top: int) -> None:
    ring = 32  # META_USB_PROF_SAMPLES
    prof = address("meta_usb_prof", elf)[0]
    starts, names = functions(elf)
    hist = collections.Counter()
    last, samples, lost = None, 0, 0
    end = time.time() + seconds
    while time.time() < end:
        words = struct.unpack("<%dI" % (1 + ring), c.read(prof, 4 * (1 + ring)))
        count = words[0]
        if last is not None:
            new = (count - last) & 0xFFFFFFFF
            if new > ring - 2:  # the ring went round (or nearly) since the last read
                lost += new - (ring - 2)
                new = ring - 2
            for k in range(count - new, count):
                pc = words[1 + (k & (ring - 1))]
                i = bisect.bisect_right(starts, pc & ~1) - 1
                hist[names[i] if i >= 0 else "0x%08x" % pc] += 1
                samples += 1
        last = count
        time.sleep(0.02)
    print("%d samples in %.1f s (%d lost)" % (samples, seconds, lost))
    for name, n in hist.most_common(top):
        print("%6.2f%%  %6d  %s" % (100.0 * n / max(samples, 1), n, name))


def cmd_fault(c: Console, elf: str) -> None:
    w = struct.unpack("<13I", c.read(address("meta_fault", elf)[0], 52))
    magic, count, rcause, early, frame, exception = w[0], w[1], w[2], w[3], w[4:12], w[12]
    causes = {0x01: "power-on", 0x02: "brown-out 1.2 V", 0x04: "brown-out 3.3 V", 0x10: "RESET button",
              0x20: "watchdog (a lockup: interrupts were off)", 0x40: "software (sam-ba, or a restart after a fault)"}
    print("this boot: %s" % ", ".join(n for b, n in causes.items() if rcause & b) or hex(rcause))
    if magic != 0x46415532 or not count:
        print("no fault recorded")
        return
    kind = {2: "NMI", 3: "HardFault", 18: "watchdog early warning: main loop stuck"}.get(exception, "IRQ %d" % (exception - 16))
    print("%d fault(s) logged; the last: exception %d (%s)" % (count, exception, kind))
    if early >= 3:
        print("%d in a row right after boot: restarts stopped, the firmware waits for the host" % early)
    for n, v in zip(["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr"], frame):
        print("%-5s 0x%08x" % (n, v))
    out = subprocess.run([toolchain("addr2line"), "-f", "-C", "-e", elf, "0x%x" % frame[6], "0x%x" % frame[5]],
                         stdout=subprocess.PIPE, text=True).stdout.split("\n")
    print("pc in %s (%s)" % (out[0], out[1]))
    print("lr in %s (%s)" % (out[2], out[3]))


ERAM_REC = struct.Struct("<3I2H4B2b2B16h16b16b4BI")  # gbl_dbg_eram_rec_t (gbl.h)


def eram_rec(name: str, r: tuple) -> None:
    frame, got_ptr, want_ptr, addr, pc, got_v, want_v, bank, ram_enable, got_way, want_way, mru, ways = r[:13]
    tags, rslots, wslots, kind, flags = r[13:29][:ways], r[29:45], r[45:61], r[61], r[65]
    print("%s: frame %d, %s %04X (bank %d, RAM %s), GB pc %04X" %
          (name, frame, "write" if kind else "read", addr, bank, "on" if ram_enable else "OFF", pc))
    print("  table: way %d @0x%08x byte %02X ; search: way %d @0x%08x byte %02X" %
          (got_way, got_ptr, got_v, want_way, want_ptr, want_v))
    print("  ways (sector, r=referenced d=dirty; newest %d): %s" % (mru, " ".join(
        "%d:%d%s%s" % (i, t, "r" if flags >> (2 * i) & 1 else "", "d" if flags >> (2 * i) & 2 else "")
        for i, t in enumerate(tags))))
    for label, slots in (("read ", rslots), ("write", wslots)):
        print("  %s slots A000+n*200 -> way: %s" %
              (label, " ".join("%X:%s" % (i, {-1: "-", -2: "??"}.get(w, str(w))) for i, w in enumerate(slots))))


def cmd_eram(c: Console, elf: str, watch: bool) -> None:
    addr = address("gbl_dbg_eram", elf)[0]
    seen = -1
    while True:
        data = c.read(addr, 16 + 2 * ERAM_REC.size)
        reads, writes, errors, rebuilds = struct.unpack_from("<4I", data)
        if errors != seen or not watch:
            print("%s  %d reads, %d writes checked, %d table rebuilds, %d divergences" %
                  (time.strftime("%H:%M:%S"), reads, writes, rebuilds, errors))
            if errors:
                eram_rec("first", ERAM_REC.unpack_from(data, 16))
                if errors > 1:
                    eram_rec("last", ERAM_REC.unpack_from(data, 16 + ERAM_REC.size))
            sys.stdout.flush()
            seen = errors
        if not watch:
            return
        time.sleep(1)


def cmd_reset(c: Console, elf: str) -> None:
    w = struct.unpack("<46I", c.read(address("meta_reset_trace", elf)[0], 184))
    magic, boots, lr, sp, ipsr, ticks = w[:6]
    prof, regs, frame = w[6:39], struct.pack("<6I", *w[39:45]), w[45]
    if magic != 0x52535431:
        print("no trace")
        return
    starts, names = functions(elf)

    def where(pc: int) -> str:
        i = bisect.bisect_right(starts, pc & ~1) - 1
        return "%s+0x%x" % (names[i], (pc & ~1) - starts[i]) if i >= 0 and pc >= 0x4000 else "0x%08x" % pc

    print("boots since the trace began: %d; this boot: entry lr 0x%08x (%s), sp 0x%08x, ipsr %d" %
          (boots, lr, where(lr), sp, ipsr))
    print("previous run: %.1f s, Game Boy frame %d" % (ticks / 1000.0, frame))
    bc, de, hl, gsp, gpc = struct.unpack_from("<5H", regs)
    a = regs[10]
    print("Game Boy: pc %04X sp %04X a %02X bc %04X de %04X hl %04X bank %d" %
          (gpc, gsp, a, bc, de, hl, struct.unpack_from("<H", regs, 16)[0]))
    count = prof[0]
    print("last PC samples (1 ms apart, oldest first):")
    for k in range(max(0, count - 32), count):
        pc = prof[1 + (k & 31)]
        print("  0x%08x  %s" % (pc, where(pc)))


RESTART_SNAP = struct.Struct("<2I2H4B16B16H")  # gbl_dbg_restart_snap_t (gbl.h)
GB_KEYS = ["A", "B", "Sel", "Start", "R", "L", "U", "D"]


def gb_keys(b: int) -> str:
    return "+".join(k for i, k in enumerate(GB_KEYS) if b >> i & 1) or "-"


def cmd_stack(c: Console, elf: str) -> None:
    """gbl_dbg_stack (GBL_DEBUG_STACK): the pop check and the last writer of
    each shadowed byte."""
    base = address("gbl_dbg_stack", elf)[0]
    head = c.read(base, 32) + c.read(base + 32, 8)
    magic, pops, checked, bad, frozen, frozen_frame, bad_frame = struct.unpack_from("<7I", head)
    if magic != 0x4B415453:
        sys.exit("gbl_dbg_stack: no magic (firmware without GBL_DEBUG_STACK?)")
    bad_sp, bad_addr, bad_pc, bad_bank, got, want, n = struct.unpack_from("<4H2BH", head, 28)
    base_addr = 0xA900 - n
    tail = 40 + n + n // 8 + 8 * n
    data = b"".join(c.read(base + o, 32) for o in range(0, tail + 60 + 2 * n, 32))
    now = struct.unpack("<I", c.read(address("gbl_frame", elf)[0], 4))[0]
    print("pops %d, bytes checked %d, wrong %d; now frame %d" % (pops, checked, bad, now))
    print("frozen: %s" % {0: "no", 1: "RAM switched off at frame %d" % frozen_frame,
                          2: "wrong pop at frame %d" % frozen_frame}.get(frozen, str(frozen)))
    if bad:
        print("first wrong pop: frame %d, sp %04X, [%04X] read %02X, shadow %02X, pc %02X:%04X" %
              (bad_frame, bad_sp, bad_addr, got, want, bad_bank, bad_pc))
    value, valid = data[40:40 + n], data[40 + n:40 + n + n // 8]
    o = 40 + n + n // 8
    pcs, sps, frames, lrs = (struct.unpack_from("<%dH" % n, data, o + k * 2 * n) for k in range(4))
    starts, names = functions(elf)

    def where(pc: int) -> str:
        i = bisect.bisect_right(starts, pc & ~1) - 1
        return "%s+0x%x" % (names[i], (pc & ~1) - starts[i]) if i >= 0 else "0x%08x" % pc

    loads, wbs, load_frame, wb_frame, flags = struct.unpack_from("<5I", data, tail)
    tags = struct.unpack_from("<16h", data, tail + 20)
    rway, wway, tway, mru, hand = struct.unpack_from("<5b", data, tail + 52)
    at_loads, actual = data[tail + 60:tail + 60 + n], data[tail + 60 + n:tail + 60 + 2 * n]
    print("stack sector: %d loads into the cache (last frame %d), %d write-backs (last frame %d)" %
          (loads, load_frame, wbs, wb_frame))
    if bad:
        ways = [i for i, t in enumerate(tags) if t != -2]
        print("at the wrong pop: read table way %d, write table way %d, search way %d; mru %d, hand %d" %
              (rway, wway, tway, mru, hand))
        print("  ways (tag ref dirty): " + "  ".join(
            "%d:%d%s%s" % (w, tags[w], "R" if flags >> (2 * w) & 1 else "-", "D" if flags >> (2 * w + 1) & 1 else "-")
            for w in ways))
    print("addr  val  cache  loads@w  frame(lo16)  GB pc  GB sp  native caller (lr/4*4)")
    for i in range(n):
        if valid[i >> 3] >> (i & 7) & 1:
            mark = "  " if not bad or actual[i] == value[i] else " *"
            print("%04X  %02X   %02X%s   %3d      %6d       %04X   %04X   %s" %
                  (base_addr + i, value[i], actual[i], mark, at_loads[i], frames[i], pcs[i], sps[i],
                   where(lrs[i] << 2 | 1)))


TRACE_REC = struct.Struct("<I4H4B")  # meta_main.cpp meta_trace_rec
TRACE_LEN = 16


def cmd_trace(port: str | None, elf: str, out: str, seconds: float) -> None:
    """META_DEBUG_TRACE: append every frame's record (meta_trace ring) to out,
    reconnecting when the console goes away; a line per missed run."""
    ring, pos_addr = address("meta_trace", elf)[0], address("meta_trace_pos", elf)[0]
    end = time.time() + seconds if seconds else None
    last, c, f = None, None, open(out, "a", buffering=1)
    f.write("# %s start; frame pc sp hl [sp] bank a ime reinits save_errors\n" % time.strftime("%F %T"))
    same_pc, shown = 0, time.time()
    while end is None or time.time() < end:
        try:
            if c is None:
                c = Console(port)
                f.write("# %s connected\n" % time.strftime("%T"))
            pos = struct.unpack("<I", c.read(pos_addr, 4))[0]
            if last is None or pos < last:
                last = max(0, pos - TRACE_LEN)
            if pos - last > TRACE_LEN:
                f.write("# %d records missed\n" % (pos - last - TRACE_LEN))
                last = pos - TRACE_LEN
            if pos > last:
                data = b"".join(c.read(ring + o, 32) for o in range(0, TRACE_REC.size * TRACE_LEN, 32))
                for k in range(last, pos):
                    frame, pc, sp, hl, top, bank, a, flags, errs = TRACE_REC.unpack_from(
                        data, (k % TRACE_LEN) * TRACE_REC.size)
                    f.write("%d %04X %04X %04X %04X %02X %02X %d %d %d\n" %
                            (frame, pc, sp, hl, top, bank, a, flags & 1, flags >> 1, errs))
                    same_pc = same_pc + 1 if pc == getattr(cmd_trace, "pc", -1) else 0
                    cmd_trace.pc = pc
                last = pos
            if time.time() - shown > 10:
                shown = time.time()
                print("%s records %d%s" % (time.strftime("%T"), pos,
                                          ", pc %04X unchanged for %d frames" % (cmd_trace.pc, same_pc)
                                          if same_pc > 120 else ""), flush=True)
        except (IOError, OSError, serial.SerialException, SystemExit) as e:
            if c is not None:
                f.write("# %s lost the console: %s\n" % (time.strftime("%T"), e))
            c = None
            time.sleep(1)
            continue
        time.sleep(0.15)


def cmd_restart(c: Console, elf: str) -> None:
    base = address("gbl_dbg_restart", elf)[0]
    data = b"".join(c.read(base + o, 32) for o in range(0, 60 + 2 * RESTART_SNAP.size + 76, 32))
    writes, combo, pos = struct.unpack_from("<3I", data)
    print("RAM enable writes: %d; frames with A+B+Select+Start held: %d; now frame %d" %
          (writes, combo, struct.unpack("<I", c.read(address("gbl_frame", elf)[0], 4))[0]))
    for k in range(max(0, writes - 2), writes):
        r = RESTART_SNAP.unpack_from(data, 60 + (k & 1) * RESTART_SNAP.size)
        frame, combo_then, addr, sp, value, ram_enable, bank = r[:7]
        keys, kframes = r[8:24], r[24:40]
        print("write %d: frame %d, [%04X] = %02X (RAM was %s), GB sp %04X, bank %d, combo frames so far %d" %
              (k + 1, frame, addr, value, "on" if ram_enable else "off", sp, bank, combo_then))
        print("  button changes before it (frame: keys):")
        print("   " + "  ".join("%d:%s" % (f, gb_keys(b)) for b, f in zip(keys, kframes) if f or b))
    o = 60 + 2 * RESTART_SNAP.size
    bank_pos, lock_frame, lock_pc, lock_bank = struct.unpack_from("<2I2H", data, o)
    vals, shadows = data[o + 12:o + 28], data[o + 28:o + 44]
    frames = struct.unpack_from("<16H", data, o + 44)
    if lock_frame:
        print("undefined opcode first hit: frame %d, pc %02X:%04X (ROM bank writes frozen there)" %
              (lock_frame, lock_bank, lock_pc))
    off = address("gbl_dbg_off", elf)[0]
    od = b"".join(c.read(off + k, 32) for k in range(0, 192, 32))
    frame, lr, sp, a_, v_ = struct.unpack_from("<3I2H", od)
    if frame:
        starts, names = functions(elf)

        def where(pc: int) -> str:
            i = bisect.bisect_right(starts, pc & ~1) - 1
            return "%s+0x%x" % (names[i], (pc & ~1) - starts[i]) if i >= 0 else "0x%08x" % pc

        regs = od[16:40]
        bc, de, hl, gsp, gpc = struct.unpack_from("<5H", regs)
        print("first RAM switch-off: frame %d, [%04X] = %02X, from %s (C sp 0x%08x)" % (frame, a_, v_, where(lr), sp))
        print("  Game Boy: pc %04X (last stored) sp %04X a %02X bc %04X de %04X hl %04X bank %d" %
              (gpc, gsp, regs[10], bc, de, hl, struct.unpack_from("<H", regs, 16)[0]))
        print("  Game Boy stack from sp-4: " + od[40:56].hex(" "))
        words = struct.unpack_from("<32I", od, 56)
        print("  code addresses on the C stack: " + "  ".join(
            where(w) for w in words if w & 1 and 0x4000 <= w < 0x40000))
    bi = address("gbl_dbg_badirq", elf)[0]
    bd = b"".join(c.read(bi + k, 32) for k in range(0, 224, 32))
    magic, frame, lr, csp = struct.unpack_from("<4I", bd)
    if magic == 0x51524942 and frame:
        starts, names = functions(elf)

        def where2(pc: int) -> str:
            i = bisect.bisect_right(starts, pc & ~1) - 1
            return "%s+0x%x" % (names[i], (pc & ~1) - starts[i]) if i >= 0 else "0x%08x" % pc

        pc, sp, bc, de, hl, bank = struct.unpack_from("<6H", bd, 16)
        a, ime = bd[28], bd[29]
        print("interrupt taken at pc %02X:%04X (frame %d), from %s (C sp 0x%08x)" % (bank, pc, frame, where2(lr), csp))
        print("  Game Boy: sp %04X a %02X bc %04X de %04X hl %04X; stack from sp-4: %s" %
              (sp, a, bc, de, hl, bd[30:44].hex(" ")))
        words = struct.unpack_from("<40I", bd, 44)
        print("  code addresses on the C stack: " + "  ".join(
            where2(w) for w in words if w & 1 and 0x4000 <= w < 0x40000))
    tb = address("gbl_dbg_tables", elf)[0]
    td = b"".join(c.read(tb + k, 32) for k in range(0, 224, 32))
    magic, checks, bad, echo_n, echo_last, echo_frame = struct.unpack_from("<6I", td)
    if magic == 0x54424C31:
        print("table checks: %d, wrong entries: %d; echo RAM writes: %d (last [%04X]=%02X at frame %d)" %
              (checks, bad, echo_n, echo_last >> 16, echo_last >> 8 & 255, echo_frame))
        syms = {v[0]: k for k, v in symbols(elf).items()}
        for k in range(min(bad, 2)):
            r = struct.unpack_from("<6I17I", td, 24 + k * 92)
            frame, where_, slot, addr, badv, want = r[:6]
            print("  %s: frame %d (%s), %s[%d] @0x%08x = 0x%08x, should be 0x%08x" %
                  ("first" if k == 0 else "last", frame, "VBlank" if where_ == 0 else "fault",
                   "gbl_eram_rmap" if slot < 16 else "gbl_eram_wmap", slot % 16, addr, badv, want))
            print("    words around (-8..+8): " + " ".join("%08x" % w for w in r[6:]))
    ib = address("gbl_dbg_irq", elf)[0]
    idt = b"".join(c.read(ib + k, 32) for k in range(0, 96, 32))
    magic, entries, checked, ibad, unmatched = struct.unpack_from("<5I", idt)
    if magic == 0x49525131:
        print("interrupts: %d entries, %d RETI checked, %d with registers not restored, %d left without RETI" %
              (entries, checked, ibad, unmatched))
        if ibad:
            for label, o in (("entry", 20), ("RETI ", 40)):
                fr, isp, ipc, ibc, ide, ihl, ibank, ia, iff, vec = struct.unpack_from("<I6H3B", idt, o)
                print("  %s: frame %d vector %02X  pc %04X sp %04X a %02X f %02X bc %04X de %04X hl %04X bank %d" %
                      (label, fr, vec, ipc, isp, ia, iff, ibc, ide, ihl, ibank))
            print("  Game Boy stack from sp-16 at the RETI: " + idt[60:84].hex(" "))
    print("last ROM bank writes (frame: bank, $A24E then):")
    print("   " + "  ".join("%d:%d,%s" % (frames[k & 15], vals[k & 15], "%d" % shadows[k & 15] if shadows[k & 15] != 0xEE else "?")
                            for k in range(max(0, bank_pos - 16), bank_pos)))


def monitor_port(port: str | None) -> str | None:
    if port:
        return port
    found = find_port(APP_PID)
    if found:
        return found
    usb = [p for p in serial.tools.list_ports.comports() if p.vid is not None]
    if len(usb) == 1:
        return usb[0].device
    if usb:
        sys.exit("several serial ports, pick one with --port:\n" +
                 "\n".join("  %s  %04x:%04x  %s" % (p.device, p.vid, p.pid, p.description) for p in usb))
    return None


def printable(b: bytes) -> str:
    return b.decode("utf-8", errors="backslashreplace").replace("\t", "    ")


def cmd_monitor(port: str | None, baud: int, log: str | None, stamp: bool) -> None:
    """Every received byte is text: split on newlines, partial lines flushed after
    200 ms of silence, undecodable bytes shown as \\xNN.  Reconnects when the
    console goes away (reset, flash, cable)."""
    f = open(log, "a", buffering=1) if log else None

    def emit(line: str) -> None:
        line = line.rstrip("\r")
        if stamp:
            line = time.strftime("%H:%M:%S ") + line
        print(line, flush=True)
        if f:
            f.write(line + "\n")

    s, buf, last, announced = None, b"", time.time(), False
    try:
        while True:
            try:
                if s is None:
                    dev = monitor_port(port)
                    if not dev:
                        if not announced:
                            print("waiting for a serial port...", file=sys.stderr, flush=True)
                            announced = True
                        time.sleep(0.5)
                        continue
                    s = serial.Serial(dev, baud, timeout=0.05)
                    announced = False
                    print("# %s %d baud" % (dev, baud), file=sys.stderr, flush=True)
                data = s.read(4096)
            except (OSError, serial.SerialException):
                if s is not None:
                    print("# console lost", file=sys.stderr, flush=True)
                    try:
                        s.close()
                    except Exception:
                        pass
                s, buf = None, b""
                time.sleep(0.5)
                continue
            if data:
                buf += data
                last = time.time()
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    emit(printable(line))
            elif buf and time.time() - last > 0.2:
                emit(printable(buf))
                buf = b""
    except KeyboardInterrupt:
        pass


def cmd_boot(port: str | None) -> None:
    port = port or find_port(APP_PID)
    if not port:
        sys.exit("no META firmware with USB found (2341:804d)")
    s = serial.Serial(port, 1200)
    time.sleep(0.1)
    s.close()
    boot = find_port(BOOT_PID, wait=10)
    if not boot:
        sys.exit("the bootloader did not show up")
    time.sleep(2)  # sam-ba's CDC ignores early requests
    print(boot)


def cmd_flash(port: str | None, firmware: str) -> None:
    boot = find_port(BOOT_PID)
    if not boot:
        cmd_boot(port)
        boot = find_port(BOOT_PID, wait=10)
    bossac = shutil.which("bossac") or os.path.join(pio_dir(), "packages", "tool-bossac", "bossac" + EXE)
    subprocess.run([bossac, "-p", boot, "-o", "0x4000", "-e", "-w", "-v", firmware, "-R"], check=True)


def main() -> int:
    global BAUD, PREFIX
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="any serial device (COM3, /dev/ttyUSB0, /dev/cu.usbmodem..); "
                    "without it the META firmware's USB id is looked for")
    ap.add_argument("--baud", type=int, default=BAUD, help="serial speed (a real UART needs it right)")
    ap.add_argument("--prefix", default=os.environ.get("M0_TOOL_PREFIX", PREFIX),
                    help="binutils prefix of the target's ELF, for nm/addr2line (default arm-none-eabi-)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    r = sub.add_parser("read")
    r.add_argument("--elf")
    r.add_argument("what")
    r.add_argument("bytes", nargs="?", type=int)
    w = sub.add_parser("write")
    w.add_argument("--elf")
    w.add_argument("--byte", action="store_true")
    w.add_argument("what")
    w.add_argument("value", type=lambda v: int(v, 0))
    p = sub.add_parser("prof")
    p.add_argument("--elf", required=True)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--top", type=int, default=25)
    k = sub.add_parser("press")
    k.add_argument("--elf", required=True)
    k.add_argument("--ms", type=int, default=150)
    k.add_argument("keys", help="comma-separated: " + ",".join(BUTTONS))
    q = sub.add_parser("fault")
    q.add_argument("--elf", required=True)
    e = sub.add_parser("eram")
    e.add_argument("--elf", required=True)
    e.add_argument("--watch", action="store_true", help="poll every second, print each change")
    u = sub.add_parser("restart")
    u.add_argument("--elf", required=True)
    tr = sub.add_parser("trace")
    tr.add_argument("--elf", required=True)
    tr.add_argument("--out", required=True)
    tr.add_argument("--seconds", type=float, default=0, help="0: until interrupted")
    g = sub.add_parser("stack")
    g.add_argument("--elf", required=True)
    t = sub.add_parser("reset")
    t.add_argument("--elf", required=True)
    mo = sub.add_parser("monitor")
    mo.add_argument("--log", help="also append the lines to this file")
    mo.add_argument("--time", action="store_true", help="prefix each line with the time")
    sub.add_parser("boot")
    f = sub.add_parser("flash")
    f.add_argument("firmware")
    a = ap.parse_args()
    BAUD, PREFIX = a.baud, a.prefix

    if a.cmd == "boot":
        cmd_boot(a.port)
        return 0
    if a.cmd == "flash":
        cmd_flash(a.port, a.firmware)
        return 0
    if a.cmd == "monitor":
        cmd_monitor(a.port, BAUD, a.log, a.time)
        return 0
    if a.cmd == "trace":
        cmd_trace(a.port, a.elf, a.out, a.seconds)
        return 0
    c = Console(a.port)
    if a.cmd == "info":
        print(c.version())
    elif a.cmd == "read":
        addr, size = address(a.what, a.elf)
        data = c.read(addr, a.bytes or size)
        if len(data) in (1, 2, 4) and not a.bytes:
            v = int.from_bytes(data, "little")
            print("0x%08x: %d (0x%x)" % (addr, v, v))
        else:
            for i in range(0, len(data), 16):
                print("%08x  %s" % (addr + i, data[i:i + 16].hex(" ")))
    elif a.cmd == "write":
        addr, _ = address(a.what, a.elf)
        c.write(addr, a.value, 1 if a.byte else 4)
    elif a.cmd == "prof":
        cmd_prof(c, a.elf, a.seconds, a.top)
    elif a.cmd == "fault":
        cmd_fault(c, a.elf)
    elif a.cmd == "restart":
        cmd_restart(c, a.elf)
    elif a.cmd == "reset":
        cmd_reset(c, a.elf)
    elif a.cmd == "stack":
        cmd_stack(c, a.elf)
    elif a.cmd == "eram":
        cmd_eram(c, a.elf, a.watch)
    elif a.cmd == "press":
        bits = 0
        for key in a.keys.lower().split(","):
            if key not in BUTTONS:
                sys.exit("unknown key: " + key)
            bits |= BUTTONS[key]
        addr, _ = address("meta_usb_buttons", a.elf)
        c.write(addr, bits, 1)
        time.sleep(a.ms / 1000.0)
        c.write(addr, 0, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
