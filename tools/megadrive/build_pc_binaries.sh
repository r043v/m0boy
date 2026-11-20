#!/usr/bin/env bash
set -euo pipefail

# La chaîne ROM Mega Drive -> exécutables Linux et Windows.  Le profil qui
# choisit le C généré vient de la recherche de parties (m0search.py) : la
# graine Start/droite/saut, puis des sessions aléatoires ; les couvertures
# sont fusionnées avant la génération native.  Des enregistrements de
# parties manuelles (--record-input de md_pc, dialecte « trame masque »)
# s'ajoutent avec EXTRA_INPUT='partie1.input partie2.input'.
#
#   tools/megadrive/build_pc_binaries.sh [ROM] [DOSSIER]
#   SESSIONS=6 FRAMES=1500 SEED=1 MANUAL_FRAMES=2500
#   M0RECOMP=... M0SEARCH=...
#
# Livrés dans DOSSIER (défaut output/<slug>-pc, le slug tiré du nom de la
# ROM) : <slug>-linux, <slug>.exe avec SDL2.dll à côté.  Sans RELEASE=1 ils
# embarquent la ROM : gardez-les dans output/, hors du dépôt et des archives.

# RELEASE=1 : les exécutables livrés n'embarquent pas la ROM (--rom-external) ;
# ils la cherchent à côté d'eux ou la reçoivent par --rom FICHIER (ou 1er argument),
# et refusent toute autre ROM (taille + CRC-32).  Le projet interpréteur de
# recherche, lui, garde la ROM : il ne sort pas de output/.
RELEASE_ROM_FLAG=""
if [[ "${RELEASE:-0}" == 1 ]]; then RELEASE_ROM_FLAG="--rom-external"; fi

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
rom=${1:-"$repo/roms/md/Sonic The Hedgehog (USA, Europe).md"}
rom=$(realpath "$rom")
recompiler=${M0RECOMP:-"$repo/build/bin/m0recomp"}
m0search=${M0SEARCH:-"$repo/tools/profile/m0search.py"}

base=$(basename "$rom"); base=${base%.*}; base=${base%% ~*}; base=${base%% (*}
slug=$(printf '%s' "$base" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]\+/-/g; s/^-//; s/-$//')
out=$(realpath -m "${2:-$repo/output/$slug-pc}")
mkdir -p "$out"

seed="$out/seed.input"
printf '600 128\n602 0\n1050 8\n1150 0\n1300 64\n1310 0\n' > "$seed"

# Le projet interpréteur collecte la couverture de chaque session.
interp="$out/interp/project"
"$recompiler" --system megadrive "$rom" --target pc -o "$interp"
cmake -G Ninja -S "$interp" -B "$interp/build"
ninja -C "$interp/build"

inputs=(--input "$seed")
if [[ -n ${EXTRA_INPUT:-} ]]; then
  for f in $EXTRA_INPUT; do inputs+=(--input "$f"); done
fi
python3 "$m0search" run --system megadrive --harness "$interp/build/md_pc" \
  -o "$out/search" --interp --sessions "${SESSIONS:-4}" \
  --frames "${FRAMES:-1500}" --seed "${SEED:-1}" \
  --manual-frames "${MANUAL_FRAMES:-2500}" "${inputs[@]}"

build_project() { # $1: nom, $2..: cmake en plus
  local name=$1; shift
  local project="$out/$name/project"
  "$recompiler" --system megadrive "$rom" --target pc \
    --coverage "$out/search/coverage.merged" ${RELEASE_ROM_FLAG:-} -o "$project"
  cmake -G Ninja -S "$project" -B "$project/build" "$@"
  ninja -C "$project/build"
}

build_project linux
cp "$out/linux/project/build/md_pc" "$out/$slug-linux"

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
build_project windows -DCMAKE_TOOLCHAIN_FILE="$toolchain"
cp "$out/windows/project/build/md_pc.exe" "$out/$slug.exe"
cp /usr/x86_64-w64-mingw32/bin/SDL2.dll "$out/SDL2.dll"
printf 'Built:\n  %s\n  %s (requires SDL2.dll beside it)\n' \
  "$out/$slug-linux" "$out/$slug.exe"
