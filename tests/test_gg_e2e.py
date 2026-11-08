"""Synthetic Game Gear/Master System cartridge (tests/gg_fixture.py): banked compiled code,
interpreter fallback and real M0 firmware.

No ROM or ROM-derived fixture is kept in the repository.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gg_fixture import fixture  # noqa: E402


def run(*args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def fingerprint(output):
    return re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+) page=([0-9,]+).*run_hash=([0-9a-f]+)', output).groups()


INPUT = '20:64:5,30:16:10,40:8:20'  # START, button 1, right


def exercise(args, root, system):
    root.mkdir()
    rom = root / ('synthetic.' + system)
    rom.write_bytes(fixture())
    coverage = root / 'synthetic.cov'
    common = ('--frames', 60, '--input', INPUT if system == 'gg' else '20:128:5,30:16:10,40:8:20')
    baseline = run(args.host, rom, *common, '--coverage', coverage,
                   '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav')
    text = coverage.read_text()
    assert text.startswith('# m0boy.' + system + '.coverage.v2\n') and '# read ' in text
    assert re.search(r'^02[0-9a-f]{4} \d+$', text, re.M) and re.search(r'^03[0-9a-f]{4} \d+$', text, re.M)  # pages 2 and 3
    # The retired explorer (deepeel explores): a long seeded play, whose
    # profile records the frames played
    explored = root / 'explored.cov'
    explore_args = (args.host, rom, '--frames', 1500, '--random', 3, '--coverage', explored)
    explore_result = run(*explore_args)
    text = explored.read_text()
    assert run(*explore_args) == explore_result
    assert explored.read_text() == text
    # Adjacent seeds used to alias because the CLI forced every seed odd.
    random2 = run(args.host, rom, '--frames', 64, '--random', 2)
    random3 = run(args.host, rom, '--frames', 64, '--random', 3)
    assert fingerprint(random2) != fingerprint(random3)
    played = re.search(r'^# frames (\d+)$', text, re.M)  # the whole seeded play
    assert played and int(played.group(1)) == 1500 and '# read ' in text
    assert re.search(r'^[0-9a-f]{6} \d+$', text, re.M)
    for target in ('desktop', 'meta', 'pokitto'):
        # Precise build: the recompiled code reproduces the interpreter,
        # mapper included.
        output = root / target
        run(args.recompiler, rom, '--target', target, '--coverage', coverage,
            '--meta-precise', '--output-prefix', 'synthetic', '-o', output)
        host = output / 'host/build'
        run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
        run('ninja', '-C', host)
        exe = host / 'synthetic_host'
        native = run(exe, *common, '--shot', root / 'native.ppm', '--wav', root / 'native.wav')
        interpreted = run(exe, *common, '--interp', '--wav', root / 'interp.wav')
        assert fingerprint(native) == fingerprint(interpreted) == fingerprint(baseline)
        assert (root / 'native.wav').read_bytes() == (root / 'interp.wav').read_bytes() == (root / 'reference.wav').read_bytes()
        assert len(set((root / 'native.wav').read_bytes()[44:])) > 1
        assert (root / 'native.ppm').read_bytes() == (root / 'reference.ppm').read_bytes()
        assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
        source = (output / 'src/sg_gen.c').read_text()
        assert 'machine.page[2] ==' in source  # guarded call into slot 2
        # An almost-empty native budget: fallback keeps every state.
        run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
            '--meta-code-budget', 160, '--output-prefix', 'synthetic', '-o', output)
        run('ninja', '-C', host)
        mixed = run(exe, *common)
        assert fingerprint(mixed) == fingerprint(baseline)
        assert int(re.search(r'interpreted=(\d+)', mixed)[1]) > 0
        # Default build: syncs at loop heads, flags only where read.
        fast_output = root / (target + '-fast')
        run(args.recompiler, rom, '--target', target, '--coverage', coverage,
            '--output-prefix', 'synthetic', '-o', fast_output)
        fast_host = fast_output / 'host/build'
        run('cmake', '-G', 'Ninja', '-S', fast_output / 'host', '-B', fast_host)
        run('ninja', '-C', fast_host)
        fast = run(fast_host / 'synthetic_host', *common, '--wav', root / 'fast.wav')
        assert re.search(r'frames=60 ', fast) and int(re.search(r'compiled=(\d+)', fast)[1]) > 0
        if target == 'pokitto' and shutil.which('arm-none-eabi-gcc'):
            build = fast_output / 'build'
            run('cmake', '-G', 'Ninja', '-S', fast_output, '-B', build,
                '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake')
            run('ninja', '-C', build)
            binary = (build / 'firmware.bin').read_bytes()
            assert len(binary) < 0x39000
            assert sum(struct.unpack_from('<8I', binary)) & 0xffffffff == 0
        print(system + '/' + target + ': precise native = interpreter (hashes, pixels, audio, fallback); default build runs')
    invalid = root / 'wrong.cov'
    invalid.write_text('# m0boy.sg1000.coverage.v1\n0000\n')
    failed = subprocess.run([args.recompiler, str(rom), '--coverage', str(invalid),
                             '-o', str(root / 'invalid')], capture_output=True)
    assert failed.returncode != 0 and not (root / 'invalid').exists()


STUB = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The SD card of a streamed project, here the cartridge file */
int gg_platform_rom_read(uint32_t sector, uint8_t *dst) {
    static FILE *file;
    if (!file) { file = fopen(ROM_FILE, "rb"); FILE *mark = fopen(ROM_FILE ".read", "wb"); if (mark) fclose(mark); }
    if (!file || fseek(file, (long)sector * 512, SEEK_SET) || fread(dst, 1, 512, file) != 512) { memset(dst, 0xff, 512); return 0; }
    return 1;
}
"""


