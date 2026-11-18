#!/usr/bin/env node
/*
 * Version Node.js, multiplateforme, de compile.sh (qui reste la référence ;
 * même layout, mêmes fichiers d'état, mêmes variables d'environnement).
 *
 * Convertit toutes les ROMs .gb/.gbc d'un dossier en jeux Gamebuino META,
 * rangés comme le loader de la console les attend.
 *
 * Usage :
 *   node tools/meta-compiler/compile.mjs [dossier_roms] [dossier_sortie]
 *   Par défaut : toutes les ROMs de ./roms/gb/, jeux dans ./roms/meta/gb/<Jeu>/
 *   (le sous-dossier gb/ range les jeux par plateforme source, comme
 *   compile.sh : gb, sg, gg ou sms — ce convertisseur ne fait que la GB)
 *   (le .elf de débogage reste dans out/<jeu>/.pio/build/meta/)
 *
 * Chaque jeu devient un dossier à copier tel quel à la racine de la carte SD :
 *   Tous les fichiers du jeu portent le même nom court 8.3, <NOM> (lettres
 *   et chiffres du nom du jeu en majuscules ; au-delà de 8, les 6 premiers
 *   et 2 chiffres hexadécimaux du SHA-1 de la ROM), comme compile.sh :
 *   gb/<Jeu>/<NOM>.BIN     le firmware ; le dossier garde le nom simple
 *                          (« Zerd (J) [!] » -> « Zerd »), que montre le
 *                          loader, qui prend le premier .bin du dossier.  Le
 *                          firmware retrouve son dossier par ce .BIN et y
 *                          cherche ses autres fichiers.
 *   <Jeu>/ICON.BMP         icône 32x32 du menu (grille)
 *   <Jeu>/TITLESCREEN.BMP  écran titre 80x64 du menu (détail)
 *     Icônes tirées de thumbnails.libretro.com (Named_Titles), mises en cache
 *     dans <dossier_sortie>/.cache/icons/ ; un fichier local du même nom
 *     (<dossier_roms>/icons/<Jeu>.png) a la priorité.  Un échec n'arrête
 *     rien : le loader affiche son icône par défaut.  --no-icons pour
 *     désactiver les téléchargements.  Conversion par ImageMagick ou Pillow.
 *   <Jeu>/<NOM>.SAV        fichier de sauvegarde (cartouche à pile ou grande
 *                          RAM) : créé une seule fois, la console y écrit.
 *   <Jeu>/<NOM>.GB         la ROM entière, pour les ROMs qui ne tiennent pas
 *                          dans la flash (256 Kio et plus) : le firmware
 *                          garde banque 0 et données les plus lues en flash
 *                          et lit le reste sur la carte.  <NOM>.STA : états.  Pas de limite de
 *                          taille de ROM.
 *   <Jeu>/.gbrmeta.json    état interne (ROM, sha1, version, pack) ; ignorable.
 *   (compatible avec l'état écrit par compile.sh : les deux scripts
 *   s'échangent les jeux déjà compilés, les profils et les icônes)
 *
 * Versions : chaque firmware embarque la version de m0recomp qui l'a produit
 * (chaîne « gbrecomp-meta x.y.z » dans le .bin, aussi imprimée au démarrage
 * sur le port USB).  Le script la relit : un firmware déjà à jour (même
 * version, même ROM) est conservé tel quel ; un firmware fait par une autre
 * version est proposé à la mise à jour.
 *
 * Interface :
 *   - Dans un terminal, une interface plein écran (ink, l'équivalent maintenu
 *     de blessed) :
 *       · écran de sélection : la liste des ROMs avec leur état (nouvelle, à
 *         jour, version vA -> vB, ROM modifiée, version inconnue), cochable
 *         (espace, a = tout) ; les firmwares d'une autre version sont
 *         proposés à la reconstruction (o/n) ;
 *       · tableau de bord pendant le travail : la liste des jeux (état et
 *         phase en direct) dans un panneau, les compteurs globaux et le
 *         journal d'activité dans l'autre.
 *       Touches : q = ne plus lancer de nouveaux jeux (ceux en cours
 *       finissent), Ctrl-C = tout arrêter immédiatement.
 *   - Sinon (script, tube, CI) : traite tout ce qui a à faire, sortie en
 *     lignes de texte.
 *       --all        comme si tout était coché
 *       --update     reconstruit aussi les firmwares d'une autre version
 *       --force      refait même ceux déjà à jour
 *       (mêmes décisions que ALL=1 / UPDATE=1 / FORCE=1 de compile.sh)
 *
 * Autres options (flags ou variables d'environnement de compile.sh) :
 *   --play             (PLAY=1)        ouvre une fenêtre pour jouer soi-même
 *                                      (interface texte, pas le tableau de bord)
 *   --no-auto          (AUTO=0)        pas de parties aléatoires
 *   --auto-redo        (AUTO_REDO=1)   refaire les parties aléatoires
 *   --replay           (REPLAY=1)      rejouer les parties à la main gardées
 *   --pack / --no-pack (PACK, défaut 1) données de ROM peu lues compressées
 *                                      (LZSS) dans la flash au lieu d'être
 *                                      lues sur la carte SD : plus de place
 *                                      pour le code compilé, pas de .GB à
 *                                      copier (retombe sur la SD ou le mode
 *                                      normal si la ROM compressée ne tient
 *                                      pas)
 *   --emu              (EMU=1)         variante pour le gamebuino-emulator
 *                                      d'origine, sans l'assembleur PPU
 *                                      exécuté depuis la SRAM (inutile avec
 *                                      le fork tools/gb-emu, qui exécute la
 *                                      SRAM et émule la carte SD)
 *   --no-static        (STATIC=0)      pas d'analyse statique en complément
 *   --jobs n           (JOBS)          ROMs en parallèle (défaut : cœurs / 4)
 *   --auto-seeds n     (AUTO_SEEDS, 8) parties aléatoires par ROM
 *   --auto-frames n    (AUTO_FRAMES, 36000)
 *   --auto-long-frames n (AUTO_LONG_FRAMES, 108000)
 *   --auto-seconds n   (AUTO_SECONDS, 60) plafond de temps réel par partie
 *   --profiles-dir d   (PROFILES_DIR, <repo>/profiles)
 *   --sessions-dir d   (SESSIONS_DIR, <dossier_roms>/profiles)
 *
 * Prérequis : m0recomp compilé (build/bin), cmake, ninja, pio (PlatformIO) ;
 * ImageMagick ou Pillow pour les icônes.  Téléchargements via fetch intégré
 * de Node (pas besoin de curl/wget).  ink et react sont installés par npm au
 * premier lancement interactif (l'interface texte de secours n'en a pas
 * besoin).
 */

import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn, spawnSync } from 'node:child_process';
import readline from 'node:readline/promises';
import { fileURLToPath } from 'node:url';
import os from 'node:os';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, '..', '..');
const IS_WIN = process.platform === 'win32';
const EXE = IS_WIN ? '.exe' : '';
const GBRECOMP = path.join(ROOT, 'build', 'bin', `m0recomp${EXE}`);
const THUMB_BASE = 'https://thumbnails.libretro.com';
/* Les jeux sont rangés par plateforme source dans le dossier de sortie
 * (compile.sh : gb, sg, gg, sms ; ce convertisseur ne produit que la GB). */
const SYS_DIR = 'gb';

/* ---------------------------------------------------------------- options */

