#!/usr/bin/env bash
set -euo pipefail

# La chaîne HuCard PC Engine -> exécutables Linux et Windows.  Les sessions
# de jeu (m0search.py) font découvrir le code sur l'interpréteur : la
# graine écran titre puis RUN, des sessions aléatoires, des rejeux
# manuels avec EXTRA_INPUT='partie1.input partie2.input' (lignes « trame
# boutons » du harnais).  La couverture fusionnée attend dans search/
# le HuC6280 -> C recompilé ; en attendant, les binaires livrés restent
# l'interpréteur, la recherche prouve surtout que le jeu se joue.
#
#   tools/pce/build_pc_binaries.sh [HUCARD] [DOSSIER]
#   SESSIONS=4 FRAMES=1500 SEED=1 MANUAL_FRAMES=2500
#   M0RECOMP=... M0SEARCH=...
#
# Livrés dans DOSSIER (défaut output/<slug>-pc, le slug tiré du nom de la
# HuCard) : <slug>-linux, <slug>.exe avec SDL2.dll à côté, README.txt.
# Ils embarquent la HuCard : gardez-les dans output/, hors du dépôt et
# des archives.

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
rom=${1:-"$repo/roms/pce/Raiden (USA).pce"}
recompiler=${M0RECOMP:-"$repo/build/bin/m0recomp"}
m0search=${M0SEARCH:-"$repo/tools/profile/m0search.py"}
rom=$(realpath "$rom")

base=$(basename "$rom"); base=${base%.*}; base=${base%% ~*}; base=${base%% (*}
slug=$(printf '%s' "$base" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]\+/-/g; s/^-//; s/-$//')
out=$(realpath -m "${2:-$repo/output/$slug-pc}")
mkdir -p "$out"

# le préfixe des projets, comme sanitize_prefix côté générateur
name=$(basename "$rom")
name=${name%.*}
name=$(printf '%s' "$name" | tr -c 'a-zA-Z0-9_' '_')

# la graine : l'écran titre (PUSH RUN vers la trame 300, l'attract lance
# sa démo vers la trame 620), RUN pendant le titre lance la partie
seed="$out/seed.input"
for f in $(seq 450 480); do echo "$f run"; done > "$seed"

# le harnais interpréteur : chaque session y collecte sa couverture
harness="$out/harness/project"
"$recompiler" --system pce "$rom" --target desktop -o "$harness"
cmake -G Ninja -S "$harness/host" -B "$harness/host/build"
ninja -C "$harness/host/build"

inputs=(--input "$seed")
if [[ -n ${EXTRA_INPUT:-} ]]; then
  for f in $EXTRA_INPUT; do inputs+=(--input "$f"); done
fi
python3 "$m0search" run --system pce \
  --harness "$harness/host/build/${name}_host" \
  -o "$out/search" --sessions "${SESSIONS:-4}" \
  --frames "${FRAMES:-1500}" --seed "${SEED:-1}" \
  --manual-frames "${MANUAL_FRAMES:-2500}" "${inputs[@]}"

build_pc() {
  local name=$1
  shift
  local project="$out/$name/project"
  "$recompiler" --system pce "$rom" --target pc -o "$project"
  cmake -G Ninja -S "$project" -B "$project/build" "$@"
  ninja -C "$project/build"
}

build_pc linux
cp "$out/linux/project/build/${name}_pc" "$out/$slug-linux"

toolchain="$out/mingw-toolchain.cmake"
cat > "$toolchain" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig
export PKG_CONFIG_PATH=
build_pc windows -DCMAKE_TOOLCHAIN_FILE="$toolchain"
cp "$out/windows/project/build/${name}_pc.exe" "$out/$slug.exe"
cp /usr/x86_64-w64-mingw32/bin/SDL2.dll "$out/SDL2.dll"

# the delivery note: what these are, how to run them, how they were made
game=$(basename "$rom"); game=${game%.*}
cat > "$out/README.txt" <<EOF
$game — PC Engine sur PC (m0boy)

Le cœur PC Engine de m0boy (HuC6280 interprété, VDC, VCE, PSG), avec la
HuCard embarquée dans l'exécutable. Rien d'autre à installer qu'une
bibliothèque SDL2 :

  Linux   : ./$slug-linux
            (SDL2 du système ; si absent : paquet sdl2 de la distribution)
  Windows : $slug.exe, avec SDL2.dll à côté du fichier
            (fournie ici)

Contrôles :
  Clavier  flèches = croix-direction, X = I (tir), Z = II,
           Entrée = RUN (Start / pause), Maj = SELECT, Échap = quitter
  Manette  croix ou stick gauche, B = I, A = II, Start = RUN,
           Select = SELECT
  Fenêtre  F8 change le filtre d'image, F10 l'échelle, F11 plein écran

Pour lancer $base : laisser défiler l'écran titre, puis RUN.

Hors écran (pour vérifier sans fenêtre, utile sur serveur) :
  ./$slug-linux --frames 300 --ppm capture.ppm
Le même run_hash sort du binaire Linux et du .exe Windows : le cœur est
déterministe d'une plateforme à l'autre.

Refaire ces binaires depuis les sources de m0boy :
  tools/pce/build_pc_binaries.sh "<HuCard.pce>" <répertoire de sortie>
(nécessite cmake, ninja, SDL2 de développement, et le compilateur croisé
x86_64-w64-mingw32 + son SDL2 pour le .exe)
EOF

printf 'Built:\n  %s\n  %s (requires SDL2.dll beside it)\n  %s\n' \
  "$out/$slug-linux" "$out/$slug.exe" "$out/README.txt"