def exercise_pool(args, root, packed=False):
    """A cartridge larger than the flash: the sectors the profile reads most stay in
    flash (the pool), the others come from the card; same run as the interpreter."""
    root.mkdir()
    rom = root / 'big.gg'
    data = fixture()
    if packed:
        data = data.ljust(20 * 0x4000, b'\xa5')
    else:
        import random
        data += random.Random(12345).randbytes(20 * 0x4000 - len(data))
    rom.write_bytes(data)  # 320 KiB, more than the flash holds raw
    coverage = root / 'big.cov'
    common = ('--frames', 60, '--input', INPUT)
    baseline = run(args.host, rom, *common, '--coverage', coverage)
    text = coverage.read_text()
    assert '# frames 60\n' in text and re.search(r'^02[0-9a-f]{4} \d+$', text, re.M)  # executions are counted
    reads = [l for l in text.splitlines() if l.startswith('# read ')]
    assert reads
    # Page 3's sectors are not in the profile (they stay on the card); the others are hot.
    kept = [l for l in text.splitlines() if not (l.startswith('# read ') and 0x60 <= int(l.split()[2], 16) < 0x80)]
    if not packed:
        # Fill the flash choice with additional cold-sector candidates so
        # both raw and compressed flash paths survive the raw promotion pass.
        kept += ['# read %04x 1' % s for s in range(128, 640)]
        kept += ['%06x 1' % (4 << 16 | pc) for pc in range(0x4000, 0x5000)]
    coverage.write_text('\n'.join(kept) + '\n')
    for target in ('pokitto', 'meta'):
        output = root / target
        log = run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
                  '--output-prefix', 'big', '-o', output)
        if packed:
            assert '(whole ROM in flash)' in log and 'ROM streaming:' not in log, log
        else:
            assert re.search(r'ROM streaming: +\d+ pages of 16 KiB read from the SD card, \d+ hot sectors', log), log
        config = (output / 'src/sg_config.h').read_text()
        pool = int(re.search(r'#define GG_POOL_SECTORS (\d+)', config)[1])
        assert 0 < pool <= (640 if packed else 254)
        assert ('#define GG_ROM_PACKED 1' in config) == packed
        data = (output / 'src/sg_rom.c').read_text()
        assert 'gg_pool_index[640]' in data and '#define GG_POOL_PACKED 1' in config
        records = [int(n) for n in re.findall(r'(\d+)u', re.search(r'gg_pool_records\[\d+\] = \{(.*?)\};', data, re.S)[1])]
        assert len(records) == pool + 1 and any(r >> 30 == 1 for r in records[:-1])
        assert all((a & 0x3fffffff) < (b & 0x3fffffff) for a, b in zip(records, records[1:]))
        assert (records[-1] & 0x3fffffff) < pool * 512  # genuinely packed, not just a flag
        src = output / 'src'
        (root / (target + '_stub.c')).write_text('#define ROM_FILE "%s"\n' % rom + STUB)
        exe = root / (target + '_host')
        run(shutil.which('cc') or 'cc', '-O1', '-DSG_GENERATED=1', '-I', src, '-o', exe, output / 'host/gg_host.c',
            *(src / n for n in ('z80.c', 'gg.c', 'gg_psg.c', 'gg_vdp.c', 'sg_gen.c', 'sg_rom.c')),
            root / (target + '_stub.c'), '-lm')
        native = run(exe, *common)
        interpreted = run(exe, *common, '--interp')
        assert fingerprint(native) == fingerprint(interpreted) == fingerprint(baseline)
        assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
        assert (root / 'big.gg.read').exists() != packed
        if packed and target == 'pokitto' and shutil.which('arm-none-eabi-gcc'):
            build = output / 'build'
            run('cmake', '-G', 'Ninja', '-S', output, '-B', build,
                '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake')
            run('ninja', '-C', build)
            assert (build / 'firmware.bin').stat().st_size < 0x39000
        print('gg/%s: %d-sector pool and the card give the interpreter\'s run' % (target, pool))


def exercise_cart_storage(args, root):
    """Card-backed cartridge RAM participates in the same logical run hash."""
    root.mkdir()
    rom = root / 'cart.gg'
    data = bytearray(fixture())
    # Replace initial DI; IM 1 with a bootstrap in unused bank-zero space.
    assert data[:3] == bytes.fromhex('f3 ed 56')
    data[:3] = bytes.fromhex('c3 00 3e')
    bootstrap = bytes.fromhex('f3 3e 08 32 fc ff 3e 5a 32 00 80 af 32 fc ff ed 56 c3 03 00')
    data[0x3e00:0x3e00 + len(bootstrap)] = bootstrap
    rom.write_bytes(data)
    coverage = root / 'cart.cov'
    common = ('--frames', 60)
    reference = fingerprint(run(args.host, rom, *common, '--coverage', coverage))
    assert '# cart-ram' in coverage.read_text()
    for target in ('meta', 'pokitto'):
        project = root / target
        run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
            '--output-prefix', 'cart', '-o', project)
        assert '#define GG_CART_SD 1' in (project / 'src/sg_config.h').read_text()
        build = project / 'host/build'
        run('cmake', '-G', 'Ninja', '-S', project / 'host', '-B', build)
        run('ninja', '-C', build)
        for flags in ([], ['--interp']):
            assert fingerprint(run(build / 'cart_host', *common, *flags)) == reference


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_gg_') as temporary:
        root = Path(temporary)
        for system in ('gg', 'sms'):
            exercise(args, root / system, system)
        exercise_pool(args, root / 'pool')
        exercise_pool(args, root / 'packed', packed=True)
        exercise_cart_storage(args, root / 'cart')


if __name__ == '__main__':
    main()