function parseArgs(argv) {
  const env = (name, dflt) => process.env[name] ?? dflt;
  const num = (v, dflt) => {
    const n = parseInt(v, 10);
    return Number.isFinite(n) && n > 0 ? n : dflt;
  };
  const cfg = {
    romDir: null,
    destDir: null,
    play: env('PLAY', '0') === '1',
    auto: env('AUTO', '1') === '1',
    staticAnalysis: env('STATIC', '1') === '1',
    compiler: env('FIRMWARE_COMPILER', 'gcc'),
    pack: env('PACK', '1') === '1',  // auto : packe quand ça tient, retombe sur la carte SD sinon
    autoRedo: env('AUTO_REDO', '0') === '1',
    replay: env('REPLAY', '0') === '1',
    all: env('ALL', '0') === '1',
    update: env('UPDATE', '0') === '1',
    force: env('FORCE', '0') === '1',
    noIcons: env('NO_ICONS', '0') === '1',
    emu: env('EMU', '0') === '1',
    jobs: num(env('JOBS', ''), Math.max(1, Math.floor(os.cpus().length / 4))),
    autoSeeds: num(env('AUTO_SEEDS', ''), 8),
    autoFrames: num(env('AUTO_FRAMES', ''), 1080000),
    autoLongFrames: num(env('AUTO_LONG_FRAMES', ''), 3240000),
    autoSeconds: num(env('AUTO_SECONDS', ''), 300),
    profilesDir: env('PROFILES_DIR', path.join(ROOT, 'profiles')),
    sessionsDir: null,
  };
  const positionals = [];
  const needValue = new Set(['--compiler', '--jobs', '--auto-seeds', '--auto-frames',
    '--auto-long-frames', '--auto-seconds', '--profiles-dir', '--sessions-dir']);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (needValue.has(a)) {
      if (i + 1 >= argv.length) fail(`valeur manquante pour ${a}`);
      const map = {
        '--compiler': 'compiler', '--jobs': 'jobs', '--auto-seeds': 'autoSeeds', '--auto-frames': 'autoFrames',
        '--auto-long-frames': 'autoLongFrames', '--auto-seconds': 'autoSeconds',
        '--profiles-dir': 'profilesDir', '--sessions-dir': 'sessionsDir',
      };
      const key = map[a];
      cfg[key] = (a.endsWith('-dir') || a === '--compiler') ? argv[++i] : num(argv[++i], 1);
    } else switch (a) {
      case '--all': cfg.all = true; break;
      case '--update': cfg.update = true; break;
      case '--force': cfg.force = true; break;
      case '--no-icons': cfg.noIcons = true; break;
      case '--emu': cfg.emu = true; break;
      case '--play': cfg.play = true; break;
      case '--no-auto': cfg.auto = false; break;
      case '--auto-redo': cfg.autoRedo = true; break;
      case '--replay': cfg.replay = true; break;
      case '--pack': cfg.pack = true; break;
      case '--no-pack': cfg.pack = false; break;
      case '--static': cfg.staticAnalysis = true; break;
      case '--no-static': cfg.staticAnalysis = false; break;
      case '-h': case '--help': usage(); process.exit(0); break;
      default:
        if (a.startsWith('-')) fail(`option inconnue : ${a} (--help pour l'aide)`);
        positionals.push(a);
    }
  }
  if (!['gcc', 'clang'].includes(cfg.compiler)) fail('--compiler/FIRMWARE_COMPILER doit être gcc ou clang');
  cfg.romDir = positionals[0] ?? path.join('.', 'roms', 'gb');
  cfg.destDir = positionals[1] ?? path.join('.', 'roms', 'meta');
  cfg.sessionsDir = cfg.sessionsDir ?? path.join(cfg.romDir, 'profiles');
  cfg.iconCache = path.join(cfg.destDir, '.cache', 'icons');
  if (cfg.play) cfg.jobs = 1;
  return cfg;
}

function usage() {
  console.log(`Usage : node compile.mjs [dossier_roms] [dossier_sortie] [options]
Options : --compiler gcc|clang --all --update --force --no-icons --emu --play --pack --no-auto --auto-redo
          --replay --no-static --jobs n --auto-seeds n --auto-frames n
          --auto-long-frames n --auto-seconds n --profiles-dir d --sessions-dir d
Voir l'en-tête du fichier et compile.sh pour le détail.`);
}

function fail(msg) {
  console.error(`[x] ${msg}`);
  process.exit(1);
}

/* ------------------------------------------------------------- petits outils */

function sha1File(file) {
  return new Promise((resolve, reject) => {
    const h = createHash('sha1');
    fs.createReadStream(file)
      .on('data', (d) => h.update(d))
      .on('error', reject)
      .on('end', () => resolve(h.digest('hex')));
  });
}

/* « Zerd no Densetsu (J) [!] » -> « Zerd no Densetsu » */
function cleanName(fileName) {
  const base = fileName.replace(/\.[^.]*$/, '');
  let n = base
    .replace(/\[[^\]]*\]|\([^)]*\)/g, ' ')
    .replace(/\s+/g, ' ')
    .replace(/^[ ._-]+/, '')
    .replace(/[ ._-]+$/, '');
  return n || base;
}

/* Caractères interdits sur une carte FAT -> espace. */
function sdSafe(name) {
  return name.replace(/[\\/:*?"<>|]/g, ' ').replace(/\s+/g, ' ').trim();
}

/* Version de m0recomp embarquée dans un firmware (voir meta_main.cpp). */
function firmwareVersion(binFile) {
  let buf;
  try {
    buf = fs.readFileSync(binFile);
  } catch {
    return '';
  }
  const i = buf.indexOf('gbrecomp-meta ');
  if (i < 0) return '';
  const from = i + 'gbrecomp-meta '.length;
  const m = /^([0-9A-Za-z._+-]+)/.exec(buf.subarray(from, from + 32).toString('latin1'));
  return m ? m[1] : '';
}

function readState(gameDir) {
  try {
    return JSON.parse(fs.readFileSync(path.join(gameDir, '.gbrmeta.json'), 'utf8'));
  } catch {
    return {};
  }
}

function writeState(gameDir, game) {
  const state = {
    rom: path.basename(game.romPath),
    sha1: game.sha,
    version: game.version,
    prefix: game.prefix,
    pack: game.pack ? 1 : 0,
    compiler: game.compiler ?? 'gcc',
    date: new Date().toISOString().slice(0, 10),
  };
  fs.writeFileSync(path.join(gameDir, '.gbrmeta.json'), `${JSON.stringify(state, null, 2)}\n`);
}

/* #define du gbl_config.h généré. */
function gblDefines(outDir) {
  const text = fs.readFileSync(path.join(outDir, 'src', 'gbl_config.h'), 'utf8');
  const val = (name) => {
    const m = new RegExp(`^#define ${name} "(.*)"$`, 'm').exec(text);
    return m ? m[1] : '';
  };
  const num = (name) => {
    const m = new RegExp(`^#define ${name} ([0-9]+)$`, 'm').exec(text);
    return m ? parseInt(m[1], 10) : 0;
  };
  return {
    saveName: val('GBL_SAVE_NAME'),
    eram: num('GBL_ERAM_SIZE'),
    battery: /^#define GBL_BATTERY 1$/m.test(text),
    streamName: val('GBL_STREAM_NAME'),
    stateName: val('GBL_STATE_NAME'),
    stateSlots: num('GBL_STATE_SLOTS'),
    stateSectors: num('GBL_STATE_SLOT_SECTORS'),
  };
}

/* Enfants actifs : pour tout arrêter net (Ctrl-C dans l'interface). */
const activeChildren = new Set();

/* Exécute un outil ; capture la sortie, tue après timeoutMs si demandé. */
function runTool(cmd, args, { timeoutMs = 0, cwd = ROOT, stdio = 'pipe', env = process.env } = {}) {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, { cwd, windowsHide: true, stdio, env });
    activeChildren.add(child);
    let out = '';
    let err = '';
    let timer = null;
    if (stdio === 'pipe') {
      child.stdout.on('data', (d) => { out += d; });
      child.stderr.on('data', (d) => { err += d; });
    }
    if (timeoutMs > 0) timer = setTimeout(() => child.kill('SIGKILL'), timeoutMs);
    const finish = (code, extraErr) => {
      activeChildren.delete(child);
      if (timer) clearTimeout(timer);
      resolve({ code, out, err: extraErr ? `${err}${extraErr}` : err });
    };
    child.on('error', (e) => finish(-1, e.message));
    child.on('close', (code) => finish(code));
  });
}

