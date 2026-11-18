#!/usr/bin/env python3
"""m0boy ROM [TARGET]: one ROM in, a complete release out.

  m0boy game.gg            -> PC release (the machine this runs on)
  m0boy game.gb pokitto    -> Pokitto project (firmware when the toolchain exists)

Steps, all automatic:
  1. the source system comes from the file (gbrecomp --info);
  2. the target defaults to `pc` (SDL2 window, native to this host);
  3. deepeel's coverage is looked up beside the ROM (<stem>.deepeel/, or
     <stem>.cov/.map.png/.rtcode); when absent, deepeel is run on the ROM
     (it resumes an earlier analysis by itself) and writes there;
  4. gbrecomp generates the project (coverage fed where the system reads it);
  5. the project is built for the target;
  6. <out>/<name>-<target>-release.tar.gz (binary or firmware, ROM-free of
     other files than the project needs, README) and
     <out>/<name>-<target>-source.tar.gz (the generated C project, the sources
     the release was built from) are written.

Only the ROM is required.  --out DIR, --no-deepeel and --jobs N are optional.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXT = {'.gb': 'gameboy', '.gbc': 'gameboy', '.sgb': 'gameboy', '.sg': 'sg1000', '.sc': 'sg1000',
       '.gg': 'gg', '.sms': 'sms', '.mx1': 'msx', '.mx2': 'msx', '.rom': 'msx', '.nes': 'nes',
       '.pce': 'pce', '.vec': 'vectrex', '.md': 'megadrive', '.gen': 'megadrive', '.smd': 'megadrive',
       '.bin': None, '.sfc': 'snes', '.smc': 'snes', '.p8': 'pico8', '.png': 'pico8'}
# deepeel's coverage is read by the Game Boy (rtcode) and the Z80 family (profile + map)
Z80 = {'sg1000': 'sg1000', 'gg': 'gg', 'sms': 'sms', 'msx': 'msx'}
DEEPEEL_SYS = {'gameboy', 'nes', 'pce', 'megadrive', 'sg1000', 'gg', 'sms', 'msx'}


def say(msg):
    print('m0boy: ' + msg, file=sys.stderr, flush=True)


def run(cmd, **kw):
    say(' '.join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        sys.exit(f'm0boy: failed ({r.returncode}): {cmd[0]}')
    return r


def find_cli():
    env = os.environ.get('M0BOY_CLI')
    if env:
        return Path(env)
    build = ROOT / 'build'
    if not (build / 'build.ninja').is_file():
        run(['cmake', '-G', 'Ninja', '-B', build, ROOT])
    run(['ninja', '-C', build, 'gbrecomp'], stdout=subprocess.DEVNULL)
    return build / 'bin' / 'gbrecomp'


def find_deepeel():
    env = os.environ.get('DEEPEEL')
    cands = [Path(env)] if env else []
    w = shutil.which('deepeel')
    if w:
        cands.append(Path(w))
    cands += [ROOT.parent / 'memscan' / 'build' / 'deepeel', ROOT.parent / 'deepeel' / 'build' / 'deepeel']
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def detect_system(cli, rom):
    ext = EXT.get(rom.suffix.lower())
    out = subprocess.run([str(cli), str(rom), '--info'] + (['--system', ext] if ext else []),
                         capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if line.startswith('system='):
            return line.split('=', 1)[1].strip()
    sys.exit(f'm0boy: cannot tell which console {rom.name} is for\n{out.stdout}{out.stderr}')


def physical_map(src, dst):
    """deepeel's text map (CPU-view addresses) -> the recompiler's physical-bank offsets (16 KiB, 0-3FFF)."""
    lines = ['# physical-bank offset-start offset-end class']
    for line in src.read_text().splitlines():
        p = line.split()
        if len(p) < 4 or line.startswith('#'):
            continue
        bank, lo, hi, klass = int(p[0], 16), int(p[1], 16), int(p[2], 16), p[3]
        if not klass.startswith('code-'):
            continue
        while lo <= hi:
            end = min(hi, (lo | 0x3fff))
            lines.append(f'{bank:x} {lo & 0x3fff:x} {end & 0x3fff:x} {klass}')
            lo = end + 1
    dst.write_text('\n'.join(lines) + '\n')


def host_target(cli):
    return 'pc'


def locate_coverage(rom, force_dir):
    """(cov, map.png, rtcode) next to the ROM, each a Path or None."""
    stem = rom.stem
    dirs = [force_dir] if force_dir else []
    dirs += [rom.parent / (stem + '.deepeel'), rom.parent]
    for d in dirs:
        cov = d / (stem + '.cov')
        if cov.is_file():
            png = d / (stem + '.map.png')
            rt = d / (stem + '.rtcode')
            return d, cov, png if png.is_file() else None, rt if rt.is_file() else None
    return None, None, None, None


def ensure_coverage(rom, system, deepeel, jobs):
    d, cov, png, rt = locate_coverage(rom, None)
    if cov:
        say(f'coverage found: {cov}')
        return d, cov, png, rt
    if system not in DEEPEEL_SYS:
        return None, None, None, None
    if not deepeel:
        say('no coverage beside the ROM and no deepeel binary found (DEEPEEL=/path): static walk only')
        return None, None, None, None
    out = rom.parent / (rom.stem + '.deepeel')
    say(f'no coverage beside the ROM: running deepeel into {out}/')
    cmd = [deepeel, rom, '--out', out]
    if jobs:
        cmd += ['--jobs', str(jobs)]
    run(cmd)
    return locate_coverage(rom, out)


def cmake_build(project, jobs):
    b = project / 'build'
    run(['cmake', '-S', project, '-B', b, '-DCMAKE_BUILD_TYPE=Release'], stdout=subprocess.DEVNULL)
    run(['cmake', '--build', b, '-j', str(jobs)], stdout=subprocess.DEVNULL)
    exes = [p for p in b.iterdir() if p.is_file() and os.access(p, os.X_OK) and p.suffix in ('', '.exe')]
    return exes


def tar(path, entries, base=None):
    with tarfile.open(path, 'w:gz') as t:
        for src, arc in entries:
            t.add(src, arcname=arc, filter=lambda ti: ti if '/build/' not in '/' + ti.name + '/' and '/.pio/' not in ti.name else None)
    say(f'wrote {path} ({path.stat().st_size // 1024} KiB)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('target', nargs='?', help='destination console (default: pc, this machine)')
    ap.add_argument('--out', help='output directory (default: <rom stem>.m0boy beside the ROM)')
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--no-deepeel', action='store_true', help='never run deepeel (use what is beside the ROM, else static walk)')
    ap.add_argument('--system', help='force the source console')
    a, extra = ap.parse_known_args()
    rom = Path(a.rom).resolve()
    if not rom.is_file():
        sys.exit(f'm0boy: {rom}: no such file')
    jobs = a.jobs or max(1, (os.cpu_count() or 2) - 1)
    cli = find_cli()
    system = a.system or detect_system(cli, rom)
    target = a.target or host_target(cli)
    name = rom.stem
    out = Path(a.out).resolve() if a.out else rom.parent / (name + '.m0boy')
    out.mkdir(parents=True, exist_ok=True)
    say(f'{rom.name}: {system} -> {target}')

    deepeel = None if a.no_deepeel else find_deepeel()
    if a.no_deepeel:
        d, cov, png, rt = locate_coverage(rom, None)
    else:
        d, cov, png, rt = ensure_coverage(rom, system, deepeel, a.jobs)

    project = out / 'project'
    if project.exists():
        shutil.rmtree(project)
    gen = [cli, rom, '--system', system, '--target', target, '--output-prefix', name.lower()[:24].replace(' ', '_') or 'game',
           '-o', project, '-j', str(jobs)]
    if system in Z80 and cov:
        profile = out / (name + '.m0boy.cov')
        bridge = [sys.executable, ROOT / 'tools' / 'deepeel2m0boy.py', Z80[system], cov, '--out', profile,
                  '--rom', rom]
        run(bridge)
        gen += ['--coverage', profile]
        if png and deepeel and system != 'msx':
            raw, mp = out / (name + '.deepeel.map'), out / (name + '.map')
            run([deepeel, 'mapx', png, raw])
            physical_map(raw, mp)
            gen += ['--coverage-map', mp]
    elif system == 'gameboy':
        gen += ['--meta-static']
        if rt:
            gen += ['--rtcode', rt]
    elif target != 'desktop':
        gen += ['--meta-static'] if system in ('gameboy', 'nes', 'pce') else []
    run(gen + extra)

    rel = out / f'{name}-{target}-release'
    if rel.exists():
        shutil.rmtree(rel)
    rel.mkdir()
    built = []
    if target == 'pc':
        if (project / 'CMakeLists.txt').is_file():
            built = cmake_build(project, jobs)
        for e in built:
            shutil.copy2(e, rel / e.name)
        for f in ('sd.img',):
            if (project / f).is_file():
                shutil.copy2(project / f, rel / f)
        sdl = shutil.which('sdl2-config')
    else:
        # console firmwares: the project is built by its own toolchain (PlatformIO / arm-none-eabi); ship what exists
        for pat in ('**/firmware.bin', '**/*.bin', '**/firmware.elf'):
            for f in project.glob(pat):
                if '/src/' not in str(f):
                    shutil.copy2(f, rel / f.name)
    cov_note = f'coverage: {cov.name}' if cov else 'coverage: none (static walk)'
    (rel / 'README.txt').write_text(
        f'{name} - {system} recompiled for {target} by m0boy\n'
        f'host: {platform.system()} {platform.machine()}\n{cov_note}\n'
        'Controls and options: the in-game menu; states and saves go beside the executable.\n'
        'Counts printed by the tools are observations of this build, not hardware measurements.\n')
    if not any(p.name != 'README.txt' for p in rel.iterdir()):
        say(f'warning: no binary or firmware was produced for {target}; the project is in the source archive')
    tar(out / f'{name}-{target}-release.tar.gz', [(rel, f'{name}-{target}-release')])
    src = out / f'{name}-{target}-source.tar.gz'
    entries = [(project, f'{name}-{target}-source/project')]
    if cov:
        entries.append((cov, f'{name}-{target}-source/coverage/{cov.name}'))
    tar(src, entries)
    say(f'done: {out}')


if __name__ == '__main__':
    main()
