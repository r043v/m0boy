"""Exercise batch routing/caching with synthetic ROMs and fake build tools."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='m0_batch_') as temporary:
    root = Path(temporary)
    shutil.copy2(SOURCE / 'compile.sh', root / 'compile.sh')
    shutil.copytree(SOURCE / 'tools/profile', root / 'tools/profile')
    (root / 'build/bin').mkdir(parents=True)
    commands = root / 'commands'
    commands.mkdir()
    tool = '''#!/usr/bin/env python3
import json,sys,pathlib,os
name=pathlib.Path(sys.argv[0]).name;args=sys.argv[1:]
with open('calls.jsonl','a') as f:f.write(json.dumps([name,args])+'\\n')
def value(flag,default=None):return args[args.index(flag)+1] if flag in args else default
if name=='gbrecomp':
 if args and args[0].endswith('-version'):print('0.8.1');sys.exit()
 out=pathlib.Path(value('-o'));(out/'src').mkdir(parents=True,exist_ok=True);(out/'host').mkdir(exist_ok=True)
 (out/'src/gbl_config.h').write_text('')
 (out/'mock_target').write_text(value('--target'))
 print('compiled: 1')
elif name=='sg1000_host':
 if os.environ.get('FAIL_PROFILE')=='1':sys.exit(1)
 p=pathlib.Path(value('--coverage'));p.write_text('# m0boy.sg1000.coverage.v1\\n0000\\n')
elif name in ('pio','ninja','make'):
 out=pathlib.Path(value('-d') if name=='pio' else value('-C'))
 if name=='ninja' and out.name=='build':out=out.parent
 if (out/'mock_target').exists():
  target=(out/'mock_target').read_text();sg='sg1000' in out.parts
  build=out/('.pio/build/meta' if target=='meta' else 'build');build.mkdir(parents=True,exist_ok=True)
  (build/'firmware.bin').write_bytes((('m0boy-sg1000-' if sg else 'gbrecomp-')+target+' 0.8.1').encode())
 print('flash: 42; RAM: 32; Flash: 42')
elif name=='nproc':print('4')
'''
    for name in ('gbrecomp', 'sg1000_host', 'cmake', 'ninja', 'make', 'pio', 'nproc'):
        path = root / ('build/bin' if name in ('gbrecomp', 'sg1000_host') else 'commands') / name
        path.write_text(tool)
        path.chmod(0o755)
    for directory in ('gb', 'sg'):
        (root / 'roms' / directory).mkdir(parents=True)
    # Identical bytes under two source systems are separate cartridges.
    (root / 'roms/gb/Same.gb').write_bytes(b'synthetic')
    (root / 'roms/sg/Same.SG').write_bytes(b'synthetic')
    (root / 'roms/sg/Duplicate.sg').write_bytes(b'synthetic')
    (root / 'roms/sg/Second game.sg').write_bytes(b'other synthetic')
    # Old layout (<rom dir>/profiles/<sha1>/<system>/): moved into the shared store.
    import hashlib
    legacy = root / 'roms/sg/profiles' / hashlib.sha1(b'other synthetic').hexdigest() / 'sg1000'
    legacy.mkdir(parents=True)
    (legacy / 'coverage-legacy.cov').write_text('# m0boy.sg1000.coverage.v2\n0010 5\n')
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'], TARGET='both',
               ALL='1', AUTO='0', STATIC='0', PACK='0', NO_ICONS='1', JOBS='1', PLAY='0')
    def run(*args, **overrides):
        result = subprocess.run(['bash', './compile.sh', *args], cwd=root,
                                env=dict(env, **overrides), text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout
    run()
    calls = [json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    # One profile per ROM, shared by both consoles, under profiles/<source>/<game>/.
    assert not (root/'roms/sg/profiles').exists()
    second = root/'profiles/sg/Second game'
    assert (second/'coverage-legacy.cov').exists()
    assert sorted(p.name for p in (root/'profiles').iterdir()) == ['gb', 'sg'], list((root/'profiles').iterdir())
    stores = sorted(p for p in (root/'profiles').glob('*/*') if p.is_dir())
    assert [p.relative_to(root/'profiles').as_posix() for p in stores] == ['gb/Same', 'sg/Duplicate', 'sg/Second game'], stores
    sizes = {'Same': 9, 'Duplicate': 9, 'Second game': 15}
    for store in stores:
        assert (store/'build-meta.txt').exists() and (store/'build-pokitto.txt').exists(), sorted(os.listdir(store))
        assert os.access(store/'play.sh', os.X_OK)
        report = json.loads((store/'profile.json').read_text())
        assert report['format'] == 'm0boy.profile.v1' and report['rom']['size'] == sizes[store.name], report['rom']
        assert set(report['builds']) == {'meta', 'pokitto'}
    report = json.loads((second/'profile.json').read_text())
    assert report['totals']['instructions'] == 1 and report['sessions'][0]['kind'] == 'other'
    assert report['rom_map']['banks'][0]['map'].startswith('C')
    help_text = subprocess.run([str(second/'play.sh'), '-h'], text=True, capture_output=True, timeout=30).stdout
    assert 'record' in help_text and 'gm0' in help_text, help_text
    # A session added to the profile after the firmware makes the game due again.
    import time
    time.sleep(1.1)
    (second/'coverage-20261022-120000.cov').write_text('# m0boy.sg1000.coverage.v2\n0020 1\n')
    rebuilt = run('roms/sg', TARGET='meta')
    assert 'Réussis : 1' in rebuilt and 'Déjà à jour : 1' in rebuilt, rebuilt
    (second/'coverage-20261022-120000.cov').unlink()
    generations = [args for name,args in calls if name=='gbrecomp' and '-o' in args]
    sg = [args for args in generations if '--system' in args]
    assert len(sg)==4, sg  # two SG cartridges, both destinations
    assert all(args[args.index('--system')+1]=='sg1000' for args in sg)
    assert all(not any(flag in args for flag in ('--meta-pack','--meta-static','--meta-bytes-per-instruction')) for args in sg)
    assert all(args[args.index('--meta-compiler')+1]=='gcc' for args in sg)  # recompiled Z80: firmware compiler forwarded
    # Every generation names the game's files on the card (--sd-name, 8.3).
    assert all('--sd-name' in args and 1 <= len(args[args.index('--sd-name')+1]) <= 8 for args in generations), generations
    assert any(name=='make' for name,args in calls)  # GB pipeline unchanged
    assert any(name=='cmake' and '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake' in args for name,args in calls)
    # Un .zip par jeu, le répertoire du jeu (avec .gbrmeta.json) dedans.
    zips = list((root/'roms').glob('*/*/*.zip'))
    assert len(zips)==6, zips
    import zipfile as zf
    for z in zips:
        with zf.ZipFile(z) as a:
            names = a.namelist()
            game = z.stem
            # META: <NOM>.BIN, the 8.3 base of the game's files (sd_name_of);
            # Pokitto: <Jeu>.bin.
            bins = [n for n in names if n.startswith(game + '/') and n.lower().endswith('.bin')]
            assert len(bins) == 1, names
            base = bins[0][len(game) + 1:-4]
            assert base == game or (bins[0].endswith('.BIN') and 1 <= len(base) <= 8
                                    and base.isalnum() and base.isupper() or base.isdigit()), names
            assert f'{game}/.gbrmeta.json' in names, names
    (root/'calls.jsonl').write_text('')
    output=run(PACK='1')  # SG ignores GB packing, already-built SG stays cached
    calls=[json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    assert not any(name=='gbrecomp' and '--system' in args for name,args in calls), calls
    assert 'Déjà à jour : 2' in output
    # Explicit SG directory; AUTO profile uses the SG host, not GB profiling tools.
    (root/'calls.jsonl').write_text('')
    run('roms/sg', 'output/sg card', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='2')
    calls=[json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    hosts=[args for name,args in calls if name=='sg1000_host']
    explorations=[args for args in hosts if '--frames' in args and args[args.index('--frames')+1]=='12000']
    profiles=[args for args in hosts if not ('--frames' in args and args[args.index('--frames')+1]=='12000')]
    # per cartridge: button 1 held, no input, then SG_SEEDS random games
    assert len(profiles)==8 and all(args[args.index('--frames')+1]=='120' for args in profiles)
    # and EXPLORERS (4) long seeded plays of AUTO_SECONDS x 6000 frames
    assert len(explorations)==8 and all('--random' in args for args in explorations)
    assert len(list((root/'profiles/sg').glob('*/coverage-explore*.cov')))==8
    assert sum('--random' in args for args in profiles)==4
    assert sum('--buttons' not in args and '--random' not in args for args in profiles)==2
    assert len(list((root/'profiles/sg').glob('*/coverage-auto.cov')))==2
    assert len(list((root/'profiles/sg').glob('*/coverage-idle.cov')))==2
    assert len(list((root/'profiles/sg').glob('*/coverage-rand*.cov')))==4
    assert len(list((root/'output/sg card').glob('sg/*.zip')))==2
    # games nested by source platform under each destination
    assert len(list((root/'roms/meta/gb').glob('*.zip')))==1
    assert len(list((root/'roms/meta/sg').glob('*.zip')))==2
    assert len(list((root/'roms/pokitto/sg').glob('*.zip')))==2
    assert all('--coverage' in args for name,args in calls if name=='gbrecomp' and '--system' in args)
    # Corrected automatic profiles are cached only for the same revision and
    # requested counts/durations. Hand-recorded profiles must survive refresh.
    stores=list((root/'profiles/sg').glob('*'))
    for store in stores:
        assert (store/'.auto-profile-config').read_text().strip()=='v2 120 2 4 12000'
        (store/'coverage-manual.cov').write_text('# m0boy.sg1000.coverage.v1\n1234\n')
    def profile_calls():
        return [args for name,args in map(json.loads,(root/'calls.jsonl').read_text().splitlines())
                if name=='sg1000_host']
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='2')
    assert not profile_calls()  # same settings: reuse
    for store in stores:
        (store/'.auto-profile-config').unlink()  # pre-fix, aliased seed cache
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='2')
    assert len(profile_calls())==16
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='3', EXPLORERS='5')
    assert len(profile_calls())==20
    assert sum('--random' in a and a[a.index('--random')+1]=='3' for a in profile_calls())==4
    assert sum('--frames' in a and a[a.index('--frames')+1]=='12000' and a[a.index('--random')+1]=='5' for a in profile_calls())==2
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='3', EXPLORERS='5',
        SG_AUTO_FRAMES='240', EXPLORE_FRAMES='24000')
    assert len(profile_calls())==20
    rand_calls=[a for a in profile_calls() if '--frames' in a and a[a.index('--frames')+1]=='240']
    explore_calls=[a for a in profile_calls() if '--frames' in a and a[a.index('--frames')+1]=='24000']
    assert len(rand_calls)==10 and len(explore_calls)==10  # per cartridge: auto+idle+3 rand at 240, 5 explore at 24000
    assert all('--random' in a for a in explore_calls)
    # A failed refresh cannot certify an incomplete run as a current cache.
    failed=subprocess.run(['bash','./compile.sh','roms/sg'],cwd=root,
                          env=dict(env,TARGET='pokitto',AUTO='1',FORCE='1',SG_SEEDS='3',EXPLORERS='5',
                                   SG_AUTO_FRAMES='241',EXPLORE_FRAMES='24000',FAIL_PROFILE='1'),
                          text=True,capture_output=True,timeout=30)
    assert failed.returncode!=0, failed.stdout+failed.stderr
    for store in stores:
        assert (store/'.auto-profile-config').read_text().strip()=='v2 240 3 5 24000'
        assert (store/'coverage-manual.cov').read_text()=='# m0boy.sg1000.coverage.v1\n1234\n'
    (stores[0]/'coverage-explore5.cov').unlink()
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='3', EXPLORERS='5',
        SG_AUTO_FRAMES='240', EXPLORE_FRAMES='24000')
    assert len(profile_calls())==10  # only the cartridge with a missing session
    assert (stores[0]/'coverage-explore5.cov').exists()
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='3', EXPLORERS='5',
        SG_AUTO_FRAMES='240', EXPLORE_FRAMES='0')
    assert len(profile_calls())==10 and all(not ('--frames' in a and a[a.index('--frames')+1]=='24000') for a in profile_calls())
    # Old exploration files need not exist when exploration is disabled.
    for store in stores:
        for p in store.glob('coverage-explore*.cov'):p.unlink()
    (root/'calls.jsonl').write_text('')
    run('roms/sg', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='3', EXPLORERS='5',
        SG_AUTO_FRAMES='240', EXPLORE_FRAMES='0')
    assert not profile_calls()
print('batch: mixed source systems, both targets, duplicate filtering, caching and SG profiles pass')