function killActiveChildren() {
  for (const c of activeChildren) {
    try {
      c.kill('SIGKILL');
    } catch { /* déjà mort */ }
  }
  activeChildren.clear();
}

/* Instance ink active, pour désinstaller l'interface même en cas d'erreur. */
let instance = null;

/* Version de l'édition META (targets/meta/VERSION), celle inscrite dans les
 * firmwares ; un m0recomp plus ancien ne connaît que --version-json. */
async function gbrecompVersion() {
  const m = await runTool(GBRECOMP, ['--meta-version']);
  const v = (m.out || '').trim().split(/\r?\n/)[0];
  if (m.code === 0 && /^[0-9][0-9A-Za-z._+-]*$/.test(v)) return v;
  const r = await runTool(GBRECOMP, ['--version-json']);
  try {
    return JSON.parse(r.out).version || 'inconnue';
  } catch {
    return 'inconnue';
  }
}

/* Recherche d'exécutable dans le PATH (sans dépendance). */
function which(name) {
  const exts = IS_WIN ? ['.exe', '.cmd', '.bat', ''] : [''];
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    if (!dir) continue;
    for (const ext of exts) {
      const p = path.join(dir, name + ext);
      try {
        fs.accessSync(p, fs.constants.X_OK);
        return p;
      } catch { /* essai suivant */ }
    }
  }
  return null;
}

/* ------------------------------------------------------- icônes (libretro) */

let imBin = which('magick') ?? which('convert');
let pythonBin = which('python3') ?? which('python');
let havePil = false;
if (pythonBin) {
  const r = spawnSync(pythonBin, ['-c', 'import PIL'], { windowsHide: true });
  havePil = r.status === 0;
}

function makeBmp(png, bmp, w, h) {
  if (imBin) {
    return runTool(imBin, [png, '-thumbnail', `${w}x${h}^`, '-gravity', 'center',
      '-extent', `${w}x${h}`, '-strip', '-type', 'truecolor', `BMP3:${bmp}`])
      .then((r) => r.code === 0);
  }
  if (havePil) {
    const script = [
      'import sys',
      'from PIL import Image, ImageOps',
      'im = Image.open(sys.argv[1]).convert("RGB")',
      'ImageOps.fit(im, (int(sys.argv[3]), int(sys.argv[4]))).save(sys.argv[2], "BMP")',
    ].join('\n');
    return runTool(pythonBin, ['-c', script, png, bmp, String(w), String(h)])
      .then((r) => r.code === 0);
  }
  return Promise.resolve(false);
}

/* Index du catalogue libretro (nom décodé <TAB> nom encodé), en cache ; les
 * fichiers Named_Titles gardent les tags No-Intro (« Tetris (World)
 * (Rev 1).png »), un nom nettoyé seul ne suffit pas.  Même format et même
 * cache que compile.sh. */
async function thumbListing(gbc, cfg) {
  const file = path.join(cfg.iconCache, gbc ? 'listing-gbc.txt' : 'listing-gb.txt');
  const url = gbc
    ? `${THUMB_BASE}/Nintendo%20-%20Game%20Boy%20Color/Named_Titles/`
    : `${THUMB_BASE}/Nintendo%20-%20Game%20Boy/Named_Titles/`;
  if (!fs.existsSync(file) || fs.statSync(file).size === 0) {
    fs.mkdirSync(cfg.iconCache, { recursive: true });
    let text;
    try {
      const res = await fetch(url);
      if (!res.ok) return null;
      text = await res.text();
    } catch {
      return null;
    }
    const lines = [...text.matchAll(/href="([^"]+\.png)"/g)]
      .map((m) => `${decodeURIComponent(m[1]).slice(0, -4)}\t${m[1].slice(0, -4)}`);
    if (lines.length === 0) return null;
    fs.writeFileSync(file, `${lines.join('\n')}\n`);
  }
  return file;
}

/* Meilleure vignette pour un nom nettoyé : sans les tags (région, révision,
 * [!]) ; (World) puis (USA, Europe) préférés, révision la plus haute. */
function findThumb(listFile, name) {
  const norm = (s) => s.replace(/\([^)]*\)|\[[^\]]*\]/g, ' ')
    .replace(/\s+/g, ' ').trim().toLowerCase();
  const target = norm(name);
  let best = null;
  let bestScore = -Infinity;  // toute correspondance unique doit gagner (ex. « (Japan) » pénalisé)
  for (const line of fs.readFileSync(listFile, 'utf8').split('\n')) {
    const tab = line.indexOf('\t');
    if (tab < 0) continue;
    const dec = line.slice(0, tab);
    if (norm(dec) !== target) continue;
    const enc = line.slice(tab + 1);
    const dl = dec.toLowerCase();
    let score = 0;
    for (const [tag, pts] of [['(world)', 40], ['(usa, europe)', 30],
      ['(usa)', 20], ['(europe)', 20]]) {
      if (dl.includes(tag)) score += pts;
    }
    const rev = /\(rev (\d+)\)/.exec(dl);
    if (rev) score += parseInt(rev[1], 10);
    if (dl.includes('(japan)') && !dl.includes('(en)')) score -= 10;
    if (score > bestScore) {
      best = enc;
      bestScore = score;
    }
  }
  return best;
}

async function download(url, dest) {
  try {
    const res = await fetch(url);
    if (!res.ok) return false;
    fs.writeFileSync(dest, Buffer.from(await res.arrayBuffer()));
    return true;
  } catch {
    return false;
  }
}

async function fetchIcons(gameDir, name, romBase, gbc, cfg, log) {
  if (cfg.noIcons) return true;
  const png = path.join(cfg.iconCache, `${name}.png`);
  if (!fs.existsSync(png)) {
    for (const cand of [path.join(cfg.romDir, 'icons', `${name}.png`),
      path.join(cfg.romDir, 'icons', `${romBase}.png`),
      path.join(cfg.romDir, `${romBase}.png`)]) {
      if (fs.existsSync(cand)) {
        fs.mkdirSync(cfg.iconCache, { recursive: true });
        fs.copyFileSync(cand, png);
        break;
      }
    }
  }
  if (!fs.existsSync(png)) {
    /* Les jeux Game Boy Color ont leur propre catalogue libretro. */
    const sets = gbc ? [true, false] : [false, true];
    fs.mkdirSync(cfg.iconCache, { recursive: true });
    let enc = null;
    let encGbc = false;
    for (const set of sets) {
      const list = await thumbListing(set, cfg);
      if (list) enc = findThumb(list, name);
      if (enc) {
        encGbc = set;
        break;
      }
    }
    if (enc) {
      const url = `${THUMB_BASE}/Nintendo%20-%20Game%20Boy${encGbc ? '%20Color' : ''}/Named_Titles/${enc}.png`;
      if (!await download(url, png)) {
        /* réessaie l'autre catalogue si le premier échoue au téléchargement */
        const other = `${THUMB_BASE}/Nintendo%20-%20Game%20Boy${encGbc ? '' : '%20Color'}/Named_Titles/${enc}.png`;
        if (!await download(other, png)) fs.rmSync(png, { force: true });
      }
    } else {
      /* Catalogue inaccessible ou nom absent : essais directs en dernier
       * recours (nom nettoyé supposé identique au nom No-Intro). */
      for (const set of ['Nintendo%20-%20Game%20Boy', 'Nintendo%20-%20Game%20Boy%20Color']) {
        if (await download(`${THUMB_BASE}/${set}/Named_Titles/${encodeURIComponent(name)}.png`, png)) break;
        fs.rmSync(png, { force: true });
      }
    }
  }
  if (!fs.existsSync(png)) {
    log(`[!] Icône introuvable pour « ${name} » sur thumbnails.libretro.com`);
    log(`    (nom No-Intro différent ? dépose ${path.join(cfg.romDir, 'icons', `${name}.png`)})`);
    return false;
  }
  if (!await makeBmp(png, path.join(gameDir, 'TITLESCREEN.BMP'), 80, 64)
      || !await makeBmp(png, path.join(gameDir, 'ICON.BMP'), 32, 32)) {
    log(`[!] Conversion d'icône impossible pour « ${name} » (ImageMagick ou Pillow requis)`);
    fs.rmSync(path.join(gameDir, 'TITLESCREEN.BMP'), { force: true });
    fs.rmSync(path.join(gameDir, 'ICON.BMP'), { force: true });
    return false;
  }
  return true;
}

