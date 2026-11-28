#!/bin/sh
# Profile guided build of the GP32 runtime, trained on local files.
# usage: pgo.sh BUILD_DIR FILE...   (FILE: an application, or a 512 KiB BIOS)
# M0_GP32_JOBS limits the parallel compilations and link time optimization jobs
# (they are memory hungry). M0_GP32_TARGETS restricts the final build, e.g.
# "m0_gp32_bench m0_gp32_sdl": every executable recompiles the application.
set -eu
[ $# -ge 2 ] || { echo "usage: $0 BUILD_DIR FILE..." >&2; exit 2; }
# M0_GP32_SOURCE: an enclosing project that includes the runtime.
source_dir=${M0_GP32_SOURCE:-$(cd "$(dirname "$0")" && pwd)}
build=$1
shift
rm -rf "$build/pgo"
cmake -S "$source_dir" -B "$build" -DCMAKE_BUILD_TYPE=Release -DM0_GP32_PGO=generate \
      -DM0_GP32_LTO_JOBS="${M0_GP32_JOBS:-}"
cmake --build "$build" --target m0_gp32_bench --parallel ${M0_GP32_JOBS:-}
for file in "$@"; do
    case "$file" in
        *.bin|*.BIN)
            "$build/m0_gp32_bench" "$file" 150000000 ;;
        *)
            "$build/m0_gp32_bench" --hle 300000000 --exec "$file"
            "$build/m0_gp32_bench" --hle 100000000 --cpi 16 --exec "$file" ;;
    esac
done
cmake -S "$source_dir" -B "$build" -DM0_GP32_PGO=use
cmake --build "$build" ${M0_GP32_TARGETS:+--target $M0_GP32_TARGETS} --parallel ${M0_GP32_JOBS:-}
