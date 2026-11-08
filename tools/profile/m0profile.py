#!/usr/bin/env python3
"""Profile store of one ROM (profiles/<source>/<game>/, see compile.sh).

  m0profile.py syms <firmware.elf> <out.tsv>   function/object symbols of a firmware
  m0profile.py report <store> [--quiet]        writes <store>/profile.json
  m0profile.py top <store> <run.cyc> [n]       cycles of one gm0 run, per symbol
  m0profile.py session <store> <run.keys> <id> gm0 --record-input -> coverage-<id>.input
  m0profile.py calibrate <store> <run.cyc> <run.ram> [<run.cyc> <run.ram>...]
                                               costs of the card and the codec from gm0
                                               runs (calibration-<console>.json); two or
                                               more lengths of one session: regression
  m0profile.py storage <store> [--budget N] [--quick] [--target meta|pokitto|espboy] [--project DIR]
                                               ROM kept in flash: traces, simulated
                                               card cache, cross-validated plan
                                               (storage.json, rom.plan, rom.rank)

The store holds the recorded sessions (coverage-<id>.cov, coverage-<id>.input,
romdata-<id>.prof), rom.env (written by compile.sh), build-<target>.txt (last
build of each console), <target>.syms.tsv and gm0/<target>-<stamp>.cyc
(EMU_PROF captures, cycles per firmware address).  gm0 cycles are emulator
estimates, never device measurements.
"""
import bisect
import json
import os
import re
import shlex
import struct
import sys

BANK = 0x4000  # GB, SG-1000 (unbanked, sliced alike), GG and SMS pages
CHUNK = 256    # granularity of the ROM map


def read_env(store):
    env = {}
    try:
        with open(os.path.join(store, 'rom.env'), encoding='utf-8') as f:
            for line in f:
                key, sep, value = line.rstrip('\n').partition('=')
                if sep and key.isidentifier():
                    env[key] = ''.join(shlex.split(value)) if value else ''
    except OSError:
        pass
    return env


# ---- firmware symbols ------------------------------------------------------

