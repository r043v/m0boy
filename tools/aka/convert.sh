#!/usr/bin/env bash
# Convertit une ou plusieurs ROMs en jeux pour la Gamebuino AKA : un .zip par jeu,
# a decompresser a la racine de la carte SD (le Launcher le liste ensuite).
#
#   tools/aka/convert.sh jeu.gg                  -> ./aka-out/jeu.zip
#   tools/aka/convert.sh jeu1.sms jeu2.gb dossier/   (fichiers et/ou dossiers de ROMs)
#   tools/aka/convert.sh -o ma_sortie -r jeu.gg   (-o : dossier de sortie, -r : rapide)
#
# Systemes pris en charge pour l'AKA : Game Boy/Color (.gb .gbc), SG-1000 (.sg),
# Game Gear (.gg), Master System (.sms), Mega Drive (.md .gen .smd ; experimental).
# Mega Drive : le profil natif vient d'une partie automatique (entrees aleatoires) rejouee sur PC.
# Prerequis : CMake, Ninja, un compilateur C/C++, zlib (dev), Python 3, PlatformIO.
# Sous Windows : utiliser convert_gui.bat (interface, MSYS2 ou WSL).
set -u
ici="$(cd "$(dirname "$0")" && pwd)"
racine="$(cd "$ici/../.." && pwd)"
sortie="$PWD/aka-out"
rapide=0
entrees=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) sortie="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2 ;;
    -r|--rapide) rapide=1; shift ;;
    -h|--help) sed -n 2,12p "$0"; exit 0 ;;
    *) entrees+=("$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")"); shift ;;
  esac
done
[ ${#entrees[@]} -gt 0 ] || { echo "Usage : $0 [-o dossier] [-r] rom_ou_dossier ..."; exit 1; }

manque=""
for c in cmake ninja python3 pio; do command -v "$c" >/dev/null 2>&1 || manque="$manque $c"; done
if [ -n "$manque" ]; then
  echo "Outils manquants :$manque"
  echo "  Debian/Ubuntu/WSL : sudo apt install cmake ninja-build g++ zlib1g-dev python3 pipx && pipx install platformio"
  exit 1
fi

# Recompilateur : construit une fois.
if [ ! -x "$racine/build/bin/m0recomp" ]; then
  echo "[aka] premiere utilisation : construction du recompilateur..."
  ( cd "$racine" && cmake -G Ninja -B build . >/dev/null && ninja -C build ) || { echo "Echec de la construction"; exit 1; }
fi

# Les ROMs sont copiees dans un dossier de travail propre (compile.sh lit un dossier).
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
n=0; nmd=0; nz=0
ajouter() {  # $1 = fichier ROM ou .zip
  local f="$1" low
  low="$(printf '%s' "$f" | tr 'A-Z' 'a-z')"
  case "$low" in
    *.gb|*.gbc|*.sg|*.gg|*.sms) cp "$f" "$tmp/"; n=$((n+1)) ;;
    *.md|*.gen|*.smd) mkdir -p "$tmp/md"; cp "$f" "$tmp/md/"; nmd=$((nmd+1)) ;;
    *.bin) if [ "$(dd if="$f" bs=1 skip=256 count=4 2>/dev/null)" = SEGA ]; then
             mkdir -p "$tmp/md"; cp "$f" "$tmp/md/"; nmd=$((nmd+1))
           else echo "[aka] $(basename "$f") : .bin non reconnu, ignore."; fi ;;
    *.zip) nz=$((nz+1)); mkdir -p "$tmp/zx$nz"; unzip -o -q "$f" -d "$tmp/zx$nz" 2>/dev/null
           while IFS= read -r -d '' g; do ajouter "$g"; done < <(find "$tmp/zx$nz" -type f -print0) ;;
    *) echo "[aka] $(basename "$f") : format inconnu, ignore." ;;
  esac
}
for e in "${entrees[@]}"; do
  if [ -d "$e" ]; then
    while IFS= read -r -d '' f; do ajouter "$f"; done < <(find "$e" -maxdepth 2 -type f -print0)
  elif [ -f "$e" ]; then
    ajouter "$e"
  else
    echo "[aka] introuvable : $e"
  fi
done
[ $((n + nmd)) -gt 0 ] || { echo "Aucune ROM utilisable."; exit 1; }

export TARGET=aka
if [ "$rapide" = 1 ]; then export AUTO_SECONDS=20 SG_SEEDS=1 EXPLORERS=0; fi
brut="$tmp/_sortie"
if [ "$n" -gt 0 ]; then
  ( cd "$racine" && ./compile.sh "$tmp" "$brut" < /dev/null ) || true
