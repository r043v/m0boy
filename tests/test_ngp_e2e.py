#!/usr/bin/env python3
"""Neo Geo Pocket end to end: the synthetic cartridge (tests/ngp_fixture.py)
boots the interpreter, the V-INT handler counts frames into the picture, the
run hashes deterministically, states round trip, and the generated desktop
project's host prints the same run_hash.  The PC window (SDL2) is built and
run head first when SDL2 is present."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ngp_fixture import fixture  # noqa: E402


def run(*args):
    result = subprocess.run([str(a) for a in args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def summary_fields(out):
    """frames, vint, run_hash from the harness's summary line"""
    m = re.search(r'frames=(\d+) executed=(\d+) \S+ \S+ \S+ irq=\d+ vint=(\d+) .*run_hash=([0-9a-f]+)', out)
    assert m, out
    return int(m.group(1)), int(m.group(3)), m.group(4)


def ppm_pixels(path):
    data = Path(path).read_bytes()
    header, _, body = data.partition(b'\n255\n')
    assert header.startswith(b'P6'), header
    return {body[i:i + 3] for i in range(0, len(body), 3)}


def build_host(project, build_dir):
    run('cmake', '-G', 'Ninja', '-S', project / 'host', '-B', build_dir)
    run('cmake', '--build', build_dir)
    exes = [p for p in build_dir.iterdir() if p.is_file() and os.access(p, os.X_OK)]
    assert len(exes) == 1, exes
    return exes[0]


def exercise(host, rom, root, tag, expect_colour=False):
    """the interpreter's own contract: determinism, the interrupt count, an
    evolving picture, and the state round trip"""
    shot_a, shot_b = root / f'{tag}_a.ppm', root / f'{tag}_b.ppm'
    out_a = run(host, rom, '--frames', 60, '--ppm', shot_a)
    frames, vint, hash_a = summary_fields(out_a)
    assert frames == 60 and vint == 60, out_a  # the handler counts every frame
    out_b = run(host, rom, '--frames', 60, '--ppm', shot_b)
    _, _, hash_b = summary_fields(out_b)
    assert hash_a == hash_b, (out_a, out_b)     # deterministic
    # the handler repaints the map every frame: frame 10 differs from 60
    early = root / f'{tag}_early.ppm'
    run(host, rom, '--frames', 10, '--ppm', early)
    assert early.read_bytes() != shot_a.read_bytes(), f'the picture never moves ({tag})'
    pixels = ppm_pixels(shot_a)
    assert len(pixels) > 1, 'blank screen'
    if expect_colour:
        # the colour cart's palettes: green (00 xx 00-ish) and blue present
        assert any(p[1] > p[0] + 100 and p[1] > p[2] + 100 for p in pixels), 'no green'
        assert any(p[2] > p[0] + 100 and p[2] > p[1] + 100 for p in pixels), 'no blue'
    # the state round trip: 60 frames saved, then 30 more, equals 90 straight
    state = root / f'{tag}.state'
    run(host, rom, '--frames', 60, '--save-state', state)
    out_load = run(host, rom, '--frames', 30, '--load-state', state)
    out_straight = run(host, rom, '--frames', 90)
    _, _, hash_load = summary_fields(out_load)
    _, _, hash_straight = summary_fields(out_straight)
    assert hash_load == hash_straight, (out_load, out_straight)
    return hash_a