def elf_symbols(path):
    """(address, size, name) of the FUNC and OBJECT symbols of an ELF32 LE."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError(f'{path}: not a little-endian ELF32 file')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        str_off = strtab[4]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_i, value, size, info, _other, shndx = struct.unpack_from('<IIIBBH', data, off)
            if (info & 15) not in (1, 2) or not shndx or not name_i:  # OBJECT, FUNC; defined
                continue
            end = data.index(b'\0', str_off + name_i)
            name = data[str_off + name_i:end].decode('utf-8', 'replace')
            addr = value & ~1 if (info & 15) == 2 else value  # Thumb bit
            out.append((addr, size, name))
    out.sort()
    return out


def cmd_syms(elf, out):
    symbols = elf_symbols(elf)
    tmp = out + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        for addr, size, name in symbols:
            f.write(f'{addr:08x}\t{size}\t{name}\n')
    os.replace(tmp, out)


def load_syms(path):
    symbols = []
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                parts = line.rstrip('\n').split('\t')
                if len(parts) == 3:
                    symbols.append((int(parts[0], 16), int(parts[1]), parts[2]))
    except OSError:
        return None
    symbols.sort()
    return symbols


def gm0_cycles(path, symbols):
    """Total cycles of an EMU_PROF capture and cycles per symbol."""
    total, per = 0, {}
    starts = [s[0] for s in symbols] if symbols else []
    with open(path, encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2:
                continue
            addr, cyc = int(parts[0], 16), int(parts[1])
            total += cyc
            name = None
            if symbols:
                i = bisect.bisect_right(starts, addr) - 1
                if i >= 0:
                    start, size, sym = symbols[i]
                    if addr < start + max(size, 2):
                        name = sym
            name = name or f'?{addr >> 12:05x}xxx'  # unnamed: grouped by 4 KiB block
            per[name] = per.get(name, 0) + cyc
    return total, per


def run_summary(path, symbols, top):
    total, per = gm0_cycles(path, symbols)
    ranked = sorted(per.items(), key=lambda kv: -kv[1])[:top]
    return {
        'file': os.path.relpath(path, os.path.dirname(os.path.dirname(path))),
        'total_cycles': total,
        'top': [{'symbol': n, 'cycles': c, 'percent': round(100.0 * c / total, 2) if total else 0.0}
                for n, c in ranked],
    }


# ---- sessions --------------------------------------------------------------

# Buttons the game receives, as gm0 reads them through the firmware's
# m0input descriptor (byte | extra byte << 8), named as the host harnesses'
# --input files spell them: bit i is NAMES[i].
NAMES = {
    'gameboy': ('a', 'b', 'select', 'start', 'right', 'left', 'up', 'down'),
    'sg1000': ('up', 'down', 'left', 'right', '1', '2'),
    'gg': ('up', 'down', 'left', 'right', '1', '2', 'start', 'pause'),
    'sms': ('up', 'down', 'left', 'right', '1', '2', 'start', 'pause'),
}


def button_names(system, state):
    names = NAMES[system]
    held = [n for i, n in enumerate(names) if state >> i & 1]
    if system == 'sg1000' and state >> 8 & 1:
        held.append('pause')
    return '+'.join(held) or 'none'


def cmd_session(store, keys_path, sid):
    """A gm0 recording -> coverage-<id>.input, the guest frames the game
    received each change at.  The session is cut where it can no longer be
    replayed from power-on: a restart, or the game's frame counter going
    back or leaping (a state loaded).  Prints the frames kept."""
    system = read_env(store).get('SYSTEM', 'gameboy')
    events, end, cut = [], None, None
    last_gm0 = last_guest = None
    with open(keys_path, encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if not parts or parts[0].startswith('#'):
                continue
            if parts[0] == 'r' and (events or end is not None):
                cut = 'restart'
                break
            if parts[0] not in ('g', 'e') or len(parts) < 3:
                continue
            gm0, guest = int(parts[1]), int(parts[2])
            if last_guest is not None and (guest < last_guest or guest - last_guest > 4 * (gm0 - last_gm0) + 8):
                cut = 'frame counter jumped (state loaded?)'
                break
            last_gm0, last_guest = gm0, guest
            if parts[0] == 'e':
                end = guest
            else:
                events.append((guest, int(parts[3], 16)))
    if not events:
        print('0')
        return 0
    end = max(end if end is not None else last_guest, events[-1][0])
    out = os.path.join(store, f'coverage-{sid}.input')
    with open(out + '.tmp', 'w', encoding='utf-8') as f:
        f.write(f'# gm0 session {os.path.basename(keys_path)}' + (f', cut: {cut}' if cut else '') + '\n')
        for guest, state in events:
            f.write(f'{guest} {button_names(system, state)}\n')
        f.write(f'{end} {button_names(system, events[-1][1])}\n')  # the session's length
    os.replace(out + '.tmp', out)
    print(end)
    return end


def session_kind(sid):
    for prefix, kind in (('gm0', 'gm0'), ('explore', 'explore'), ('rand', 'random'), ('idle', 'idle'),
                         ('demo', 'demo'), ('ai', 'ai'), ('auto', 'auto')):
        if sid.startswith(prefix):
            return kind
    return 'manual' if re.fullmatch(r'\d{8}-\d{6}', sid) else 'other'


def parse_cov(path, system):
    """{rom offset: executions}, data chunk reads {chunk: n}, frames or None."""
    code, reads, frames = {}, {}, None
    bank_shift = 14
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                parts = line.split()
                if len(parts) == 3 and parts[1] == 'frames':
                    frames = int(parts[2])
                elif len(parts) == 3 and parts[1] == 'bank-shift':
                    bank_shift = int(parts[2])
                elif len(parts) == 4 and parts[1] == 'read':  # GG/SMS: 512-byte sector, frames read
                    sector = int(parts[2], 16)
                    for c in (sector * 2, sector * 2 + 1):
                        reads[c] = reads.get(c, 0) + int(parts[3])
                continue
            parts = line.split()
            if not parts:
                continue
            if system == 'gameboy':
                if len(parts) < 3:
                    continue
                bank, addr = int(parts[0], 16), int(parts[1], 16)
                off = bank * BANK + (addr & 0x3fff)
                hits = int(parts[3], 16) if len(parts) > 3 else 1
            else:
                loc = int(parts[0], 16)
                pc = loc & 0xffff
                if pc >= 0xc000:  # Z80 RAM
                    continue
                off = ((loc >> 16) << bank_shift) + (pc & ((1 << bank_shift) - 1)) if system in ('gg', 'sms') else pc
                hits = int(parts[1]) if len(parts) > 1 else 1
            code[off] = code.get(off, 0) + hits
    return code, reads, frames


def parse_romdata(path):
    """GB romdata-<id>.prof: 256-byte page, reads."""
    reads = {}
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    reads[int(parts[0], 16)] = int(parts[1])
    except OSError:
        pass
    return reads


def input_frames(path):
    last = None
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                parts = line.split()
                if parts and parts[0].isdigit():
                    last = int(parts[0])
    except OSError:
        pass
    return last


def parse_build(path):
    info, log = {}, []
    with open(path, encoding='utf-8', errors='replace') as f:
        header = True
        for line in f:
            line = line.rstrip('\n')
            if header and line == '---':
                header = False
            elif header:
                key, _, value = line.partition('=')
                info[key] = value
            elif line.strip():
                log.append(line)
    for line in log:
        m = re.match(r'^(RAM|Flash|flash):.*?used (\d+) bytes from (\d+) bytes', line)
        if m:
            info[m.group(1).lower()] = {'used': int(m.group(2)), 'size': int(m.group(3))}
        m = re.search(r'instructions: *(\d+) compiled', line) or re.search(r'compiled: *(\d+)', line)
        if m and 'compiled_instructions' not in info:
            info['compiled_instructions'] = int(m.group(1))
    for key in ('bpi',):
        if info.get(key, '').isdigit():
            info[key] = int(info[key])
    info['log'] = log
    return info


# ---- report ----------------------------------------------------------------

def cpu_address(off, system):
    """Address as the CPU sees it: GB bank n in 4000-7fff, SG-1000 unbanked;
    GG/SMS pages move between slots, so their offset in the page only."""
    if system == 'gameboy':
        return f'{off if off < BANK else BANK + off % BANK:04x}'
    if system == 'sg1000':
        return f'{off:04x}'
    return f'{off % BANK:04x}'


def cmd_report(store, quiet=False):
    env = read_env(store)
    system = env.get('SYSTEM', 'gameboy')
    rom_path = env.get('ROM', '')
    rom_size = os.path.getsize(rom_path) if rom_path and os.path.isfile(rom_path) else 0
    if system in ('gg', 'sms') and rom_size % BANK == 512:  # copier header
        rom_size -= 512
    names = sorted(os.listdir(store))
    sessions, all_code, all_reads, per_session = [], {}, {}, []
    for name in names:
        m = re.fullmatch(r'coverage-(.+)\.cov', name)
        if not m:
            continue
        sid = m.group(1)
        code, reads, frames = parse_cov(os.path.join(store, name), system)
        if system == 'gameboy':
            reads = parse_romdata(os.path.join(store, f'romdata-{sid}.prof'))
            frames = input_frames(os.path.join(store, f'coverage-{sid}.input'))
        per_session.append((sid, set(code)))
        for off, n in code.items():
            all_code[off] = all_code.get(off, 0) + n
        for c, n in reads.items():
            all_reads[c] = all_reads.get(c, 0) + n
        sessions.append({
            'id': sid, 'kind': session_kind(sid), 'frames': frames,
            'input': os.path.exists(os.path.join(store, f'coverage-{sid}.input')),
            'instructions': len(code), 'executions': sum(code.values()),
            'data_chunks': len(reads),
        })
    # instructions only this session reached: which sessions are worth keeping
    seen = {}
    for _sid, offs in per_session:
        for off in offs:
            seen[off] = seen.get(off, 0) + 1
    for s, (_sid, offs) in zip(sessions, per_session):
        s['unique_instructions'] = sum(1 for off in offs if seen[off] == 1)

    size = max(rom_size, (max(all_code) + 1) if all_code else 0, (max(all_reads) + 1) * CHUNK if all_reads else 0)
    banks = []
    code_chunks = {off // CHUNK for off in all_code}
    for b in range((size + BANK - 1) // BANK):
        row, end = [], min((b + 1) * BANK, size)
        chunks = range(b * BANK // CHUNK, (end + CHUNK - 1) // CHUNK)
        for c in chunks:
            is_code, is_data = c in code_chunks, c in all_reads
            row.append('B' if is_code and is_data else 'C' if is_code else 'D' if is_data else '.')
        offs = [o for o in all_code if b * BANK <= o < (b + 1) * BANK]
        banks.append({
            'bank': b, 'offset': f'{b * BANK:06x}',
            'instructions': len(offs), 'executions': sum(all_code[o] for o in offs),
            'data_bytes_read': CHUNK * sum(1 for c in chunks if c in all_reads),
            'map': ''.join(row),
        })
    hot = sorted(all_code.items(), key=lambda kv: -kv[1])[:32]

    builds = {}
    for name in names:
        m = re.fullmatch(r'build-(.+)\.txt', name)
        if m:
            builds[m.group(1)] = parse_build(os.path.join(store, name))

    gm0 = {}
    gm0_dir = os.path.join(store, 'gm0')
    if os.path.isdir(gm0_dir):
        for name in sorted(os.listdir(gm0_dir)):
            m = re.fullmatch(r'(.+?)-(\d{8}-\d{6})\.cyc', name)
            if not m:
                continue
            target = m.group(1)
            symbols = load_syms(os.path.join(store, f'{target}.syms.tsv'))
            runs = gm0.setdefault(target, [])
            summary = run_summary(os.path.join(gm0_dir, name), symbols, 30)
            summary['date'] = m.group(2)
            runs.append(summary)
        for target, runs in gm0.items():
            for r in runs[:-1]:
                r['top'] = r['top'][:10]  # the latest run in full, older ones short

    report = {
        'format': 'm0boy.profile.v1',
        'rom': {'name': env.get('NAME'), 'file': os.path.basename(rom_path) or None, 'path': rom_path or None,
                'sha1': env.get('SHA1'), 'system': system, 'size': rom_size or None},
        'totals': {
            'sessions': len(sessions), 'instructions': len(all_code),
            'executions': sum(all_code.values()),
            'data_bytes_read': CHUNK * len(all_reads),
            'code_bytes_touched': CHUNK * len(code_chunks),
            'rom_touched_percent': round(100.0 * CHUNK * len(code_chunks | set(all_reads)) / size, 2) if size else 0.0,
        },
        'sessions': sessions,
        'rom_map': {
            'bank_size': BANK, 'chunk': CHUNK,
            'legend': {'C': 'code executed', 'D': 'data read', 'B': 'both', '.': 'untouched'},
            'banks': banks,
        },
        'hot_instructions': [{'offset': f'{o:06x}', 'bank': o // BANK, 'address': cpu_address(o, system),
                              'executions': n} for o, n in hot],
        'builds': builds,
        'gm0': {'note': 'cycles counted by the gm0 emulator (EMU_PROF): estimates, not device measurements',
                'runs': gm0},
    }
    try:
        with open(os.path.join(store, 'storage.json'), encoding='utf-8') as f:
            st = json.load(f)
        report['storage'] = {k: st.get(k) for k in ('note', 'target', 'traffic', 'calibration', 'budget', 'min_block',
                                                    'cross_validation')}
        for k in ('plan', 'result', 'ways_curve', 'budget_curve'):
            report['storage'][k] = st['final'].get(k)
    except (OSError, ValueError, KeyError):
        pass
    out = os.path.join(store, 'profile.json')
    with open(out + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1, ensure_ascii=False)
        f.write('\n')
    os.replace(out + '.tmp', out)
    if not quiet:
        t = report['totals']
        print(f"[i] Profil : {t['sessions']} parties, {t['instructions']} instructions, "
              f"{t['data_bytes_read'] // 1024} Kio de données lues, {t['rom_touched_percent']} % de la ROM -> {out}")
    return report


# ---- ROM storage: traces, simulation, plan --------------------------------

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Blocks under 512 bytes lose on held-out sessions once access costs count
# (Ristar, docs/PROFILING_STOCKAGE: raw, LZSS or zero-elided), and the
# runtimes keep whole sectors: the minimum size is chosen from 512 up.
MIN_SIZES = (512, 1024, 2048, 4096)
ALL_SIZES = (512, 1024, 2048, 4096, 8192, 16384)
GB_SIZES = (512, 4096)  # Game Boy: 4 KiB chunks raw, sectors packed


def session_args(store, sid, system, rom):
    """Host arguments that play session sid again, as it was recorded (the
    automatic ones from .auto-profile-config, the others from their .input),
    or None when it cannot be replayed."""
    inp = os.path.join(store, f'coverage-{sid}.input')
    if system == 'gb':  # the generated harness (its interpreter): recorded inputs only
        if not os.path.exists(inp):
            return None
        last = input_frames(inp) or 0
        return ['--interp', '--frames', str(min(last + 120, TRACE_MAX_FRAMES)), '--input', inp]
    if os.path.exists(inp):
        last = input_frames(inp) or 0
        return [rom, '--frames', str(min(last + 60, TRACE_MAX_FRAMES)), '--input', inp]
    try:
        with open(os.path.join(store, '.auto-profile-config'), encoding='utf-8') as f:
            version, frames, _seeds, _explorers, explore = f.read().split()
    except (OSError, ValueError):
        return None
    if version != 'v2':
        return None
    m = re.fullmatch(r'(auto|idle|rand|explore)(\d*)', sid)
    if not m:
        return None
    kind, n = m.groups()
    if kind == 'auto' and not n:
        return [rom, '--frames', frames, '--buttons', '16', '--start-frame', '30']
    if kind == 'idle' and not n:
        return [rom, '--frames', frames]
    if kind == 'rand' and n:
        return [rom, '--frames', frames, '--start-frame', '30', '--random', n]
    if kind == 'explore' and n:
        # the coverage-guided explorer is retired (deepeel explores): an
        # explore session is a long seeded play
        return [rom, '--frames', explore, '--random', n]
    return None


# a session traced for the storage plan: its first ten minutes at most
# (a three-hour gm0 session would make each simulation round minutes long)
TRACE_MAX_FRAMES = int(os.environ.get('M0_TRACE_MAX_FRAMES', 36000))


def project_host(project):
    """The host harness of a generated project (its own compiled code, the
    firmware's timing and traffic), built with -DM0_ROM_TRACE=ON in
    <project>/host/build-trace; None when it cannot be built."""
    import subprocess
    src = os.path.join(project, 'host')
    if not os.path.isfile(os.path.join(src, 'CMakeLists.txt')):
        return None
    build = os.path.join(src, 'build-trace')
    if subprocess.run(['cmake', '-G', 'Ninja', '-S', src, '-B', build, '-DM0_ROM_TRACE=ON'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode or \
       subprocess.run(['ninja', '-C', build], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        return None
    exe = [os.path.join(build, n) for n in os.listdir(build) if n.endswith('_host')]
    return exe[0] if exe else None


def ensure_traces(store, env, jobs, target='meta', project=None):
    """Every replayable session's ROM trace, made again when missing or
    older than the session or the harness.  With the target's generated
    project (project: its last build), its own host harness replays them:
    the firmware's compiled code, timing and ROM traffic (gm0 agreed read
    for read on Ristar).  Otherwise the generic harness, which interprets
    everything: with compiled-<target>.txt it leaves out the fetches of the
    compiled instructions.  Returns ({id: path}, whether fetches are in)."""
    import concurrent.futures
    import subprocess
    system = {'gameboy': 'gb'}.get(env.get('SYSTEM'), env.get('SYSTEM'))
    host, rom = env.get('HOST', ''), env.get('ROM', '')
    if system not in ('gg', 'sms', 'gb') or not os.path.isfile(host) or not os.path.isfile(rom):
        raise SystemExit(f'[x] storage : harnais {host or "absent"} ou ROM introuvable')
    generated = project_host(project) if project and system != 'gb' else None
    compiled = os.path.join(store, f'compiled-{target}.txt')
    if generated or not os.path.exists(compiled):
        compiled = None
    out = os.path.join(REPO, 'out', 'romtrace', os.path.basename(os.path.dirname(store)), os.path.basename(store),
                       target + '-project' if generated else target if compiled else 'data')
    if generated:
        host = generated
    os.makedirs(out, exist_ok=True)
    work, traces = [], {}
    for name in sorted(os.listdir(store)):
        m = re.fullmatch(r'coverage-(.+)\.cov', name)
        if not m:
            continue
        sid = m.group(1)
        args = session_args(store, sid, system, rom)
        if not args:
            continue
        if generated:
            args = args[1:]  # the generated harness carries its cartridge
        path = os.path.join(out, f'{sid}.trace.gz')
        traces[sid] = path
        newest = max(os.path.getmtime(os.path.join(store, name)), os.path.getmtime(host),
                     os.path.getmtime(compiled) if compiled else 0)
        if not os.path.exists(path) or os.path.getmtime(path) < newest:
            work.append((sid, args, path))

    def run(item):
        sid, args, path = item
        cov = path[:-9] + '.cov'
        skip = ['--rom-trace-skip', compiled] if compiled else []
        r = subprocess.run([host, *args, '--coverage', cov, '--rom-trace-out', path[:-9] + '.tmp.gz', *skip],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode:
            raise RuntimeError(f'{sid}: {r.stderr.strip()}')
        os.replace(path[:-9] + '.tmp.gz', path)
        os.remove(cov)
    if work:
        print(f'[i] Traces d\'accès ROM : {len(work)} partie(s) rejouée(s) -> {out}')
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            list(pool.map(run, work))
    return traces, compiled is not None or generated is not None


RAM_BASE = {'meta': 0x20000000, 'pokitto': 0x10000000}


def cmd_calibrate(store, runs):
    """What an SD read and an LZSS decode cost the firmware, and the cache's
    ways at the end: cycles gm0 counted in the card and codec functions,
    over the firmware's own counters read in its RAM (--dump-ram).  runs:
    [(capture.cyc, ram.bin)], the same session cut at different lengths;
    from two on, a least-squares line per counter sets apart what does not
    scale with it (opening the files at boot: 3.6 M cycles on Ristar).  The
    SPI rate switches follow the screen and the buttons, every frame: left
    out.  gm0 estimates, not device measurements."""
    import struct
    target = os.path.basename(runs[0][0]).split('-')[0]
    symbols = load_syms(os.path.join(store, f'{target}.syms.tsv'))
    if not symbols or target not in RAM_BASE:
        print(f'[!] Calibration : symboles {target} absents')
        return None
    addr = {name: (a, size) for a, size, name in symbols}
    rows = []
    for cyc, ram_path in runs:
        with open(ram_path, 'rb') as f:
            ram = f.read()

        def var(name, size=4):
            if name not in addr:
                return None
            o = addr[name][0] - RAM_BASE[target]
            return struct.unpack_from('<I' if size == 4 else '<B', ram, o)[0] if 0 <= o <= len(ram) - size else None
        _total, per = gm0_cycles(cyc, symbols)
        card = sum(c for n, c in per.items()
                   if re.match(r'(meta|pokitto)_(sd_|spi_transfer|card_|fat)|g[gb]l?_platform_rom_read$', n))
        codec = sum(c for n, c in per.items() if n.startswith(('m0_rom_unpack512', 'm0_rom_lzss', 'gbl_unpack512')))
        rows.append((var('gg_stream_reads') or 0, var('gg_stream_unpacked') or var('gbl_stream_unpacked') or 0,
                     card, codec, var('cache_ways', 1)))
    reads = [r[0] for r in rows]
    if not max(reads):
        print('[!] Calibration : aucune lecture SD dans ces runs')
        return None

    def slope(xs, ys):
        if len(xs) < 2 or max(xs) == min(xs):
            return (ys[-1] / xs[-1] if xs[-1] else None), 0.0
        n, mx, my = len(xs), sum(xs) / len(xs), sum(ys) / len(ys)
        b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        return b, my - b * mx
    sd, boot = slope(reads, [r[2] for r in rows])
    lz, _ = slope([r[1] for r in rows], [r[3] for r in rows])
    cal = {'note': 'gm0 cycle estimates over the firmware counters, not device measurements',
           'method': 'regression' if len(rows) >= 2 else 'single',
           'runs': [os.path.basename(c) for c, _ in runs], 'sd_reads': reads, 'sd_cycles': round(sd),
           'boot_card_cycles': round(boot), 'decodes': [r[1] for r in rows],
           'lzss_cycles': round(lz) if lz else None, 'ways': rows[-1][4]}
    out = os.path.join(store, f'calibration-{target}.json')
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(cal, f, indent=1)
    print(f"[i] Calibration {target} ({cal['method']}, {len(rows)} run(s)) : lecture SD {cal['sd_cycles']} cycles, "
          f"décodage LZSS 512 o {cal['lzss_cycles']} cycles, cache {cal['ways']} voies (estimations gm0)")
    return cal


def romsim(rom, traces, *args):
    import subprocess
    exe = os.path.join(REPO, 'build', 'bin', 'm0romsim')
    r = subprocess.run([exe, *map(str, args), rom, *traces], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    return json.loads(r.stdout)


def held_out_folds(traces, limit=None):
    """Sessions held out in turn: the long and the human ones (explore,
    gm0, played), which resemble a player, the human ones first; the others
    always train."""
    human = [sid for sid in traces if re.match(r'(gm0-|ai|\d{8}-)', sid)]
    test = human + [sid for sid in traces if sid.startswith('explore')]
    if len(test) < 2:
        test = sorted(traces)
    return test[:limit] if limit else test


def cart_on_card(project):
    """Does the generated Sega project keep its cartridge RAM in the save file
    (GG_CART_SD): its blocks then share the cache with the ROM's."""
    try:
        with open(os.path.join(project, 'src', 'sg_config.h'), encoding='utf-8') as f:
            return re.search(r'^#define GG_CART_SD\s+1\b', f.read(), re.M) is not None
    except OSError:
        return False