/* ------------------------------------------------------------- état d'un jeu */

/* État d'un jeu : tag lisible + décision de (re)construction.
 * todo : 0 rien, 1 à faire, 2 à confirmer. */
function assess(game, cfg, curVersion) {
  const bin = path.join(cfg.destDir, SYS_DIR, game.dir, `${sdNameOf(game.dir, game.sha)}.BIN`);
  game.todo = 1;
  game.tag = 'nouvelle';
  if (!fs.existsSync(bin)) return;
  let ver = firmwareVersion(bin);
  const state = readState(path.join(cfg.destDir, SYS_DIR, game.dir));
  if (!ver && state.version) ver = String(state.version);
  if (state.sha1 && state.sha1 !== game.sha) {
    game.tag = 'ROM modifiée';
  } else if ((state.pack ?? 0) !== (cfg.pack ? 1 : 0)) {
    game.tag = `PACK=${state.pack ?? 0} -> PACK=${cfg.pack ? 1 : 0}`;
    game.todo = 1;
  } else if ((state.compiler ?? 'gcc') !== cfg.compiler) {
    game.tag = `compilateur ${state.compiler ?? 'gcc'} -> ${cfg.compiler}`;
    game.todo = 1;
  } else if (!ver) {
    game.tag = 'version inconnue';
    game.todo = 2;
  } else if (ver === curVersion) {
    game.tag = `à jour (v${curVersion})`;
    game.todo = 0;
  } else {
    game.tag = `v${ver} -> v${curVersion}`;
    game.todo = 2;
  }
}

/* ---------------------------------------------------- entrées aléatoires */

/* Même règles que compile.sh : une touche toutes les 20 frames à partir de
 * la 30e, Start au début de chaque fenêtre de 600 frames. */
function autoInput(seed, frames) {
  let s = (seed * 2654435761) >>> 0 || 1;
  const rand = () => {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    return s / 4294967296;
  };
  const names = ['right', 'left', 'up', 'down', 'a', 'b'];
  const lines = [];
  for (let f = 30; f < frames; f += 20) {
    if (f % 600 < 20) {
      lines.push(`${f} start`);
      continue;
    }
    let b = '';
    for (let i = 0; i < 6; i++) {
      if (rand() < (i === 0 ? 0.55 : 0.25)) b += (b ? '+' : '') + names[i];
    }
    lines.push(`${f} ${b || 'none'}`);
  }
  return `${lines.join('\n')}\n`;
}

/* ------------------------------------------------------------ outils disque */

function glob(dir, pattern) {
  /* pattern : suffixe de nom simple avec joker * (ex. « coverage-*.cov »). */
  const re = new RegExp(`^${pattern.replace(/[.+^${}()|[\]\\]/g, '\\$&')
    .replace(/\*/g, '.*')}$`, 'i');
  try {
    return fs.readdirSync(dir).filter((f) => re.test(f)).sort()
      .map((f) => path.join(dir, f));
  } catch {
    return [];
  }
}

function copyIfMissing(src, dst) {
  if (!fs.existsSync(dst) && fs.existsSync(src)) fs.copyFileSync(src, dst);
}

function copyGlobIfMissing(dir, pattern, dstDir) {
  for (const f of glob(dir, pattern)) copyIfMissing(f, path.join(dstDir, path.basename(f)));
}

/* ------------------------------------------------------------- compilation */

/* Nom court 8.3 des fichiers d'un jeu sur la carte (comme sd_name_of de
 * compile.sh). */
function sdNameOf(gameName, sha) {
  let s = gameName.replace(/[^A-Za-z0-9]/g, '').toUpperCase() || 'GAME';
  if (s.length > 8) s = s.slice(0, 6) + sha.slice(0, 2).toUpperCase();
  return s;
}

