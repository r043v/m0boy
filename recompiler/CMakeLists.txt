# Source CPU, source system and output backend are independent build layers.
add_library(m0boy_common INTERFACE)
target_include_directories(m0boy_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/common/runtime)
find_package(ZLIB REQUIRED)
add_library(m0boy_rom_storage STATIC src/rom_storage.cpp)
target_link_libraries(m0boy_rom_storage PUBLIC m0boy_common ZLIB::ZLIB)
set_target_properties(m0boy_rom_storage PROPERTIES POSITION_INDEPENDENT_CODE ON)

# No Nintendo header or mapper dependency in the CPU decoder.
add_library(m0boy_cpu_sm83 STATIC src/cpu/sm83/decoder.cpp)
target_link_libraries(m0boy_cpu_sm83 PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_sm83 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(m0boy_cpu_z80 STATIC src/cpu/z80/decoder.cpp)
target_link_libraries(m0boy_cpu_z80 PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_z80 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(m0boy_cpu_6502 STATIC src/cpu/m6502/decoder.cpp)
target_link_libraries(m0boy_cpu_6502 PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_6502 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(m0boy_cpu_huc6280 STATIC src/cpu/huc6280/decoder.cpp)
target_link_libraries(m0boy_cpu_huc6280 PUBLIC m0boy_cpu_6502)
set_target_properties(m0boy_cpu_huc6280 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(m0boy_cpu_m0plus STATIC src/cpu/m0plus/decoder.cpp)
target_link_libraries(m0boy_cpu_m0plus PUBLIC m0boy_common)
set_target_properties(m0boy_cpu_m0plus PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(m0boy_frontends STATIC
    src/system.cpp
    src/system/gameboy/cartridge.cpp
    src/system/gameboy/frontend.cpp
    src/system/sg1000/cartridge.cpp
    src/system/pico8/cartridge.cpp
    src/system/nes/cartridge.cpp
    src/system/pce/cartridge.cpp
    src/system/megadrive/cartridge.cpp
    src/system/vectrex/cartridge.cpp
    src/system/snes/cartridge.cpp
    src/system/m0plus/firmware.cpp
    src/system/m0plus/generator.cpp
)
target_link_libraries(m0boy_frontends PUBLIC m0boy_common)
set_target_properties(m0boy_frontends PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Existing Game Boy analysis, IR and emitters retain their public API.
add_library(gbrecomp_core STATIC
    src/code_scan.cpp
    src/system/gameboy/analyzer.cpp
    src/system/gameboy/bank_tracker.cpp
    src/system/gameboy/decode_bank.cpp
    src/system/console_project.cpp
    src/system/sg1000/generator.cpp
    src/system/sg1000/emitter.cpp
    src/system/pico8/generator.cpp
    src/system/nes/emitter.cpp
    src/system/nes/generator.cpp
    src/system/pce/generator.cpp
    src/system/pce/emitter.cpp
    src/system/snes/generator.cpp
    src/system/snes/emitter.cpp
    src/system/megadrive/generator.cpp
    src/system/megadrive/emitter.cpp
    src/system/vectrex/emitter.cpp
    src/system/vectrex/generator.cpp
    src/symbol_table.cpp
    src/native_patch.cpp
    src/generator.cpp
    src/ir/ir_builder.cpp
    src/ir/ir_optimizer.cpp
    src/codegen/c_emitter.cpp
    src/codegen/register_ir.cpp
    src/codegen/meta_emitter.cpp
    src/codegen/deepeel_png.cpp
    src/target_info.cpp
)

target_link_libraries(gbrecomp_core PUBLIC m0boy_frontends m0boy_cpu_sm83 m0boy_cpu_z80 m0boy_z80_runtime m0boy_rom_storage)

target_include_directories(gbrecomp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # the SM83 cycle table shared with the runtime (gbl_cycles.h)
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/runtime
)

# Versions of the two consoles of the META edition, one file each: the META's
# in targets/meta/VERSION (the one source for the repository and the release
# archive), the Pokitto's in targets/consoles/pokitto/VERSION.  The meta
# emitter stamps the target console's into gbl_config.h so a firmware records
# which gbrecomp produced it; compile.sh and tools/meta-compiler read it back
# from firmware.bin and compare it with `gbrecomp --meta-version` or
# `gbrecomp --pokitto-version`.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/espboy/VERSION" GBRECOMP_ESPBOY_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION" GBRECOMP_META_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/pokitto/VERSION" GBRECOMP_POKITTO_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/pc/VERSION" GBRECOMP_PC_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/odroid/VERSION" GBRECOMP_ODROID_VERSION LIMIT_COUNT 1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/aka/VERSION" GBRECOMP_AKA_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/meta/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/pokitto/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/espboy/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/pc/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/odroid/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/../targets/consoles/aka/VERSION")
target_compile_definitions(gbrecomp_core PRIVATE
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
    GBRECOMP_POKITTO_VERSION="${GBRECOMP_POKITTO_VERSION}"
    GBRECOMP_ESPBOY_VERSION="${GBRECOMP_ESPBOY_VERSION}"
    GBRECOMP_PC_VERSION="${GBRECOMP_PC_VERSION}"
    GBRECOMP_ODROID_VERSION="${GBRECOMP_ODROID_VERSION}"
    GBRECOMP_AKA_VERSION="${GBRECOMP_AKA_VERSION}"
)

# Enable position-independent code for shared library support
set_target_properties(gbrecomp_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_LINUX)
endif()

# m0recomp is the system-neutral entry point; gbrecomp stays available for
# compile.sh, the Node frontend and existing generated-project tests.
add_library(m0boy_cli OBJECT src/main.cpp)
target_link_libraries(m0boy_cli PRIVATE gbrecomp_core m0boy_cpu_z80)
add_executable(m0recomp $<TARGET_OBJECTS:m0boy_cli>)
add_executable(gbrecomp $<TARGET_OBJECTS:m0boy_cli>)
target_link_libraries(m0recomp PRIVATE gbrecomp_core m0boy_cpu_z80 m0boy_cpu_6502 m0boy_cpu_huc6280 m0boy_cpu_m0plus)
target_link_libraries(gbrecomp PRIVATE gbrecomp_core m0boy_cpu_z80 m0boy_cpu_6502 m0boy_cpu_huc6280 m0boy_cpu_m0plus)
target_compile_definitions(m0boy_cli PRIVATE
    GBRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime"
    GBRECOMP_DEFAULT_META_DIR="${PROJECT_SOURCE_DIR}/targets/meta"
    GBRECOMP_VERSION="${PROJECT_VERSION}"
    GBRECOMP_META_VERSION="${GBRECOMP_META_VERSION}"
    GBRECOMP_POKITTO_VERSION="${GBRECOMP_POKITTO_VERSION}"
    GBRECOMP_ESPBOY_VERSION="${GBRECOMP_ESPBOY_VERSION}"
    GBRECOMP_PC_VERSION="${GBRECOMP_PC_VERSION}"
    GBRECOMP_ODROID_VERSION="${GBRECOMP_ODROID_VERSION}"
    GBRECOMP_AKA_VERSION="${GBRECOMP_AKA_VERSION}"
)

# PICO-8: the cartridge reader and the Lua 5.2 (PICO-8 flavour, fix32 numbers)
# compiler of femto8 do the front end on the PC; the generated C needs none of
# it.  femto8 stays a separate checkout (FEMTO8_DIR).
set(FEMTO8_DIR "${PROJECT_SOURCE_DIR}/../femto8" CACHE PATH "femto8 checkout (PICO-8 front end)")
if(EXISTS "${FEMTO8_DIR}/src/lua/lparser.c")
    file(GLOB P8_LUA_SOURCES "${FEMTO8_DIR}/src/lua/*.c")
    list(FILTER P8_LUA_SOURCES EXCLUDE REGEX "ltests\\.c$")
    add_library(m0boy_p8_femto8 STATIC ${P8_LUA_SOURCES}
        ${FEMTO8_DIR}/src/p8_parser.c ${FEMTO8_DIR}/src/strtcpy.c
        ${FEMTO8_DIR}/src/lodepng/lodepng.c
        ${FEMTO8_DIR}/src/lexaloffle/p8_compress.c ${FEMTO8_DIR}/src/lexaloffle/pxa_compress_snippets.c)
    target_include_directories(m0boy_p8_femto8 PUBLIC ${FEMTO8_DIR}/src ${FEMTO8_DIR}/src/lua ${FEMTO8_DIR}/src/data
        ${FEMTO8_DIR}/src/lodepng ${FEMTO8_DIR}/src/lexaloffle)
    target_compile_definitions(m0boy_p8_femto8 PRIVATE LUA_USE_POSIX LODEPNG_NO_COMPILE_ENCODER LODEPNG_NO_COMPILE_DISK
        LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS LODEPNG_NO_COLOR_CONVERT _GNU_SOURCE)
    target_compile_options(m0boy_p8_femto8 PRIVATE -w)
    find_package(SDL2 REQUIRED) # p8_emu.h includes SDL.h; nothing of it is linked or run
    target_include_directories(m0boy_p8_femto8 PRIVATE ${SDL2_INCLUDE_DIRS})
    set_target_properties(m0boy_p8_femto8 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # the compiler itself (p8c_main), used by the p8c tool and by m0recomp
    add_library(m0boy_p8c STATIC src/system/pico8/p8c.c)
    target_link_libraries(m0boy_p8c PUBLIC m0boy_p8_femto8 m)
    target_compile_definitions(m0boy_p8c PRIVATE LUA_USE_POSIX LODEPNG_NO_COLOR_CONVERT)
    target_include_directories(m0boy_p8c PRIVATE ${PROJECT_SOURCE_DIR}/targets/pico8/runtime ${FEMTO8_DIR}/src/data)
    set_target_properties(m0boy_p8c PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_executable(p8c src/system/pico8/p8c_main.c)
    target_link_libraries(p8c PRIVATE m0boy_p8c)
endif()

# gbrecomp_core compiles PICO-8 projects when the front end is there
if(TARGET m0boy_p8c)
    target_link_libraries(gbrecomp_core PUBLIC m0boy_p8c)
    target_compile_definitions(gbrecomp_core PRIVATE M0BOY_PICO8=1)
endif()
