#!/usr/bin/env python3
"""ESPboy project ownership, flash layout and precise generated/interpreter parity.
Optional real Xtensa build: ESPBOY_PIO=/path/to/pio (SDK already installed).
Uses only the synthetic cartridge shared with the META end-to-end test.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile
from test_meta_target_e2e import make_rom, build_host, run, parse


def gm0_check(project, rom, frames=240):
    """Optional (ESPBOY_GM0=/path/to/gm0): the real firmware, Xtensa code and
    all, in gm0's ESPboy machine: it must neither stop nor leave the screen
    blank.  Timing in gm0 is estimated, nothing here measures the chip."""
    gm0 = os.environ.get('ESPBOY_GM0')
    if not gm0:
        return
    shot = project / 'gm0.ppm'
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', EMU_NOPACE='1')
    firmware = project / '.pio/build/espboy/firmware.bin'
    cmd = [gm0, str(firmware), '--frames', str(frames), '--shot', str(shot)]
    if rom:
        cmd += ['--rom', str(rom)]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    assert result.returncode == 0 and 'ARRÊT' not in result.stderr, result.stderr[-800:]
    pixels = shot.read_bytes().split(b'\n255\n', 1)[1]
    assert len(set(pixels[i:i + 3] for i in range(0, len(pixels), 3))) > 1, 'gm0: blank screen'
    print('ESPboy in gm0: runs, the screen is drawn')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--m0recomp', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='espboy_') as tmp:
        root = Path(tmp)
        rom = root / 'synthetic.gb'
        rom.write_bytes(make_rom(1989))
        project = root / 'project'
        base = [args.m0recomp, str(rom), '--target', 'espboy', '--output-prefix', 'e2e',
                '--meta-static', '--meta-precise', '-o', str(project)]
        run(base)
        assert not (project / 'rom.bin').exists()  # a small ROM is part of the firmware
        run(base + ['--meta-pack'])
        assert not (project / 'rom.bin').exists()
        assert 'GBL_ROM_PACKED' not in (project / 'src/gbl_config.h').read_text()
        assert not list((project / 'src').glob('pokitto_*'))
        assert not (project / 'src/meta_hw.h').exists()
        ini = (project / 'platformio.ini').read_text()
        assert 'flash_size = 4MB' in ini and 'flash_mode = qio' in ini
        assert not list((project / 'src').glob('espboy_startup*'))
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_TARGET_ESPBOY 1' in config
        assert 'GBL_ROM_SD' not in config and 'GBL_STATE_NAME' not in config
        assert 'GBL_FLASH_GAME_ID' in config and '#define GBL_STATE_SLOTS 7' in config, config
        build = build_host(project)
        results = []
        for interp in [[], ['--interp']]:
            result = run([str(build / 'e2e_host'), '--frames', '180', *interp])
            results.append(parse(result)['run_hash'])
        assert results[0] == results[1], results
        print('ESPboy precise compiled/interpreter:', results[0])
        # espboy_flash.cpp (battery RAM copies, power cuts, states) on a mock flash.
        tests = Path(__file__).resolve().parent
        obj = root / 'flashcheck'
        obj.mkdir()
        src = project / 'src'
        objs = []
        for c in sorted(c for c in src.glob('gbl_*.c') if c.name != 'gbl_display.c') + [src / 'e2e_gen.c', src / 'e2e_rom.c']:
            run(['cc', '-O1', '-w', '-I', str(src), '-c', str(c), '-o', str(obj / (c.name + '.o'))])
            objs.append(str(obj / (c.name + '.o')))
        for c in [src / 'espboy_flash.cpp', tests / 'espboy_flash_check.cpp']:
            run(['c++', '-O1', '-w', '-DGBL_SAVE_IMAGE', '-I', str(tests / 'espboy_mock'), '-I', str(src),
                 '-c', str(c), '-o', str(obj / (c.name + '.o'))])
            objs.append(str(obj / (c.name + '.o')))
        run(['c++', *objs, '-o', str(obj / 'flashcheck')])
        print(run([str(obj / 'flashcheck')]).strip())
        pio = os.environ.get('ESPBOY_PIO')
        if pio:
            run([pio, 'run', '-d', str(project)])
            binary = project / '.pio/build/espboy/firmware.bin'
            assert binary.stat().st_size <= 0x100000
            print('ESPboy Xtensa firmware:', binary.stat().st_size, 'bytes')
            gm0_check(project, None)
        else:
            print('SKIP Xtensa firmware: set ESPBOY_PIO with an installed ESP8266 SDK')
        # A cartridge without battery RAM (nothing to save, states still offered).
        plain = bytearray(rom.read_bytes())
        plain[0x147] = 0x00
        plain[0x149] = 0
        plain[0x14d] = (-sum(plain[0x134:0x14d]) - 25) & 255
        rom.write_bytes(plain)
        plain_project = root / 'plain'
        run([args.m0recomp, str(rom), '--target', 'espboy', '--output-prefix', 'e2e', '--meta-static',
             '--meta-precise', '-o', str(plain_project)])
        assert 'GBL_BATTERY' not in (plain_project / 'src/gbl_config.h').read_text()
        if pio:
            run([pio, 'run', '-d', str(plain_project)])
            gm0_check(plain_project, None)
        rom.write_bytes(make_rom(1989))
        # The flash map in the runtime header and in the converter agree.
        header = (Path(__file__).resolve().parents[1] / 'targets/meta/runtime/gbl_espboy.h').read_text()
        emitter = (Path(__file__).resolve().parents[1] / 'recompiler/src/codegen/meta_emitter.cpp').read_text()
        def hdr(name): return int(re.search(r'#define %s (0x[0-9A-Fa-f]+)u' % name, header).group(1), 16)
        def emi(name): return int(re.search(r'constexpr size_t %s = (0x[0-9A-Fa-f]+);' % name, emitter).group(1), 16)
        assert hdr('ESPBOY_APP_BYTES') == emi('kEspApp') == hdr('ESPBOY_ROM_OFF')
        assert hdr('ESPBOY_EMBED_BYTES') == emi('kEspEmbed')
        assert hdr('ESPBOY_ROM_BYTES') == emi('kEspRomBytes')
        assert hdr('ESPBOY_ROM_OFF') + hdr('ESPBOY_ROM_BYTES') == hdr('ESPBOY_SAVE_OFF')
        assert hdr('ESPBOY_SAVE_OFF') + hdr('ESPBOY_SAVE_BYTES') == 0x3FB000
        assert hdr('ESPBOY_SAVE_BYTES') == int(re.search(r'kEspSaveBlocks = (0x[0-9A-Fa-f]+) / 4096', emitter).group(1), 16)
        assert '0x100000' in (Path(__file__).resolve().parents[1] / 'targets/consoles/espboy/espboy_build.py.in').read_text()
        ld = (Path(__file__).resolve().parents[1] / 'targets/consoles/espboy/espboy.ld.in').read_text()
        assert 'len = 0xfeff0' in ld  # ESPBOY_APP_BYTES - 0x1010
        # Above what the firmware holds: bank 0 in it, the other sectors raw in rom.bin.
        mid = bytearray(rom.read_bytes())
        mid.extend(bytes(512 * 1024 - len(mid)))
        mid[0x148] = 4
        mid[0x14d] = (-sum(mid[0x134:0x14d]) - 25) & 255
        rom.write_bytes(mid)
        run(base)
        assert 'GBL_ROM_PACKED 1' in (project / 'src/gbl_config.h').read_text()
        assert (project / 'rom.bin').stat().st_size == 512 * 1024 - 16384 + 2
        # A 4 MiB cartridge needs partial packing in the 3 MiB ROM area.
        large = bytearray(rom.read_bytes())
        large.extend(bytes(4 * 1024 * 1024 - len(large)))
        large[0x147] = 0x1b  # MBC5 + RAM + battery, every page selectable
        large[0x148] = 7
        large[0x14d] = (-sum(large[0x134:0x14d]) - 25) & 255
        rom.write_bytes(large)
        run(base)
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_ROM_PACKED 1' in config and 'GBL_ROM_SD 1' not in config
        assert (project / 'rom.bin').stat().st_size <= 0x2C0000
        # Read every logical cartridge byte through the actual runtime cache,
        # including raw sectors, LZSS back-references, and bank transitions.
        (project / 'host/rom_check.c').write_text(r'''#include "gbl.h"
#include <stdio.h>
int gbl_platform_line_check(uint8_t y, uint32_t s) { return 0; }
uint16_t *gbl_platform_line_begin(uint8_t y) { return 0; }
void gbl_platform_line_end(uint8_t y) {}
void gbl_platform_vblank(void) {}
extern const uint8_t gbl_rom_packed[];
/* What espboy_flash.cpp does with spi_flash_read: rom.bin = gbl_rom_packed. */
int espboy_rom_read(uint32_t off, uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = gbl_rom_packed[off + i];
    return 1;
}
int gbl_platform_save_read(uint32_t s, uint8_t *p) { return 0; }
int gbl_platform_save_write(uint32_t s, const uint8_t *p) { return 0; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    gbl_reset();
    for (unsigned bank = 0; bank < 256; bank++) {
        gbl_wr(0x2000, bank);
        gbl_wr(0x3000, bank >> 8);
        for (unsigned a = 0; a < 16384; a++) {
            int expected = fgetc(f);
            if (expected < 0 || gbl_rd(0x4000 + a) != expected) return 2;
        }
    }
    fclose(f);
    return 0;
}
''')
        with (project / 'host/CMakeLists.txt').open('a') as f:
            f.write('\nadd_executable(rom_check rom_check.c ${GBL_RUNTIME} ${GBL_GENERATED})\n'
                    'target_include_directories(rom_check PRIVATE ${GBL_SRC})\n'
                    'target_compile_definitions(rom_check PRIVATE GBL_ROM_PACKED_FLASH=1)\n')
        build = build_host(project)
        run([str(build / 'rom_check'), str(rom)])
        packed_native = parse(run([str(build / 'e2e_host'), '--frames', '180']))
        packed_interp = parse(run([str(build / 'e2e_host'), '--frames', '180', '--interp']))
        assert packed_native['run_hash'] == packed_interp['run_hash']
        assert packed_native['wram_hash'] == packed_interp['wram_hash']
        offsets = re.search(r'gbl_rom_packed_off\[\].*?= \{(.*?)\}',
                            (project / 'src/e2e_rom.c').read_text(), re.S).group(1)
        offsets = [int(n) for n in re.findall(r'(\d+)u', offsets)]
        packed = sum(0 < b - a < 512 for a, b in zip(offsets, offsets[1:]))
        raw = sum(b - a == 512 for a, b in zip(offsets, offsets[1:]))
        assert packed and raw, (packed, raw)
        print('ESPboy partial packing:', packed, 'compressed sectors,', raw, 'raw; all bytes verified')
        if pio:
            run([pio, 'run', '-d', str(project)])
            assert (project / '.pio/build/espboy/firmware.bin').stat().st_size <= 0x100000
            gm0_check(project, project / 'rom.bin', 480)
        import random
        noise = bytearray(random.Random(8086).randbytes(len(large)))
        noise[:512] = large[:512]
        rejected = root / 'incompressible.gb'
        rejected.write_bytes(noise)
        result = subprocess.run([args.m0recomp, str(rejected), '--target', 'espboy', '-o', str(root / 'rejected')], text=True, capture_output=True)
        assert result.returncode and 'even compressed' in result.stderr, result.stderr
        # Reject SD and architecture-specific paths rather than producing unusable projects.
        for flags in [['--meta-stream', 'on'], ['--meta-thumb']]:
            result = subprocess.run(base + flags, text=True, capture_output=True)
            assert result.returncode and 'ESPboy' in result.stderr
        # Switching consoles must not leave any ESPboy driver or linker script.
        run([args.m0recomp, str(rom), '--target', 'meta', '-o', str(project)])
        assert not list((project / 'src').glob('espboy_*'))
        assert not (project / 'espboy.ld').exists()
        assert not (project / 'rom.bin').exists()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