async function buildGame(game, cfg, curVersion, log) {
  const { romPath, prefix, dir: gameName, sha } = game;
  const gameDir = path.join(cfg.destDir, SYS_DIR, gameName);
  const outDir = path.join(ROOT, 'out', prefix);
  const store = path.join(cfg.sessionsDir, sha);
  const gbc = /\.(gbc)$/i.test(romPath);
  const sdName = sdNameOf(gameName, sha);
  const hostBin = (name) => path.join(outDir, 'host', 'build', name + EXE);

  fs.mkdirSync(outDir, { recursive: true });
  fs.mkdirSync(store, { recursive: true });
  fs.mkdirSync(gameDir, { recursive: true });
  if (!fs.existsSync(path.join(store, 'JEU.txt'))) {
    fs.writeFileSync(path.join(store, 'JEU.txt'), `${path.basename(romPath)}\n`);
  }

  /* a0) Parties enregistrées pour cette ROM exacte (par SHA-1) : celles du
   *     dossier profiles/ fourni, puis celles sauvées par les lancements
   *     précédents dans sessionsDir/<sha1>/ (source de vérité). */
  const saveSessions = () => {
    for (const f of [...glob(outDir, 'coverage-*.cov'), ...glob(outDir, 'coverage-*.input'),
      ...glob(outDir, 'romdata-*.prof')]) {
      copyIfMissing(f, path.join(store, path.basename(f)));
    }
    copyIfMissing(path.join(outDir, 'auto.input'), path.join(store, 'coverage-auto.input'));
  };
  saveSessions(); // parties laissées dans out/ par une exécution précédente
  if (fs.existsSync(path.join(cfg.profilesDir, sha))) {
    copyGlobIfMissing(path.join(cfg.profilesDir, sha), '*.cov', outDir);
    copyGlobIfMissing(path.join(cfg.profilesDir, sha), '*.prof', outDir);
    let jeu = sha;
    try {
      jeu = fs.readFileSync(path.join(cfg.profilesDir, sha, 'JEU.txt'), 'utf8').trim() || sha;
    } catch { /* pas de JEU.txt */ }
    log(`[i] Profils fournis utilisés : ${jeu}`);
  }
  for (const f of [...glob(store, 'coverage-*.cov'), ...glob(store, 'romdata-*.prof')]) {
    fs.copyFileSync(f, path.join(outDir, path.basename(f)));
  }
  const sessions = glob(store, 'coverage-*.cov').length;
  if (sessions > 0) log(`[i] ${sessions} partie(s) déjà enregistrée(s) réutilisée(s)`);

  /* a) Projet interpréteur seul et outils PC */
  const packArgs = cfg.pack ? ['--meta-pack'] : [];
  let r = await runTool(GBRECOMP, [romPath, '--target', 'meta', '--meta-compiler', cfg.compiler, '--output-prefix', prefix,
    '--sd-name', sdName, ...packArgs, '-o', outDir]);
  if (r.code !== 0) { log(r.out + r.err); return false; }
  r = await runTool('cmake', ['-G', 'Ninja', '-S', path.join(outDir, 'host'), '-B', path.join(outDir, 'host', 'build')]);
  if (r.code !== 0) { log(r.out + r.err); return false; }
  r = await runTool('ninja', ['-C', path.join(outDir, 'host', 'build')]);
  if (r.code !== 0) { log(r.out + r.err); return false; }

  /* a2) REPLAY : rejoue les parties à la main enregistrées (utile après une
   *     mise à jour du runtime ou pour ajouter le profil de lectures SD). */
  if (cfg.replay) {
    for (const inp of glob(store, 'coverage-*.input')) {
      const id = path.basename(inp, '.input').replace(/^coverage-/, '');
      if (/^auto/.test(id)) continue;
      const lines = fs.readFileSync(inp, 'utf8').trimEnd().split('\n');
      const lastFrame = lines.length ? parseInt(lines[lines.length - 1], 10) + 120 : 120;
      log(`[i] Partie ${id} rejouée (${lastFrame} frames)`);
      await runTool(hostBin(`${prefix}_profile`), ['--interp', '--frames', String(lastFrame),
        '--input', inp, '--coverage', path.join(store, `coverage-${id}.cov`),
        '--rom-profile-out', path.join(store, `romdata-${id}.prof`)]);
      copyIfMissing(path.join(store, `coverage-${id}.cov`), path.join(outDir, `coverage-${id}.cov`));
      copyIfMissing(path.join(store, `romdata-${id}.prof`), path.join(outDir, `romdata-${id}.prof`));
    }
  }

  /* b) Partie interactive : enregistre une couverture de plus */
  if (cfg.play) {
    const stamp = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 15);
    log('[!] Partie d\'enregistrement. Flèches | X=A | Z=B | Entrée=Start | Retour=Select | Échap=Quitter');
    await runTool(hostBin(`${prefix}_play`), ['--interp',
      '--coverage', path.join(outDir, `coverage-${stamp}.cov`),
      '--rom-profile-out', path.join(outDir, `romdata-${stamp}.prof`),
      '--record-input', path.join(outDir, `coverage-${stamp}.input`)], { stdio: 'inherit' });
    saveSessions();
  }

  /* b2) Parties automatiques, sans fenêtre ni son, en parallèle : Start de
   *     temps en temps, directions/A/B au hasard, une graine par partie.
   *     Déjà enregistrées pour cette ROM : réutilisées (--auto-redo pour
   *     les refaire). */
  const haveAuto = glob(store, 'coverage-auto*.cov').length > 0
    || glob(path.join(cfg.profilesDir, sha), 'coverage-auto*.cov').length > 0;
  if (cfg.auto && !cfg.autoRedo && haveAuto) {
    log('[i] Parties automatiques déjà enregistrées : réutilisées');
  } else if (cfg.auto) {
    log(`[i] Parties automatiques : ${cfg.autoSeeds} en parallèle`);
    for (const f of [...glob(store, 'coverage-auto*'), ...glob(store, 'romdata-auto*'),
      ...glob(outDir, 'coverage-auto*'), ...glob(outDir, 'romdata-auto*')]) {
      fs.rmSync(f, { force: true });
    }
    fs.rmSync(path.join(outDir, 'auto.input'), { force: true });
    const runs = [];
    for (let seed = 1; seed <= cfg.autoSeeds; seed++) {
      const frames = seed === 1 ? cfg.autoLongFrames : cfg.autoFrames;
      const input = path.join(outDir, `coverage-auto${seed}.input`);
      fs.writeFileSync(input, autoInput(seed, frames));
      runs.push(runTool(hostBin(`${prefix}_profile`), ['--interp', '--frames', String(frames),
        '--max-seconds', String(cfg.autoSeconds), '--input', input,
        '--coverage', path.join(outDir, `coverage-auto${seed}.cov`),
        '--rom-profile-out', path.join(outDir, `romdata-auto${seed}.prof`)],
      { timeoutMs: (cfg.autoSeconds + 30) * 1000 }));
    }
    await Promise.all(runs);
    saveSessions();
  }

  /* b3) Jeux lus depuis la carte SD ou compressés (--meta-pack) : il faut
   *     savoir quelles données de la ROM sont lues pour choisir ce qui
   *     reste brut en flash.  Parties réutilisées sans ce profil
   *     (enregistrées avec un firmware ordinaire) : rejouées une fois pour
   *     le mesurer. */
  if (glob(path.join(outDir, 'host'), '*_romfull.c').length > 0
      && glob(outDir, 'romdata-*.prof').length === 0) {
    const replays = [];
    for (const inp of [...glob(store, 'coverage-*.input'),
      ...glob(path.join(cfg.profilesDir, sha), 'coverage-*.input')]) {
      const id = path.basename(inp, '.input').replace(/^coverage-/, '');
      if (fs.existsSync(path.join(outDir, `romdata-${id}.prof`))) continue;
      const lines = fs.readFileSync(inp, 'utf8').trimEnd().split('\n');
      const lastFrame = lines.length ? parseInt(lines[lines.length - 1], 10) + 120 : 120;
      replays.push(runTool(hostBin(`${prefix}_profile`), ['--interp', '--frames', String(lastFrame),
        '--max-seconds', String(cfg.autoSeconds), '--input', inp,
        '--rom-profile-out', path.join(outDir, `romdata-${id}.prof`)],
      { timeoutMs: (cfg.autoSeconds + 30) * 1000 }));
    }
    await Promise.all(replays);
    if (glob(outDir, 'romdata-*.prof').length > 0) {
      log('[i] Profil des lectures de ROM mesuré en rejouant les parties enregistrées');
      saveSessions();
    }
  }

  /* c) Régénération avec toutes les couvertures enregistrées (+ analyse
   *    statique) */
  log('[i] Analyse et génération du firmware');
  const covArgs = [];
  for (const f of glob(outDir, 'coverage-*.cov')) covArgs.push('--coverage', f);
  for (const f of glob(outDir, 'romdata-*.prof')) covArgs.push('--rom-profile', f);
  const staticArgs = cfg.staticAnalysis ? ['--meta-static'] : [];
  if (covArgs.length === 0 && staticArgs.length === 0) {
    log('[!] Aucune couverture : firmware 100 % interpréteur (plus lent).');
  }

  /* d) Firmware, en remplissant la flash au maximum.  Le code est trié du
   *    plus exécuté au moins exécuté ; --meta-bytes-per-instruction règle
   *    combien en garder.  Recherche dichotomique de la plus petite valeur
   *    (= le plus de code) qui tient encore dans les 240 Kio de la META.
   *    Codes retour : 0 ok, 1 flash pleine, 2 erreur. */
  let genLog = '';
  let buildLog = '';
  const buildWith = async (bytesPerInstr) => {
    const g = await runTool(GBRECOMP, [romPath, '--target', 'meta', '--meta-compiler', cfg.compiler, '--output-prefix', prefix,
      '--sd-name', sdName, ...covArgs, ...staticArgs, ...packArgs,
      '--meta-bytes-per-instruction', String(bytesPerInstr), '-o', outDir]);
    if (g.code !== 0) {
      genLog = g.out + g.err;
      return 2;
    }
    genLog = g.out;
    if (cfg.emu) {
      /* L'émulateur n'exécute pas le code depuis la SRAM ni la carte SD. */
      const ini = path.join(outDir, 'platformio.ini');
      let txt = fs.readFileSync(ini, 'utf8');
      if (!txt.includes('GBL_PPU_ASM=0')) {
        txt = txt.replace('build_flags =',
          'build_flags =\n    -DGBL_PPU_ASM=0\n    -DGBL_CORE_ASM=0\n    -DGBL_PPU_RAMFUNC=0', 1);
        fs.writeFileSync(ini, txt);
      }
    }
    const p = await runTool('pio', ['run', '-d', outDir],
      { env: { ...process.env, FIRMWARE_COMPILER: cfg.compiler } });
    buildLog = p.out + p.err;
    if (p.code === 0) return 0;
    return /overflowed|greater than maximum allowed/.test(buildLog) ? 1 : 2;
  };
  let lo = 8;
  let hi = 64;
  if (staticArgs.length === 0 && covArgs.length === 0) lo = 64;
  let best = 0;
  while (lo <= hi) {
    const mid = Math.floor((lo + hi) / 2);
    const rc = await buildWith(mid);
    if (rc === 0) {
      best = mid;
      hi = mid - 1;
    } else if (rc === 1) {
      lo = mid + 1;
    } else {
      log(buildLog.split('\n').slice(-20).join('\n'));
      if (genLog) log(genLog);
      return false;
    }
  }
  if (best === 0) {
    log('[x] Même sans code compilé, le firmware ne tient pas en flash.');
    return false;
  }
  /* Le dernier essai n'est pas forcément le meilleur : reconstruire celui-ci. */
  if (await buildWith(best) !== 0) return false;
  for (const line of genLog.split('\n')) {
    if (/compiled:|interpreter only:|wait loops:|ROM streaming:|ROM packed:|--meta-pack:/.test(line)) log(line.trim());
  }
  for (const line of buildLog.split('\n')) {
    if (/RAM:|Flash:/.test(line)) log(line.trim());
  }

  fs.copyFileSync(path.join(outDir, '.pio', 'build', 'meta', 'firmware.bin'),
    path.join(gameDir, `${sdName}.BIN`));

  /* Icônes du loader (PNG libretro -> BMP 24 bits), sans bloquer le jeu. */
  if (!await fetchIcons(gameDir, gameName, path.basename(romPath), gbc, cfg, log)) {
    log('[i] (jeu conservé sans icône)');
  }

  /* Sauvegarde (cartouche à pile) ou grande RAM de cartouche : un fichier
   * .SAV de la taille de cette RAM.  Jamais écrasé : il contient la
   * progression. */
  const def = gblDefines(outDir);
  if (def.saveName && (def.battery || def.eram > 2048)) {
    const sav = path.join(gameDir, def.saveName);
    if (!fs.existsSync(sav)) fs.writeFileSync(sav, Buffer.alloc(def.eram, 0xff));
    log(`[i] Sauvegarde : ${def.saveName} (déjà sur la carte : ne plus y toucher)`);
  }
  /* États sauvegardés : un fichier de stateSlots emplacements, créé une fois
   * (jamais écrasé) ; sans lui, le menu n'offre pas Sauver/Charger l'état. */
  if (def.stateName && def.stateSlots && def.stateSectors) {
    const sta = path.join(gameDir, def.stateName);
    const bytes = def.stateSlots * def.stateSectors * 512;
    if (!fs.existsSync(sta) || fs.statSync(sta).size < bytes) fs.writeFileSync(sta, Buffer.alloc(bytes, 0));
    log(`[i] États : ${def.stateName} (${def.stateSlots} emplacements, ${Math.round(bytes / 1024)} Kio)`);
  }
  /* ROM trop grosse pour la flash : le firmware la lit sur la carte SD. */
  if (def.streamName) {
    fs.copyFileSync(romPath, path.join(gameDir, def.streamName));
    log(`[i] ROM lue depuis la carte SD : ${def.streamName} (dans le dossier du jeu)`);
  }

  /* État pour les lancements suivants : version, ROM exacte, fichiers. */
  game.version = curVersion;
  game.pack = cfg.pack;
  game.compiler = cfg.compiler;
  writeState(gameDir, game);
  const fwVer = firmwareVersion(path.join(gameDir, `${sdName}.BIN`)) || curVersion;
  log(`[i] Firmware produit par m0recomp v${fwVer}`);
  return true;
}