def cmd_storage(store, budget=None, jobs=None, quick=False, target='meta', project=None):
    """Traced by the target's generated project when given (its last build),
    else with the firmware's compiled list (its interpreted fetches count),
    else data reads only.  Access costs: the runtime's, measured in gm0
    (m0romsim --runtime lean); the SD read and LZSS decode costs and the
    ways from calibration-<target>.json when a gm0 run measured them.
    quick: no sensitivity curves, three sessions held out at most
    (compile.sh).  The same for every target console: only the cache's
    shape differs."""
    import concurrent.futures
    env = read_env(store)
    jobs = jobs or os.cpu_count() or 4
    rom = env.get('ROM', '')
    traces, fetches = ensure_traces(store, env, jobs, target, project)
    model = ['--fetch', 1 if fetches else 0, '--runtime', 'lean']
    sega = env.get('SYSTEM') in ('gg', 'sms')
    if target == 'pokitto':  # USB RAM bank: four 512-byte blocks on Game Gear, two on Master System (it shares the bank with its tables)
        model += ['--block', 512, '--ways', 2 if env.get('SYSTEM') == 'sms' else 4]
    if sega and project and cart_on_card(project):  # the save file's blocks compete with the ROM's
        model += ['--cart-on-card', 1]
    calibration = None
    try:
        with open(os.path.join(store, f'calibration-{target}.json'), encoding='utf-8') as f:
            calibration = json.load(f)
        if calibration.get('method') == 'regression':  # older ones counted boot and per-frame SPI in
            model += ['--sd-cycles', calibration['sd_cycles']]
            if calibration.get('lzss_cycles'):
                model += ['--lzss-cycles', calibration['lzss_cycles']]
        if calibration.get('ways'):
            model += ['--ways', calibration['ways']]
    except (OSError, ValueError, KeyError):
        pass
    if len(traces) < 2:
        raise SystemExit('[x] storage : au moins deux parties rejouables sont nécessaires')
    if budget is None:
        budget = 65536
        for name in os.listdir(store):
            if name == f'build-{target}.txt':
                for line in parse_build(os.path.join(store, name))['log']:
                    m = re.search(r'ROM flash pool:.*?(\d+) bytes', line)
                    if m:
                        budget = int(m.group(1))
    folds = held_out_folds(traces, 3 if quick else None)
    gb = env.get('SYSTEM') in ('gb', 'gameboy')
    if gb and not (calibration and calibration.get('ways')):  # the unified pool of the Game Boy firmwares, not Game Gear's 12 ways
        model += ['--ways', 61]
    mins, every_size = ((512,), GB_SIZES) if gb else (MIN_SIZES, ALL_SIZES)
    if gb:
        model += ['--block', 64, '--raw-sizes', 4096]
    print(f'[i] Validation croisée : {len(mins)} tailles minimales x {len(folds)} parties tenues à l\'écart, '
          f'budget {budget} octets')
    tasks = [(size, ','.join(str(x) for x in every_size if x >= size), sid) for size in mins for sid in folds]

    def fold(task):
        size, sizes, sid = task
        plan = os.path.join(REPO, 'out', 'romtrace', f'.fold-{os.getpid()}-{size}-{sid}.plan')
        train = [p for k, p in sorted(traces.items()) if k != sid]
        romsim(rom, train, *model, '--budget', budget, '--sizes', sizes, '--plan', plan)
        test = romsim(rom, [traces[sid]], *model, '--simulate-plan', plan)['given_plan']['result']
        os.remove(plan)
        return size, sid, test
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(fold, tasks))
    table = []
    for size in mins:
        rows = [(sid, t) for s, sid, t in results if s == size]
        frames = sum(t['frames'] for _, t in rows) or 1
        table.append({
            'min_block': size,
            'held_out_sd_reads_per_frame': round(sum(t['sd_reads'] for _, t in rows) / frames, 4),
            'held_out_cycles_per_frame': round(sum(t['estimated_cycles_per_frame'] * t['frames'] for _, t in rows) / frames, 1),
            'held_out_p99_max': max(t['sd_reads_per_frame']['p99'] for _, t in rows),
            'held_out_max': max(t['sd_reads_per_frame']['max'] for _, t in rows),
            'folds': {sid: {'sd_reads_per_frame': t['sd_reads_per_frame']['mean'], 'max': t['sd_reads_per_frame']['max']}
                      for sid, t in rows}})
    best = min(table, key=lambda r: (r['held_out_cycles_per_frame'], r['held_out_max']))
    sizes = ','.join(str(x) for x in every_size if x >= best['min_block'])
    plan_path = os.path.join(store, 'rom.plan')
    curves = [] if quick else ['--ways-curve', '12,24,48,64',
                               '--budget-curve', ','.join(str(budget * k // 4) for k in (2, 6, 8))]
    every = sorted(traces.values())
    final = romsim(rom, every, *model, '--budget', budget, '--sizes', sizes, '--plan', plan_path, *curves)
    # the generator's ranking: what each block saves, beyond this budget
    # too (its code competes for the same flash)
    romsim(rom, every, *model, '--budget', budget * 3, '--sizes', sizes, '--plan', os.path.join(store, 'rom.rank'))
    storage = {
        'format': 'm0boy.storage.v1',
        'note': 'card cache simulated on host traces of the recorded sessions: estimates, not device measurements',
        'budget': budget, 'sessions': sorted(traces), 'held_out': folds, 'target': target,
        'traffic': 'data reads and the fetches of the instructions this firmware interprets' if fetches
                   else 'data reads only (no compiled list yet)',
        'calibration': calibration,
        'min_block': best['min_block'], 'cross_validation': table, 'final': final,
    }
    out = os.path.join(store, 'storage.json')
    with open(out + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(storage, f, indent=1)
    os.replace(out + '.tmp', out)
    print('[i] Taille minimale de bloc par validation croisée (lectures SD / image, parties tenues à l\'écart) :')
    for r in table:
        print(f"    {r['min_block']:>5} o : {r['held_out_sd_reads_per_frame']:.3f}, pire image {r['held_out_max']}, "
              f"{r['held_out_cycles_per_frame']:.0f} cycles/image estimés{' <=' if r is best else ''}")
    res = final['result']['sd_reads_per_frame']
    print(f"[i] Plan ({best['min_block']} o et plus) : {final['plan']['flash_bytes']} octets de flash, "
          f"{final['plan']['rom_bytes_shipped']} octets de ROM, {res['mean']:.3f} lecture SD / image sur toutes "
          f"les parties -> {plan_path}")
    cmd_report(store, quiet=True)
    return storage


def cmd_top(store, cyc, n=20):
    target = os.path.basename(cyc).split('-')[0]
    symbols = load_syms(os.path.join(store, f'{target}.syms.tsv'))
    total, per = gm0_cycles(cyc, symbols)
    if not total:
        print('[!] Profil de cycles vide')
        return
    print(f'[i] gm0 {target} : {total} cycles émulés (estimation gm0, pas une mesure sur console)')
    if symbols is None:
        print(f'[!] {target}.syms.tsv absent : adresses non nommées (refaire le build avec compile.sh)')
    for name, c in sorted(per.items(), key=lambda kv: -kv[1])[:n]:
        print(f'  {100.0 * c / total:6.2f} %  {name}')


def main(argv):
    if len(argv) >= 3 and argv[0] == 'syms':
        cmd_syms(argv[1], argv[2])
    elif len(argv) >= 2 and argv[0] == 'report':
        cmd_report(argv[1], '--quiet' in argv[2:])
    elif len(argv) >= 4 and argv[0] == 'session':
        cmd_session(argv[1], argv[2], argv[3])
    elif len(argv) >= 2 and argv[0] == 'storage':
        cmd_storage(argv[1], int(argv[argv.index('--budget') + 1]) if '--budget' in argv else None,
                    quick='--quick' in argv, target=argv[argv.index('--target') + 1] if '--target' in argv else 'meta',
                    project=argv[argv.index('--project') + 1] if '--project' in argv else None)
    elif len(argv) >= 4 and argv[0] == 'calibrate' and len(argv) % 2 == 0:
        cmd_calibrate(argv[1], list(zip(argv[2::2], argv[3::2])))
    elif len(argv) >= 3 and argv[0] == 'top':
        cmd_top(argv[1], argv[2], int(argv[3]) if len(argv) > 3 else 20)
    else:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
