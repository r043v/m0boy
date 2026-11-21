#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
project=${1:-"$root/output/wonderswan-meta"}
rom=${2:-}
meta="$root/targets/consoles/meta"
common="$root/targets/common"

mkdir -p "$project/src" "$project/boards" "$project/toolchain"
cp "$meta/boards/gamebuino_meta.json" "$project/boards/"
sed 's/__StackTop - SIZEOF(.stack_dummy)/__StackTop - SIZEOF(.stack_dummy) - 2048/'     "$meta/gamebuino_meta.ld.in" > "$project/gamebuino_meta.ld"
sed -e 's/@NAME@/wonderswan/g' -e 's/@COMPILER@/gcc/g' \
    "$meta/platformio.ini.in" > "$project/platformio.ini"
cp "$common/toolchain/platformio_compiler.py" "$project/toolchain/"
cp "$meta"/{meta_startup.c,meta_clock.cpp,meta_audio.cpp,meta_dma.h,meta_lcd.cpp,meta_lcd_pack.h,meta_spi.c,meta_sd_bus.cpp,meta_hw.h,meta_usb.c,meta_usb.h,meta_usb_msc.h} "$project/src/"
cp "$common/platform"/m0_{audio,card,clock,fat,lcd,menu,pad,sd,target}.h "$project/src/"
cp "$common/platform"/m0_{fat,sd}.c "$common/runtime/m0_scale.h" "$project/src/"
cp "$root/targets/meta/runtime/gbl_board.h" "$project/src/"
cp "$root"/targets/wonderswan/runtime/*.[ch] "$root/targets/wonderswan/platform/ws_meta.cpp" "$project/src/"
sed -i 's/build_flags = /build_flags = -DWS_ENABLE_COLOR=0 /' "$project/platformio.ini"
cat > "$project/src/gbl_config.h" <<'CONFIG'
#pragma once
#define GBL_TARGET_POKITTO 0
CONFIG
cat > "$project/src/gbl_profile.h" <<'PROFILE'
#pragma once
#define META_AUDIO_PUSH_RATE 24000u
#define META_LCD_DMA 0
#define META_LCD_12BPP 0
#define META_OVERCLOCK 0
#define META_USB_MODE 1
#define META_USB_DEBUG 1
#include "gbl_board.h"
PROFILE
pack=()
if [[ -n "$rom" ]]; then
    cmake -G Ninja -B "$root/build" "$root" -DBUILD_TESTS=ON
    ninja -C "$root/build" ws_host
    "$root/build/bin/ws_host" "$rom" "$project/profile.pgm" 600 120         --rom-trace "$project/rom-reads.txt"
    pack=(--rom "$rom" --trace "$project/rom-reads.txt")
fi
exec python3 "$root/targets/wonderswan/platform/size_meta.py" "$project" "${pack[@]}"
