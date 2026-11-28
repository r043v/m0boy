#!/bin/sh
# Link time and profile guided optimized build, trained on the application.
# usage: build.sh [BUILD_DIR]
# M0_GP32_JOBS limits parallel compilations (each one is memory hungry).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/app/settings.sh"
build=${1:-$here/build}
mkdir -p "$build/cards"
M0_GP32_SOURCE=$here M0_GP32_TARGETS=${M0_GP32_TARGETS:-$targets} \
    M0_GP32_CARDS=$build/cards M0_GP32_JOBS=${M0_GP32_JOBS:-4} \
    sh "$here/gp32/runtime/pgo.sh" "$build" "$here/app/$app"