fi

# ---- Mega Drive : PC (profil automatique) puis projet AKA puis firmware puis zip ----
convertir_md() {  # $1 = ROM
  local rom="$1" base nom k pc aka frames cov core boot d
  base="$(basename "$rom")"; nom="${base%.*}"; nom="${nom%% (*}"
  nom="$(printf '%s' "$nom" | tr -d "'" | tr -c 'A-Za-z0-9 _.-' '_' | sed 's/  */ /g; s/^ //; s/ $//')"
  [ -n "$nom" ] || nom="Jeu"
  k="$tmp/mdw/$nom"; pc="$k/pc"; aka="$k/aka"; mkdir -p "$k"
  frames=24000; [ "$rapide" = 1 ] && frames=6000
  echo "=== [Mega Drive] $nom ==="
  "$racine/build/bin/m0recomp" --system megadrive "$rom" --target pc -o "$pc" >"$k/pc.log" 2>&1 || { tail -3 "$k/pc.log"; return 1; }
  cmake -G Ninja -S "$pc" -B "$pc/build" >>"$k/pc.log" 2>&1 && ninja -C "$pc/build" >>"$k/pc.log" 2>&1 \
    || { echo "[aka] $nom : construction PC impossible (SDL2 requis)."; tail -5 "$k/pc.log"; return 1; }
  python3 - "$k/entrees.txt" "$frames" <<'PY'
import random, sys
random.seed(7); L = []; f = 60
masks = [128, 64, 16, 32, 128|64, 1, 2, 4, 8, 64|8, 64|4, 32|8]
while f < int(sys.argv[2]):
    L.append((f, random.choice(masks))); L.append((f + random.randint(3, 12), 0)); f += random.randint(20, 90)
open(sys.argv[1], "w").write("\n".join("%d %d" % x for x in L) + "\n")
PY
  echo "[aka] $nom : partie automatique de $frames images (profil natif)..."
  ( cd "$pc/build" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./md_pc --interp --frames "$frames" \
      --input "$k/entrees.txt" --coverage "$k/couverture.txt" >"$k/run.log" 2>&1 ) || true
  cov=(); [ -s "$k/couverture.txt" ] && cov=(--coverage "$k/couverture.txt")
  "$racine/build/bin/m0recomp" --system megadrive "$rom" --target aka "${cov[@]}" -o "$aka" >"$k/aka.log" 2>&1 \
    || { tail -3 "$k/aka.log"; return 1; }
  grep -h "instructions in" "$k/aka.log" | head -1
  echo "[aka] $nom : compilation du firmware (PlatformIO)..."
  pio run -d "$aka" >"$k/pio.log" 2>&1 || { echo "[aka] $nom : echec de la compilation :"; tail -12 "$k/pio.log"; return 1; }
  d="$brut/md/$nom"; mkdir -p "$d"
  core="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
  for f in firmware.bin firmware.sym bootloader.bin partitions.bin; do cp "$aka/.pio/build/aka/$f" "$d/$f" 2>/dev/null; done
  [ -f "$d/firmware.bin" ] || { echo "[aka] $nom : firmware.bin absent."; return 1; }
  cp "$core/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$d/" 2>/dev/null
  cp "$aka/README.md" "$d/README.md" 2>/dev/null
  python3 - "$d/meta.json" "$nom" <<'PY'
import datetime, json, sys
open(sys.argv[1], "w", encoding="utf-8").write(json.dumps(dict(title=sys.argv[2][:40], author="m0boy",
    description="Mega Drive (experimental), converti avec m0boy", version="1",
    date=str(datetime.date.today())), ensure_ascii=False, indent=2) + "\n")
PY
  ( cd "$brut/md" && zip -qr "$nom.zip" "$nom" ) || return 1
  echo "[aka] $nom : OK"
}
if [ "$nmd" -gt 0 ]; then
  mkdir -p "$brut/md"
  for rom in "$tmp"/md/*; do convertir_md "$rom" || true; done
fi

mkdir -p "$sortie"
nb=0
while IFS= read -r -d '' z; do cp "$z" "$sortie/"; nb=$((nb+1)); done < <(find "$brut" -name '*.zip' -print0 2>/dev/null)
if [ "$nb" -eq 0 ]; then echo; echo "Aucun jeu produit : voir les messages ci-dessus."; exit 1; fi
echo; echo "[aka] $nb jeu(x) dans : $sortie"
echo "      Decompresse chaque .zip a la racine de la carte SD, puis lance-le depuis le Launcher."