/* -------------------------------------------------- interface plein écran */

async function loadInk() {
  try {
    const [React, ink] = await Promise.all([import('react'), import('ink')]);
    return { React, ...ink };
  } catch { /* pas installé */ }
  if (!process.stdin.isTTY || !process.stdout.isTTY) return null;
  process.stdout.write('L\'interface plein écran a besoin de ink+react (tools/meta-compiler).\n');
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  const ok = (await rl.question('Les installer maintenant avec npm ? [O/n] ')).trim();
  rl.close();
  if (/^n/i.test(ok)) return null;
  const npm = IS_WIN ? 'npm.cmd' : 'npm';
  const r = spawnSync(npm, ['install', '--no-audit', '--no-fund'],
    { cwd: SCRIPT_DIR, stdio: 'inherit', shell: IS_WIN });
  if (r.status !== 0) return null;
  try {
    const [React, ink] = await Promise.all([import('react'), import('ink')]);
    return { React, ...ink };
  } catch {
    return null;
  }
}

/* Icônes et couleurs des états d'un jeu dans les listes. */
const GAME_ICONS = { idle: '·', run: '▶', ok: '✔', fail: '✘', skip: '=', kept: '»' };
const GAME_COLORS = { run: 'cyan', ok: 'green', fail: 'red', skip: 'gray', kept: 'yellow' };

