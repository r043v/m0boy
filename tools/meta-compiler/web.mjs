#!/usr/bin/env node
/*
 * Version web du convertisseur : on donne un .gb/.gbc dans le navigateur,
 * on reçoit un .zip contenant le répertoire entier du jeu pour la META
 * (<Jeu>/<Jeu>.bin, ICON.BMP, TITLESCREEN.BMP, .SAV, .GB le cas échéant).
 *
 * Le gros du travail (recompilation m0recomp + cross-compilation PIO) ne
 * peut pas tourner dans un navigateur : ce serveur pilote l'existant
 * (compile.mjs) sur une copie temporaire de la ROM et renvoie le résultat
 * zippé.  Tout se passe sur la machine locale ; aucune dépendance npm.
 *
 * Usage :
 *   node tools/meta-compiler/web.mjs [port]
 *   (défaut 8642, GB_WEB_PORT pour surcharger ; écoute sur 127.0.0.1)
 * Prérequis : les mêmes que compile.mjs (m0recomp compilé, pio).
 *
 * API :
 *   GET  /                    la page (glisser un .gb)
 *   GET  /api/health          { m0recomp, pio }
 *   POST /api/convert?name=X.gb   corps = ROM brute -> { id }
 *   GET  /api/job/<id>        { state: queued|running|done|error, lines, game }
 *   GET  /api/job/<id>/zip    le zip du répertoire de jeu
 */

import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, '..', '..');
const EXE = process.platform === 'win32' ? '.exe' : '';
const GBRECOMP = path.join(ROOT, 'build', 'bin', `m0recomp${EXE}`);
const PORT = Number(process.env.GB_WEB_PORT) || (Number(process.argv[2]) || 8642);
const MAX_ROM = 64 * 1024 * 1024;
const WORK = path.join(os.tmpdir(), 'gb-meta-web');

/* ------------------------------------------------------------------ outils */

function log(...a) { console.log(new Date().toTimeString().slice(0, 8), ...a); }