def main():
    cli = sys.argv[1]
    root = Path(sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix='ngp_'))
    root.mkdir(exist_ok=True)

    mono = root / 'synthetic.ngp'
    mono.write_bytes(fixture(mode=0x00))
    colour = root / 'synthetic.ngc'
    colour.write_bytes(fixture(mode=0x10))

    # the checkout harness first (the reference every project must match)
    harness = Path(__file__).resolve().parent.parent / 'build' / 'bin' / 'ngp_host'
    interp_hash = exercise(harness, mono, root, 'interp')
    exercise(harness, colour, root, 'interp_c', expect_colour=True)

    # the generated desktop project: the same run_hash
    project = root / 'project'
    report = run(cli, '--system', 'ngp', mono, '--target', 'desktop',
                 '--output-prefix', 'synth', '-o', project)
    assert 'Generated Neo Geo Pocket project' in report, report
    host = build_host(project, root / 'build')
    generated_hash = exercise(host, mono, root, 'gen')
    assert generated_hash == interp_hash, (generated_hash, interp_hash)

    # the colour cart as a generated project: palettes through the K2GE
    cproject = root / 'project-c'
    run(cli, '--system', 'ngp', colour, '--target', 'desktop',
        '--output-prefix', 'synthc', '-o', cproject)
    chost = build_host(cproject, root / 'build-c')
    exercise(chost, colour, root, 'gen_c', expect_colour=True)

    # the flash saves: the host writes <program>.ngpflash next to itself and
    # the file round-trips (--ram-out then --ram-in, byte for byte)
    for out in (root / 'gen_a.ngpflash',):
        out.unlink(missing_ok=True)
    run(host, mono, '--frames', 10, '--ram-out', root / 'f1.ngpflash')
    run(host, mono, '--frames', 10, '--ram-in', root / 'f1.ngpflash',
        '--ram-out', root / 'f2.ngpflash')
    assert (root / 'f1.ngpflash').read_bytes() == (root / 'f2.ngpflash').read_bytes(), 'flash round trip'
    # the default save: <host>.ngpflash in the working directory, not "(null)"
    run(host, '--frames', 5)
    assert (Path.cwd() / (host.name + '.ngpflash')).exists() or \
        (host.parent / (host.name + '.ngpflash')).exists(), 'default flash file'
    # the cartridge loaded at run time (the project carries no ROM bytes)
    ext_project = root / 'project-ext'
    run(cli, '--system', 'ngp', mono, '--target', 'desktop', '--rom-external',
        '--output-prefix', 'synthx', '-o', ext_project)
    assert 'M0_ROM_EXTERNAL' in (ext_project / 'host' / 'CMakeLists.txt').read_text()
    assert 'const uint8_t ngp_rom_data[]' not in (ext_project / 'src' / 'ngp_cart.c').read_text(), \
        'the rom-external project still embeds the cartridge'
    ext_host = build_host(ext_project, root / 'build-ext')
    res = subprocess.run([str(ext_host), '--frames', '5'], capture_output=True, text=True)
    assert res.returncode != 0 and 'ROM not found' in res.stderr, res  # refuses without the cartridge
    shutil.copy(mono, ext_host.parent / 'synthetic.ngp')
    ext_out = run(ext_host, '--frames', 60)
    _, _, ext_hash = summary_fields(ext_out)
    assert ext_hash == interp_hash, (ext_hash, interp_hash)

    # the PC window, head first, when SDL2 is present
    have_sdl2 = shutil.which('pkg-config') is None or \
        subprocess.run(['pkg-config', '--exists', 'sdl2']).returncode == 0
    if have_sdl2:
        pc_project = root / 'pc-project'
        run(cli, '--system', 'ngp', mono, '--target', 'pc',
            '--output-prefix', 'synth', '-o', pc_project)
        assert 'pkg_check_modules(SDL2' in (pc_project / 'CMakeLists.txt').read_text()
        run('cmake', '-G', 'Ninja', '-S', pc_project, '-B', root / 'pc-build')
        run('cmake', '--build', root / 'pc-build')
        window = next(p for p in (root / 'pc-build').iterdir()
                      if p.is_file() and os.access(p, os.X_OK))
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        result = subprocess.run([str(window), '--frames', '60', '--ppm', str(root / 'pc.ppm')],
                                env=env, capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, (result.stdout, result.stderr)
        m = re.search(r'run_hash=([0-9a-f]+)', result.stdout)
        assert m and m.group(1) == interp_hash, (result.stdout, interp_hash)
        assert ppm_pixels(root / 'pc.ppm') == ppm_pixels(root / 'interp_a.ppm')

    print(f'ngp e2e ok: interp={interp_hash} generated={generated_hash}'
          + (' pc-window ok' if have_sdl2 else ' (SDL2 absent: window skipped)'))


if __name__ == '__main__':
    main()