function fmtElapsed(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

/* L'application ink : écran de sélection, confirmation, puis tableau de bord
 * (liste des jeux à gauche, compteurs et journal d'activité à droite). */
function makeApp(mods, store, onSelected, onDone) {
  const { React, Box, Text, useInput } = mods;
  const h = React.createElement;

  return function App() {
    const [, setTick] = React.useState(0);
    React.useEffect(() => {
      const t = setInterval(() => setTick((x) => x + 1), 200);
      return () => clearInterval(t);
    }, []);
    const games = store.games;
    const [mode, setMode] = React.useState('select'); // select | confirm | run
    const [cursor, setCursor] = React.useState(0);
    const [checked, setChecked] = React.useState(() => {
      const s = new Set();
      games.forEach((g, i) => {
        if (g.todo !== 0) s.add(i);
      });
      return s;
    });
    const rows = Math.max(10, (process.stdout.rows || 24) - 1);
    const selectRows = Math.max(3, rows - 6); // lignes de jeux affichables
    const dashRows = Math.max(3, rows - 9);

    useInput((input, key) => {
      if (key.ctrl && input === 'c') {
        killActiveChildren();
        process.exit(130);
      }
      if (mode === 'select') {
        if (key.upArrow) setCursor((c) => Math.max(0, c - 1));
        else if (key.downArrow) setCursor((c) => Math.min(games.length - 1, c + 1));
        else if (input === ' ') {
          setChecked((prev) => {
            const n = new Set(prev);
            if (n.has(cursor)) n.delete(cursor); else n.add(cursor);
            return n;
          });
        } else if (input === 'a') {
          setChecked((prev) => (prev.size === games.length
            ? new Set() : new Set(games.map((_, i) => i))));
        } else if (key.return) {
          const sel = [...checked];
          if (sel.some((i) => games[i].todo === 2)) setMode('confirm');
          else {
            setMode('run');
            store.startedAt = Date.now();
            onSelected(sel, []);
          }
        } else if (input === 'q') process.exit(0);
      } else if (mode === 'confirm') {
        if (key.return || input === 'o' || input === 'O' || input === 'y') {
          const sel = [...checked];
          setMode('run');
          store.startedAt = Date.now();
          onSelected(sel, []);
        } else if (input === 'n' || input === 'N') {
          const sel = [...checked].filter((i) => games[i].todo !== 2);
          const kept = [...checked].filter((i) => games[i].todo === 2);
          setChecked(new Set(sel));
          setMode('run');
          store.startedAt = Date.now();
          onSelected(sel, kept);
        } else if (key.escape) setMode('select');
      } else if (store.finished) {
        if (key.return || input === 'q') onDone();
      } else if (input === 'q') {
        store.aborted = true;
      }
    });

    const paneRows = mode === 'run' ? dashRows : selectRows;
    const start = Math.max(0, Math.min(mode === 'select' ? cursor : store.follow,
      games.length - paneRows));
    const visible = games.slice(start, start + paneRows);
    const gameRow = (g, i, selected) => {
      const mark = mode === 'select' ? (selected ? '[x] ' : '[ ] ') : '    ';
      const phase = g.state === 'run' ? (g.phase || '…') : g.tag;
      const color = g.state ? GAME_COLORS[g.state] : (g.todo === 2 ? 'yellow' : undefined);
      const line = `${mark}${GAME_ICONS[g.state] ?? '·'} ${g.dir.padEnd(24, ' ').slice(0, 32)} ${phase}`;
      return h(Box, { key: `${i}-${g.dir}` }, h(Text, {
        inverse: mode === 'select' && i === cursor,
        color,
        wrap: 'truncate-end',
      }, ` ${line}`));
    };

    if (mode === 'select' || mode === 'confirm') {
      const outdated = [...checked].filter((i) => games[i].todo === 2);
      return h(Box, { flexDirection: 'column' }, [
        h(Text, { key: 't' }, ` m0recomp v${store.curVersion} · ${store.cfg.romDir} → ${store.cfg.destDir}`),
        h(Box, { key: 'l', flexDirection: 'column', borderStyle: 'round' }, [
          h(Text, { key: 'h', bold: true }, ` Jeux (${games.length}) — espace : cocher, a : tout, entrée : valider`),
          ...visible.map((g, k) => gameRow(g, start + k, checked.has(start + k))),
        ]),
        mode === 'confirm'
          ? h(Box, { flexDirection: 'column', borderStyle: 'round' }, [
            h(Text, { key: 'h', bold: true }, ' Firmwares faits par une autre version (ou de version inconnue) :'),
            ...outdated.map((i) => h(Text, { key: i }, `  • ${games[i].dir}  [${games[i].tag}]`)),
            h(Text, { key: 'q' }, ' Reconstruire avec m0recomp v'
              + `${store.curVersion} ? [entrée/o = oui, n = conserver, échap = retour]`),
          ])
          : h(Text, { key: 'f', dimColor: true }, ' q : quitter · Ctrl-C : arrêter'),
      ]);
    }

    /* Tableau de bord. */
    const c = { idle: 0, run: 0, ok: 0, fail: 0, skip: 0, kept: 0 };
    for (const g of games) c[g.state] = (c[g.state] ?? 0) + 1;
    const selectedCount = store.sel.size;
    const logH = Math.max(3, rows - dashRows - 7);
    const tail = store.logLines.slice(-logH);
    return h(Box, { flexDirection: 'column', height: rows }, [
      h(Text, { key: 't' }, ` m0recomp v${store.curVersion} · ${store.cfg.romDir} → ${store.cfg.destDir}`
        + ` · ${fmtElapsed(Date.now() - store.startedAt)}`
        + (store.aborted ? ' · arrêt demandé (les jeux lancés finissent)' : '')
        + (store.finished ? ' · TERMINÉ' : '')),
      h(Box, { key: 'mid', flexDirection: 'row', flexGrow: 1 }, [
        h(Box, { key: 'left', flexDirection: 'column', width: '55%', borderStyle: 'round', height: dashRows + 2 }, [
          h(Text, { key: 'h', bold: true }, ` Jeux (${selectedCount} sélectionné(s))`),
          ...visible.map((g, k) => gameRow(g, start + k, store.sel.has(start + k))),
        ]),
        h(Box, { key: 'right', flexDirection: 'column', width: '45%', gap: 0 }, [
          h(Box, { key: 'stats', flexDirection: 'column', borderStyle: 'round' }, [
            h(Text, { key: 'h', bold: true }, ' État global'),
            h(Text, { key: 'run' }, `  en cours      ${c.run}/${selectedCount}`),
            h(Text, { key: 'ok', color: 'green' }, `  réussis       ${c.ok}`),
            h(Text, { key: 'fail', color: c.fail > 0 ? 'red' : undefined }, `  échecs        ${c.fail}`),
            h(Text, { key: 'skip', color: 'gray' }, `  déjà à jour   ${c.skip}`),
            h(Text, { key: 'kept', color: 'yellow' }, `  conservés     ${c.kept}`),
            h(Text, { key: 'idle', color: 'yellow' }, `  en attente    ${c.idle}`),
          ]),
          h(Box, { key: 'act', flexDirection: 'column', borderStyle: 'round', flexGrow: 1, overflow: 'hidden' }, [
            h(Text, { key: 'h', bold: true }, ' Activité'),
            ...tail.map((l, i) => h(Text, { key: i, wrap: 'truncate-end', dimColor: true }, ` ${l}`)),
          ]),
        ]),
      ]),
      h(Text, { key: 'f', dimColor: true }, store.finished
        ? ' terminé — entrée/q : quitter'
        : ' q : ne plus lancer de nouveaux jeux · Ctrl-C : tout arrêter'),
    ]);
  };
}

/* ------------------------------------------------ liste numérotée de secours */

async function basicPick(games, cfg, curVersion) {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  console.log(`\nROMs de '${cfg.romDir}' (m0recomp v${curVersion}) :`);
  games.forEach((g, i) => {
    const extra = g.dir !== g.base ? `  <== ${g.base}` : '';
    console.log(` ${String(i + 1).padStart(2)}. ${g.dir.padEnd(28)} [${g.tag}]${extra}`);
  });
  const todoCount = games.filter((g) => g.todo !== 0).length;
  const ans = (await rl.question(
    `Jeux à traiter [Entrée = ce qui a à faire (${todoCount}), a = tout, ex: 1,3-5, q = rien] : `,
  )).trim();
  let sel = [];
  if (/^q/i.test(ans)) {
    rl.close();
    process.exit(0);
  } else if (ans === '' || /^a$/i.test(ans)) {
    sel = games.map((_, i) => i).filter((i) => (ans === '' ? games[i].todo !== 0 : true));
  } else {
    for (const part of ans.split(',')) {
      const partTrim = part.trim();
      const range = /^(\d+)-(\d+)$/.exec(partTrim);
      if (range) {
        for (let j = +range[1]; j <= +range[2]; j++) {
          if (j >= 1 && j <= games.length) sel.push(j - 1);
        }
      } else if (/^\d+$/.test(partTrim) && +partTrim >= 1 && +partTrim <= games.length) {
        sel.push(+partTrim - 1);
      }
    }
    sel = [...new Set(sel)];
  }
  /* Proposer la mise à jour des firmwares faits par une autre version. */
  let kept = [];
  const outdated = sel.filter((i) => games[i].todo === 2);
  if (outdated.length > 0) {
    console.log('\nFirmware(s) fait(s) par une autre version (ou de version inconnue) :');
    for (const i of outdated) console.log(`  - ${games[i].dir}  [${games[i].tag}]`);
    const o = (await rl.question(`Reconstruire avec m0recomp v${curVersion} ? [O/n] `)).trim();
    if (/^n/i.test(o)) {
      kept = outdated;
      sel = sel.filter((i) => games[i].todo !== 2);
    }
  }
  rl.close();
  return { sel, kept };
}

/* --------------------------------------------------------------------- main */

async function main() {
  const cfg = parseArgs(process.argv.slice(2));
  if (!fs.existsSync(GBRECOMP)) {
    fail(`m0recomp introuvable : compile d'abord l'outil (cmake -G Ninja -B build . && ninja -C build).`);
  }
  fs.mkdirSync(cfg.destDir, { recursive: true });
  const curVersion = await gbrecompVersion();

  const roms = fs.existsSync(cfg.romDir)
    ? fs.readdirSync(cfg.romDir)
      .filter((f) => /\.(gb|gbc)$/i.test(f))
      .sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }))
      .map((f) => path.join(cfg.romDir, f))
    : [];
  if (roms.length === 0) {
    console.log(`Aucun fichier .gb ou .gbc trouvé dans '${cfg.romDir}'.`);
    process.exit(0);
  }

  /* Inventaire : nom propre, préfixe C, sha1, état. */
  const games = [];
  const usedDirs = [];
  const usedPrefixes = [];
  let nDup = 0;
  for (const romPath of roms) {
    const sha = await sha1File(romPath);
    const twin = games.find((g) => g.sha === sha);
    if (twin) {
      console.log(`[-] Doublon (même sha1 que ${twin.dir}) : ${path.basename(romPath)}`);
      nDup++;
      continue;
    }
    let clean = sdSafe(cleanName(path.basename(romPath)));
    if (!clean) clean = sdSafe(path.basename(romPath));
    let dir = clean;
    for (let k = 2; usedDirs.includes(dir); k++) dir = `${clean} ${k}`;
    let prefix = clean.replace(/[^a-zA-Z0-9_]/g, '_').replace(/_+$/, '') || 'game';
    if (/^[0-9]/.test(prefix)) prefix = `g${prefix}`;
    while (usedPrefixes.includes(prefix)) prefix = `${prefix}_2`;
    const game = {
      romPath, sha, dir, prefix, base: path.basename(romPath),
      tag: '', todo: 1, version: '', state: null, phase: '',
    };
    usedDirs.push(dir);
    usedPrefixes.push(prefix);
    assess(game, cfg, curVersion);
    games.push(game);
  }
  if (games.length === 0) {
    console.log(`Rien à faire (${nDup} doublon(s) ignoré(s)).`);
    process.exit(0);
  }

  /* Choix des jeux : plein écran (ink), liste numérotée de secours, ou lot. */
  const interactive = Boolean(process.stdin.isTTY && process.stdout.isTTY) && !cfg.all;
  let sel = [];
  let kept = [];
  let mods = null;
  let store = null;
  let donePromise = Promise.resolve();

  if (interactive && !cfg.play) {
    mods = await loadInk();
  }
  if (mods) {
    store = {
      games, cfg, curVersion,
      startedAt: Date.now(), logLines: [],
      aborted: false, finished: false,
      sel: new Set(), follow: 0,
    };
    let resolveSel;
    let resolveDone;
    const selPromise = new Promise((r) => { resolveSel = r; });
    donePromise = new Promise((r) => { resolveDone = r; });
    const App = makeApp(mods, store,
      (s, k) => resolveSel([s, k]),
      () => resolveDone());
    instance = mods.render(mods.React.createElement(App), { fullscreen: true, exitOnCtrlC: false });
    [sel, kept] = await selPromise;
    store.sel = new Set(sel);
    for (const i of kept) games[i].state = 'kept';
  } else if (interactive) {
    ({ sel, kept } = await basicPick(games, cfg, curVersion));
  } else {
    /* Non interactif : ce qui a à faire, sauf mise à jour de version
     * (--update pour l'autoriser) et déjà-à-jour (--force pour refaire). */
    games.forEach((g, i) => {
      if (g.todo === 1) sel.push(i);
      else if (g.todo === 2) (cfg.update ? sel : kept).push(i);
      else sel.push(i); // déjà à jour : complétion des icônes sans recompiler
    });
  }

  const jobs = Math.max(1, cfg.jobs);
  const dashboard = Boolean(store);
  if (!dashboard) {
    console.log(`\n${sel.length} jeu(x) à traiter, ${jobs} en parallèle, `
      + `${cfg.autoSeeds} parties automatiques par ROM (${os.cpus().length} cœurs)`);
    if (kept.length > 0) {
      console.log('Firmware(s) conservé(s) sans reconstruction :');
      for (const i of kept) console.log(`  - ${games[i].dir}  [${games[i].tag}]`);
      if (!interactive) console.log(`  (--update pour les reconstruire avec v${curVersion})`);
    }
  }

  const counts = { ok: 0, fail: 0, skip: 0 };
  const failed = [];
  const pushLog = (line) => {
    store.logLines.push(line.length > 72 ? `${line.slice(0, 69)}…` : line);
  };

  const processOne = async (i) => {
    const g = games[i];
    const gameDir = path.join(cfg.destDir, SYS_DIR, g.dir);
    if (g.todo === 0 && !cfg.force) {
      /* Déjà à jour : on ne recompile pas ; icônes manquantes complétées. */
      const withLog = (line) => {
        if (dashboard) pushLog(`= ${g.dir} : ${line}`);
        else console.log(`[i] ${g.dir} : ${line}`);
      };
      const iconsOk = await fetchIcons(gameDir, g.dir, g.base, /\.gbc$/i.test(g.base), cfg, withLog);
      const line = `[=] ${g.dir} : déjà à jour (v${curVersion}), ignoré${iconsOk ? '' : ' (icône absente)'}`;
      g.state = 'skip';
      if (dashboard) {
        g.phase = 'déjà à jour';
        pushLog(`${line.replace(/^\[=\] /, '')}`);
      } else console.log(line);
      counts.skip++;
      return;
    }
    /* Journal détaillé de chaque jeu dans out/<jeu>/compile.log ; affiché en
     * direct quand les ROMs ne tournent pas en parallèle. */
    const outFile = path.join(ROOT, 'out', g.prefix);
    fs.mkdirSync(outFile, { recursive: true });
    const logFile = path.join(outFile, 'compile.log');
    const lines = [`===== ${g.dir} (${g.base}) =====`];
    if (!dashboard && jobs === 1) console.log(lines[0]);
    const log = (line) => {
      lines.push(line);
      if (dashboard) {
        pushLog(`▶ ${g.dir} : ${line}`);
        if (g.state === 'run') g.phase = line.replace(/^\[[i!x]\] /, '').slice(0, 40);
      } else if (jobs === 1) console.log(line);
    };
    g.state = 'run';
    g.phase = 'démarrage';
    if (dashboard) store.follow = i;
    const ok = await buildGame(g, cfg, curVersion, log);
    fs.appendFileSync(logFile, `${lines.join('\n')}\n`);
    g.state = ok ? 'ok' : 'fail';
    g.phase = ok ? 'terminé' : 'échec';
    const tail = ok
      ? `[+] Terminé : ${path.join(cfg.destDir, g.dir, `${sdNameOf(g.dir, g.sha)}.BIN`)}`
      : `[x] Échec pour ${g.dir} (voir ${path.relative(ROOT, logFile)})`;
    if (ok) counts.ok++; else { counts.fail++; failed.push(g.dir); }
    if (dashboard) pushLog(tail.replace(/^\[.\] /, `${ok ? '✔' : '✘'} ${g.dir} : `));
    else console.log(tail);
  };

  /* Pool de JOBS traitements en parallèle ; q = ne plus en lancer. */
  let next = 0;
  const workers = Array.from({ length: Math.min(jobs, sel.length) }, async () => {
    while (next < sel.length && !(store && store.aborted)) {
      const i = sel[next++];
      await processOne(i);
    }
  });
  await Promise.all(workers);
  if (store) {
    store.follow = sel.length > 0 ? sel[sel.length - 1] : 0;
    store.finished = true;
    await donePromise;
    instance.unmount();
  }

  const summary = [
    `Réussis : ${counts.ok}   Échecs : ${counts.fail}   Déjà à jour : ${counts.skip}   Conservés : ${kept.length}`,
    `Jeux dans : ${path.join(cfg.destDir, '<Jeu>')}  (à copier tels quels à la racine de la carte SD)`,
  ];
  if (failed.length > 0) summary.splice(1, 0, `  échec : ${failed.join(', ')}`);
  for (const line of summary) console.log(line);
  process.exit(counts.fail === 0 ? 0 : 1);
}

main().catch((e) => {
  if (instance) {
    try {
      instance.unmount();
    } catch { /* déjà sorti */ }
  }
  console.error(e);
  process.exit(1);
});