/* Zip STORE (sans compression) : les .GB/.bin sont déjà des données brutes
 * compactes, et ça reste trivial et sans dépendance.  Noms UTF-8 si besoin. */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function dosTime(d) {
  const time = ((d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1)) & 0xffff;
  const date = (((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate()) & 0xffff;
  return [time, date];
}
/* chemins relatifs du dossier jeu -> Buffer zip (entrées "<Jeu>/<fichier>") */
function zipStore(entries) {
  const parts = [];
  const central = [];
  let offset = 0;
  for (const [name, data, mtime, isDir] of entries) {
    const nameBuf = Buffer.from(name, 'utf8');
    const utf8 = /[^\x20-\x7e]/.test(name) ? 0x0800 : 0;
    const [time, date] = dosTime(mtime);
    const crc = crc32(data);
    /* attributs unix comme les releases (drwxr-xr-x / rw-r--r--) */
    const extAttr = isDir ? 0x41ed0000 : 0x81a40000;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(utf8, 6);
    local.writeUInt16LE(0, 8); /* store */
    local.writeUInt16LE(time, 10);
    local.writeUInt16LE(date, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    parts.push(local, nameBuf, data);
    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(0x02014b50, 0);
    cen.writeUInt16LE(20, 4);
    cen.writeUInt16LE(20, 6);
    cen.writeUInt16LE(utf8, 8);
    cen.writeUInt16LE(0, 10);
    cen.writeUInt16LE(time, 12);
    cen.writeUInt16LE(date, 14);
    cen.writeUInt32LE(crc, 16);
    cen.writeUInt32LE(data.length, 20);
    cen.writeUInt32LE(data.length, 24);
    cen.writeUInt16LE(nameBuf.length, 28);
    cen.writeUInt32LE(offset, 42);
    cen.writeUInt32LE(extAttr, 38);
    /* le nom suit l'en-tête de 46 octets : concat obligatoire (copy dans un
     * tampon trop petit est ignorée silencieusement) */
    central.push(Buffer.concat([cen, nameBuf]));
    offset += local.length + nameBuf.length + data.length;
  }
  const cenBuf = Buffer.concat(central);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(cenBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);
  return Buffer.concat([...parts, cenBuf, eocd]);
}

function listFilesRec(dir, base) {
  const out = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    /* tout le répertoire part, y compris .gbrmeta.json : c'est le format
     * de release tel que compile.mjs le produit (voir roms/meta/*.zip) */
    const full = path.join(dir, e.name);
    const rel = base ? `${base}/${e.name}` : e.name;
    if (e.isDirectory()) out.push(...listFilesRec(full, rel));
    else out.push([full, rel]);
  }
  return out;
}

function zipGameDir(gameDir, zipName) {
  const stat = fs.statSync(gameDir);
  const entries = [[`${zipName}/`, Buffer.alloc(0), stat.mtime, true]];
  for (const [full, rel] of listFilesRec(gameDir, '')) {
    entries.push([`${zipName}/${rel}`, fs.readFileSync(full), fs.statSync(full).mtime, false]);
  }
  return zipStore(entries);
}

/* --------------------------------------------------------------- travaux */

const jobs = new Map();
let queueTail = Promise.resolve();

function jobDir(id) { return path.join(WORK, id); }

function runJob(id) {
  const job = jobs.get(id);
  queueTail = queueTail.then(() => new Promise((resolve) => {
    if (job.state !== 'queued') { resolve(); return; }
    job.state = 'running';
    job.lines.push('[i] Conversion démarrée (m0recomp + pio, quelques minutes au premier build)…');
    const dir = jobDir(id);
    const child = spawn(process.execPath,
      [path.join(SCRIPT_DIR, 'compile.mjs'), path.join(dir, 'roms'), path.join(dir, 'meta'),
       '--all', '--no-auto', '--jobs', '1'],
      { cwd: ROOT, windowsHide: true });
    job.child = child;
    const feed = (chunk) => {
      for (const line of chunk.toString().split(/\r?\n/)) {
        if (line.trim()) job.lines.push(line);
      }
      if (job.lines.length > 400) job.lines.splice(0, job.lines.length - 400);
    };
    child.stdout.on('data', feed);
    child.stderr.on('data', feed);
    child.on('error', (err) => {
      job.state = 'error';
      job.lines.push(`[x] Lancement impossible : ${err.message}`);
      resolve();
    });
    child.on('close', (code) => {
      job.child = null;
      try {
        if (code !== 0) {
          job.state = 'error';
          job.lines.push(`[x] compile.mjs a échoué (code ${code}).`);
          resolve();
          return;
        }
        const meta = path.join(dir, 'meta');
        /* .cache (icônes) et autres dot-dirs ne sont pas des jeux ; les jeux
         * sont rangés par plateforme source (gb/), comme compile.sh */
        const sysDir = path.join(meta, 'gb');
        const gameName = fs.readdirSync(sysDir)
          .find((n) => !n.startsWith('.') && fs.statSync(path.join(sysDir, n)).isDirectory());
        if (!gameName) throw new Error('aucun répertoire de jeu produit');
        job.game = gameName;
        job.zipPath = path.join(dir, 'job.zip');
        fs.writeFileSync(job.zipPath, zipGameDir(path.join(sysDir, gameName), gameName));
        job.state = 'done';
        job.lines.push(`[i] Zip prêt : ${gameName}.zip`);
      } catch (err) {
        job.state = 'error';
        job.lines.push(`[x] ${err.message}`);
      }
      resolve();
    });
  }));
}

function newJob(name) {
  const id = Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
  const dir = jobDir(id);
  fs.mkdirSync(path.join(dir, 'roms'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'roms', name), Buffer.alloc(0));
  const job = { id, name, state: 'queued', lines: [], game: null, zipPath: null, child: null };
  jobs.set(id, job);
  log(`travail ${id} : ${name}`);
  runJob(id);
  return id;
}

/* travaux oubliés d'un précédent run (> 24 h) */
function cleanOldJobs() {
  if (!fs.existsSync(WORK)) return;
  const cut = Date.now() - 24 * 3600 * 1000;
  for (const e of fs.readdirSync(WORK)) {
    const p = path.join(WORK, e);
    try { if (fs.statSync(p).mtimeMs < cut) fs.rmSync(p, { recursive: true, force: true }); } catch { /* ignore */ }
  }
}

/* ----------------------------------------------------------------- page */

const PAGE = `<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>META — convertisseur .gb</title>
<style>
  html, body { margin: 0; min-height: 100%; background: #101418; color: #cfd8dc;
               font: 15px/1.5 system-ui, sans-serif; }
  main { max-width: 720px; margin: 0 auto; padding: 32px 16px; text-align: center; }
  h1 { color: #9ccc65; font-size: 22px; margin: 0 0 24px; }
  #drop {
    border: 2px dashed #37474f; border-radius: 14px; padding: 48px 24px;
    cursor: pointer; background: #161c24; transition: border-color .15s;
  }
  #drop.over { border-color: #9ccc65; background: #1b2430; }
  #drop .ic { font-size: 64px; line-height: 1; }
  #drop p { margin: 12px 0 0; color: #78909c; }
  #log {
    display: none; text-align: left; background: #0b0e12; border: 1px solid #263238;
    border-radius: 8px; padding: 12px; margin-top: 20px; max-height: 300px;
    overflow-y: auto; font: 12px/1.6 ui-monospace, monospace; color: #90a4ae;
    white-space: pre-wrap; word-break: break-word;
  }
  #log.ok { border-color: #2e7d32; }
  #log.err { border-color: #c62828; }
  #dl { display: none; margin: 20px auto 0; padding: 12px 24px; font-size: 16px;
        background: #1b5e20; color: #c8e6c9; border: 1px solid #2e7d32;
        border-radius: 8px; cursor: pointer; }
  #dl:hover { background: #2e7d32; }
  small { color: #546e7a; }
  #banner { display: none; margin: 0 0 20px; color: #ef9a9a; font-size: 14px; }
</style>
</head>
<body>
<main>
  <h1>Gamebuino META — convertisseur <small>.gb → zip</small></h1>
  <div id="banner"></div>
  <div id="drop">
    <div class="ic">📥</div>
    <div>Déposez une ROM <b>.gb</b> / <b>.gbc</b> ici<br>(ou cliquez pour la choisir)</div>
    <p>La conversion (recompilation + PIO) tourne sur cette machine ;
       vous recevrez le répertoire de jeu complet en .zip.</p>
  </div>
  <div id="log"></div>
  <button id="dl">⬇ Télécharger le .zip</button>
  <p><small>Serveur local 127.0.0.1 — aucun envoi sur le réseau.</small></p>
  <input id="file" type="file" accept=".gb,.gbc" hidden>
</main>
<script>
const drop = document.getElementById('drop');
const logEl = document.getElementById('log');
const dl = document.getElementById('dl');
const banner = document.getElementById('banner');
const fileInput = document.getElementById('file');
let jobId = null, lastState = null, timer = null;

fetch('/api/health').then(r => r.json()).then(h => {
  const miss = [];
  if (!h.m0recomp) miss.push('m0recomp compilé (ninja -C build)');
  if (!h.pio) miss.push('pio dans le PATH');
  if (miss.length) { banner.textContent = '⚠ Il manque : ' + miss.join(' + ');
                     banner.style.display = 'block'; }
}).catch(() => {});

function send(file) {
  if (!/\\.(gb|gbc)$/i.test(file.name)) { alert('Fichier .gb / .gbc attendu'); return; }
  logEl.className = ''; logEl.style.display = 'block';
  logEl.textContent = 'envoi de ' + file.name + ' (' + Math.round(file.size / 1024) + ' Kio)…';
  dl.style.display = 'none';
  fetch('/api/convert?name=' + encodeURIComponent(file.name),
        { method: 'POST', body: file })
    .then(r => { if (!r.ok) return r.text().then(t => { throw new Error(t); });
                 return r.json(); })
    .then(({ id }) => { jobId = id; lastState = null; poll(); })
    .catch(e => { logEl.className = 'err';
                  logEl.textContent = 'échec de l\\'envoi : ' + e.message; });
}

function poll() {
  clearInterval(timer);
  timer = setInterval(() => {
    if (!jobId) return;
    fetch('/api/job/' + jobId).then(r => r.json()).then(j => {
      logEl.textContent = j.lines.join('\\n');
      logEl.scrollTop = logEl.scrollHeight;
      if (j.state !== lastState) {
        lastState = j.state;
        if (j.state === 'done') {
          logEl.className = 'ok';
          dl.style.display = 'inline-block';
          dl.textContent = '⬇ Télécharger ' + j.game + '.zip';
          dl.onclick = () => { location.href = '/api/job/' + jobId + '/zip'; };
          location.href = '/api/job/' + jobId + '/zip'; /* téléchargement direct */
          clearInterval(timer);
        } else if (j.state === 'error') {
          logEl.className = 'err';
          clearInterval(timer);
        }
      }
    }).catch(() => {});
  }, 600);
}

drop.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => { if (fileInput.files[0]) send(fileInput.files[0]);
                                              fileInput.value = ''; });
drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', e => {
  e.preventDefault(); drop.classList.remove('over');
  const f = e.dataTransfer.files && e.dataTransfer.files[0];
  if (f) send(f);
});
</script>
</body>
</html>`;

/* ------------------------------------------------------------- serveur */

function health() {
  const pio = spawnSync('pio', ['--version'], { windowsHide: true });
  return { gbrecomp: fs.existsSync(GBRECOMP), pio: !pio.error };
}

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let n = 0;
    req.on('data', (c) => {
      n += c.length;
      if (n > limit) { reject(new Error('ROM trop grosse')); req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  try {
    if (req.method === 'GET' && (url.pathname === '/' || url.pathname === '/index.html')) {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(PAGE);
      return;
    }
    if (req.method === 'GET' && url.pathname === '/api/health') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(health()));
      return;
    }
    if (req.method === 'POST' && url.pathname === '/api/convert') {
      const raw = url.searchParams.get('name') || 'game.gb';
      const name = path.basename(decodeURIComponent(raw)).replace(/[\\/:*?"<>|]/g, '_');
      if (!/\.(gb|gbc)$/i.test(name)) {
        res.writeHead(400); res.end('fichier .gb / .gbc attendu'); return;
      }
      const body = await readBody(req, MAX_ROM);
      if (body.length < 0x150) {
        res.writeHead(400); res.end('fichier trop court pour une ROM Game Boy'); return;
      }
      const id = newJob(name);
      fs.writeFileSync(path.join(jobDir(id), 'roms', name), body);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ id }));
      return;
    }
    const m = url.pathname.match(/^\/api\/job\/([a-z0-9]+)(\/zip)?$/);
    if (req.method === 'GET' && m) {
      const job = jobs.get(m[1]);
      if (!job) { res.writeHead(404); res.end('travail inconnu'); return; }
      if (m[2]) {
        if (job.state !== 'done' || !job.zipPath) {
          res.writeHead(409); res.end('zip pas prêt'); return;
        }
        const zipName = (job.game || job.name).replace(/\.gb.*$/i, '') + '.zip';
        res.writeHead(200, {
          'Content-Type': 'application/zip',
          'Content-Disposition': `attachment; filename*=UTF-8''${encodeURIComponent(zipName)}`,
        });
        fs.createReadStream(job.zipPath).pipe(res);
        return;
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ state: job.state, lines: job.lines, game: job.game }));
      return;
    }
    res.writeHead(404); res.end();
  } catch (err) {
    res.writeHead(500); res.end(err.message);
  }
});

cleanOldJobs();
server.listen(PORT, '127.0.0.1', () => {
  const h = health();
  log(`convertisseur web : http://127.0.0.1:${PORT}/`);
  if (!h.m0recomp) log('[!] m0recomp absent : compile d\'abord (ninja -C build)');
  if (!h.pio) log('[!] pio absent de PATH : les conversions échoueront');
});
