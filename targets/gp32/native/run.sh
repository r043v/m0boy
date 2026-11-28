#!/bin/sh
# usage: run.sh [m0_gp32_sdl options]   (M0_GP32_BUILD: the build directory)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/app/settings.sh"
mkdir -p "$here/save"
exec "${M0_GP32_BUILD:-$here/build}/m0_gp32_sdl" --hle "$here/app/$app" \
    --eeprom "$here/save/gp32.eeprom" "$@"
