"""Replay a WonderSwan BK2 against BizHawk's native Cygne/Mednafen core."""

import argparse
import ctypes as c
from datetime import datetime, timezone
import hashlib
import json
import sys
import zlib
from contextlib import ExitStack
from pathlib import Path
from zipfile import ZipFile


class SyncSettings(c.Structure):
    _fields_ = [("initial_time", c.c_uint64)] + [
        (name, c.c_uint32)
        for name in ("year", "month", "day", "color", "real_time", "language", "sex", "blood")
    ] + [("name", c.c_char * 17)]


class VideoSettings(c.Structure):
    _fields_ = [("layers", c.c_uint32), ("gray", c.c_uint32 * 16),
                ("rgb", c.c_uint32 * 4096)]


def rgb_bytes(screen):
    argb = memoryview(screen).cast("B")
    channel_offsets = (2, 1, 0) if sys.byteorder == "little" else (1, 2, 3)
    output = bytearray(len(screen) * 3)
    for channel, offset in enumerate(channel_offsets):
        output[channel::3] = argb[offset::4].tobytes()
    return output


def unrotate(rgb):
    width, height = 224, 144
    output = bytearray(len(rgb))
    for y in range(height):
        start = y * width * 3
        for channel in range(3):
            column = rgb[3 * y + channel::height * 3]
            output[start + channel:start + width * 3:3] = column[::-1]
    return output


def read_movie(path, rom):
    with ZipFile(path) as archive:
        header = dict(line.split(" ", 1) for line in
                      archive.read("Header.txt").decode().splitlines() if " " in line)
        if header.get("Platform") != "WSWAN" or header.get("SHA1", "").lower() != hashlib.sha1(rom).hexdigest():
            raise ValueError("The BK2 platform or ROM SHA-1 does not match")
        settings = json.loads(archive.read("SyncSettings.json"))["o"]
        lines = archive.read("Input Log.txt").decode().splitlines()

    expected = ["Power", "Rotate"]
    for player in (1, 2):
        expected.extend(f"P{player} {key}" for key in
                        ("X1", "X2", "X3", "X4", "Y1", "Y2", "Y3", "Y4", "Start", "B", "A"))
    key = next(line[7:] for line in lines if line.startswith("LogKey:"))
    if key.replace("#", "").rstrip("|").split("|") != expected:
        raise ValueError("Unexpected WonderSwan BK2 controller layout")

    def mask(pad):
        return sum(1 << bit for bit, button in enumerate(pad[:9] + pad[10] + pad[9])
                   if button != ".")

    for line in lines:
        if not line.startswith("|"):
            continue
        flags, normal, rotated = line.strip("|").split("|")
        if [len(flags), len(normal), len(rotated)] != [2, 11, 11]:
            raise ValueError("Invalid WonderSwan BK2 input row")
        yield settings, flags[0] != ".", (
            mask(normal) | (mask(rotated) << 16) | ((flags[1] != ".") << 31)
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("movie", type=Path)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--input-out", type=Path)
    parser.add_argument("--settings-out", type=Path,
                        help="write the cartridge model from the BK2 sync settings as JSON")
    parser.add_argument("--host-options-out", type=Path,
                        help="write ws_host options matching the BK2 hardware and timing")
    parser.add_argument("--settings-only", action="store_true",
                        help="export sync settings without loading the BizHawk library")
    parser.add_argument("--capture", type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    with ZipFile(args.movie) as archive:
        movie_settings = json.loads(archive.read("SyncSettings.json"))["o"]
    if args.settings_out:
        args.settings_out.write_text(json.dumps({
            "color": bool(movie_settings["Color"]),
            "initial_time": movie_settings["InitialTime"],
            "use_real_time": bool(movie_settings["UseRealTime"]),
            "birth_date": movie_settings["BirthDate"],
            "language": movie_settings["Language"],
            "gender": movie_settings["Gender"],
            "blood_type": movie_settings["BloodType"],
            "name": movie_settings["Name"],
        }, indent=2) + "\n", encoding="utf-8")
    if args.host_options_out:
        options = ["--bizhawk-timing"]
        if movie_settings["Color"]:
            options.append("--color")
        args.host_options_out.write_text("\n".join(options) + "\n", encoding="ascii")
    if args.settings_only:
        return 0
    lib = c.CDLL(str(args.library.resolve()))
    lib.bizswan_new.restype = c.c_void_p
    lib.bizswan_delete.argtypes = [c.c_void_p]
    lib.bizswan_reset.argtypes = [c.c_void_p]
    lib.bizswan_load.argtypes = [c.c_void_p, c.c_char_p, c.c_int,
                                c.POINTER(SyncSettings), c.POINTER(c.c_int)]
    lib.bizswan_advance.argtypes = [c.c_void_p, c.c_uint32, c.c_bool,
                                   c.POINTER(c.c_uint32), c.POINTER(c.c_int16),
                                   c.POINTER(c.c_int), c.POINTER(c.c_int)]
    lib.bizswan_getnecreg.argtypes = [c.c_void_p, c.c_int]
    lib.bizswan_getnecreg.restype = c.c_uint32
    lib.bizswan_putsettings.argtypes = [c.c_void_p, c.POINTER(VideoSettings)]
    lib.bizswan_putsettings.restype = None

    core = lib.bizswan_new()
    screen = (c.c_uint32 * (224 * 144))()
    audio = (c.c_int16 * 8192)()
    rotated = c.c_int()
    video_settings = VideoSettings()
    video_settings.layers = 7
    for shade in range(16):
        video_settings.gray[shade] = 0xff000000 | (shade * 0x111111)
    for color in range(4096):
        red, green, blue = color >> 8, (color >> 4) & 15, color & 15
        video_settings.rgb[color] = 0xff000000 | (red * 17 << 16) | (green * 17 << 8) | blue * 17
    with ExitStack() as stack:
        stack.callback(lib.bizswan_delete, core)
        trace = stack.enter_context(args.trace.open("w")) if args.trace else None
        replay = stack.enter_context(args.input_out.open("w")) if args.input_out else None
        if trace:
            print("frame,buttons,lag,rotate,cs,ip,video_sha256,video_crc32", file=trace)
        for frame, (settings, power, buttons) in enumerate(read_movie(args.movie, rom), 1):
            if args.frames is not None and frame > args.frames:
                break
            if frame == 1:
                initial = datetime.fromisoformat(settings["InitialTime"]).replace(tzinfo=timezone.utc)
                birth = datetime.fromisoformat(settings["BirthDate"])
                config = SyncSettings(int(initial.timestamp()), birth.year, birth.month, birth.day,
                                      settings["Color"], settings["UseRealTime"], settings["Language"],
                                      settings["Gender"], settings["BloodType"], settings["Name"].encode("ascii")[:16])
                if not lib.bizswan_load(core, rom, len(rom), c.byref(config), c.byref(rotated)):
                    raise ValueError("BizHawk refused the ROM")
                lib.bizswan_putsettings(core, c.byref(video_settings))
            if power:
                if replay:
                    raise ValueError("The ws_host input format cannot replay a power reset")
                lib.bizswan_reset(core)
            samples = c.c_int(len(audio) // 2)
            lag = lib.bizswan_advance(core, buttons, False, screen, audio,
                                      c.byref(samples), c.byref(rotated))
            if trace:
                cs = lib.bizswan_getnecreg(core, 12)
                ip = lib.bizswan_getnecreg(core, 1)
                rgb = rgb_bytes(screen)
                video = hashlib.sha256(rgb).hexdigest()
                crc = zlib.crc32(unrotate(rgb) if rotated.value else rgb)
                print(f"{frame},{buttons:08x},{lag},{rotated.value},{cs:04x},{ip:04x},{video},{crc:08x}",
                      file=trace)
            if replay:
                pad = (buttons >> (16 if rotated.value else 0)) & 0x7ff
                keys = ((pad & 0xff) << 4) | ((pad >> 7) & 0x0e)
                print(f"{keys:03x}", file=replay)
        if args.capture:
            width, height = (144, 224) if rotated.value else (224, 144)
            args.capture.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb_bytes(screen))


if __name__ == "__main__":
    main()
