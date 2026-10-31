# m0boy: gbrecomp (the recompiler) and the Cortex-M0+ targets, Gamebuino
# META and Pokitto.  `cmake -G Ninja -B build . && ninja -C build` gives
# build/bin/gbrecomp, which compile.sh uses.
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_SOURCE_DIR}/targets/meta/host/prefer_compiler.cmake")
m0boy_select_host_compiler(C CXX)
# The version (targets/meta/VERSION) is stamped into every firmware
# ("gbrecomp-meta x.y.z"); compile.sh offers to rebuild firmwares made by
# another version.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/targets/meta/VERSION" GBRECOMP_META_VERSION_FILE LIMIT_COUNT 1)
project(m0boy VERSION ${GBRECOMP_META_VERSION_FILE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(recompiler)

add_library(m0boy_m0plus_runtime STATIC targets/m0plus/runtime/m0_cpu.c
    targets/m0plus/runtime/m0_machine.c)
target_include_directories(m0boy_m0plus_runtime PUBLIC targets/m0plus/runtime)

add_library(m0boy_z80_runtime STATIC targets/sg1000/runtime/z80.c)
target_include_directories(m0boy_z80_runtime PUBLIC targets/sg1000/runtime)
add_library(m0boy_sg1000_runtime STATIC targets/sg1000/runtime/sg1000.c targets/sg1000/runtime/sg_audio.c targets/sg1000/runtime/tms9918.c)
target_include_directories(m0boy_sg1000_runtime PUBLIC targets/common/runtime) # sg_psg.h
target_link_libraries(m0boy_sg1000_runtime PUBLIC m0boy_z80_runtime)
add_executable(sg1000_host targets/sg1000/host/sg_host.c)
target_include_directories(sg1000_host PRIVATE targets/common/host) # m0_explore.h
target_link_libraries(sg1000_host PRIVATE m0boy_sg1000_runtime)
# MSX: the SG-1000 runtime with its memory map, ports and BIOS replaced (SG_MSX).
add_library(m0boy_msx_runtime STATIC targets/sg1000/runtime/sg1000.c targets/sg1000/runtime/tms9918.c
    targets/msx/runtime/msx.c targets/msx/runtime/msx_ay.c)
target_include_directories(m0boy_msx_runtime PUBLIC targets/common/runtime targets/sg1000/runtime)
target_link_libraries(m0boy_msx_runtime PUBLIC m0boy_z80_runtime)
target_compile_definitions(m0boy_msx_runtime PUBLIC SG_MSX=1 SG_MSX_RAM=16384 SG_MIX_SLOTS=48 SG_MSX_TRACE=1) # TRACE: the frames that read each ROM sector, for the storage plan
add_executable(msx_host targets/sg1000/host/sg_host.c)
target_include_directories(msx_host PRIVATE targets/common/host) # m0_explore.h
target_link_libraries(msx_host PRIVATE m0boy_msx_runtime)
add_library(m0boy_gg_runtime STATIC targets/gamegear/runtime/gg.c targets/gamegear/runtime/gg_vdp.c targets/gamegear/runtime/gg_psg.c)
target_include_directories(m0boy_gg_runtime PUBLIC targets/gamegear/runtime)
target_include_directories(m0boy_gg_runtime PUBLIC targets/common/runtime) # sg_psg.h
target_link_libraries(m0boy_gg_runtime PUBLIC m0boy_z80_runtime)
target_compile_definitions(m0boy_gg_runtime PUBLIC GG_ROM_TRACE=1) # host profiling of ROM data reads
add_executable(gg_host targets/gamegear/host/gg_host.c)
target_include_directories(gg_host PRIVATE targets/common/host) # m0_explore.h
target_link_libraries(gg_host PRIVATE m0boy_gg_runtime)
# SNES desktop reference harness (the PPU currently renders a subset).
add_executable(snes_host targets/snes/host/snes_host.c targets/snes/runtime/snes.c
    targets/snes/runtime/snes_cpu.c targets/snes/runtime/snes_ppu.c targets/snes/runtime/snes_smp.c targets/snes/runtime/snes_dsp.c targets/snes/runtime/snes_dsp1.c targets/snes/runtime/snes_fx.c targets/snes/runtime/snes_sa1.c)
target_compile_definitions(snes_host PRIVATE SNES_TRACE=1)
target_include_directories(snes_host PRIVATE targets/consoles/pc)
# Card cache and flash residency simulated on host ROM traces (tools/profile).
add_executable(m0romsim tools/profile/m0romsim.cpp)
target_link_libraries(m0romsim PRIVATE m0boy_rom_storage)
# Desktop play is optional; firmware and the reference harness remain headless.
find_package(SDL2 QUIET)
if(TARGET SDL2::SDL2)
    add_executable(sg1000_play targets/sg1000/host/sg_player.c)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(sg1000_play PRIVATE SDL2::SDL2main)
    endif()
    target_link_libraries(sg1000_play PRIVATE m0boy_sg1000_runtime SDL2::SDL2)
endif()


option(BUILD_TESTS "Build the META and Pokitto target tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_snes_runtime tests/test_snes_runtime.c
        targets/snes/runtime/snes.c targets/snes/runtime/snes_cpu.c targets/snes/runtime/snes_ppu.c targets/snes/runtime/snes_smp.c targets/snes/runtime/snes_dsp.c targets/snes/runtime/snes_dsp1.c targets/snes/runtime/snes_fx.c targets/snes/runtime/snes_sa1.c)
    add_test(NAME snes_runtime_bus_interrupt COMMAND test_snes_runtime)
    add_executable(test_m0plus_cpu tests/test_m0plus_cpu.c)
    target_link_libraries(test_m0plus_cpu PRIVATE m0boy_m0plus_runtime)
    add_test(NAME m0plus_cpu_core COMMAND test_m0plus_cpu)
    add_executable(test_explore tests/test_explore.c)
    target_include_directories(test_explore PRIVATE targets/common/host)
    add_test(NAME common_explorer_checkpoints COMMAND test_explore)
    add_executable(test_rom_storage tests/test_rom_storage.cpp)
    target_link_libraries(test_rom_storage PRIVATE m0boy_rom_storage)
    target_include_directories(test_rom_storage PRIVATE targets/common/runtime)
    add_test(NAME common_rom_storage_codecs COMMAND test_rom_storage)
    add_executable(rom_codec_bench_data tests/rom_codec_bench_data.cpp)
    target_link_libraries(rom_codec_bench_data PRIVATE m0boy_rom_storage)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME snes_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_snes_e2e.py"
                "$<TARGET_FILE:m0recomp>")
    add_test(NAME m0plus_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_m0plus_e2e.py"
                "$<TARGET_FILE:m0recomp>")
    add_test(NAME compile_batch_source_systems
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_compile_batch.py")
    add_executable(test_z80_execution tests/test_z80_execution.cpp)
    target_link_libraries(test_z80_execution PRIVATE m0boy_z80_runtime m0boy_cpu_z80)
    add_test(NAME z80_execution_reference COMMAND test_z80_execution)
    add_executable(test_z80_fast tests/test_z80_fast.c)
    target_link_libraries(test_z80_fast PRIVATE m0boy_z80_runtime)
    add_test(NAME z80_fast_path_against_reference COMMAND test_z80_fast)
    add_executable(test_sg1000_runtime tests/test_sg1000_runtime.c)
    target_link_libraries(test_sg1000_runtime PRIVATE m0boy_sg1000_runtime)
    add_test(NAME sg1000_runtime_devices COMMAND test_sg1000_runtime)
    # the Vectrex core: the run loop against the reference in lockstep
    add_executable(test_vectrex_core tests/test_vectrex_core.c
        targets/vectrex/runtime/vectrex.c targets/vectrex/runtime/m6809.c
        targets/vectrex/runtime/m6809_run.c targets/vectrex/runtime/via.c
        targets/vectrex/runtime/psg.c)
    add_test(NAME vectrex_core_smoke COMMAND test_vectrex_core)
    add_executable(test_gg_runtime tests/test_gg_runtime.c)
    target_link_libraries(test_gg_runtime PRIVATE m0boy_gg_runtime)
    add_test(NAME gg_sms_runtime_reference COMMAND test_gg_runtime)
    add_test(NAME korean150_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_korean150_e2e.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --host "$<TARGET_FILE:gg_host>")
    set_tests_properties(korean150_native_end_to_end PROPERTIES TIMEOUT 300)
    add_test(NAME sg1000_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sg1000_e2e.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --host "$<TARGET_FILE:sg1000_host>")
    set_tests_properties(sg1000_native_end_to_end PROPERTIES TIMEOUT 300)
    add_executable(test_msx_runtime tests/test_msx_runtime.c)
    target_link_libraries(test_msx_runtime PRIVATE m0boy_msx_runtime)
    add_test(NAME msx_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_msx_e2e.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --host "$<TARGET_FILE:msx_host>"
                --runtime-test "$<TARGET_FILE:test_msx_runtime>")
    set_tests_properties(msx_native_end_to_end PROPERTIES TIMEOUT 300)
    add_test(NAME gg_native_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_gg_e2e.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --host "$<TARGET_FILE:gg_host>")
    set_tests_properties(gg_native_end_to_end PROPERTIES TIMEOUT 300)
    add_test(NAME profile_search_generic
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_profile_search.py")
    set_tests_properties(profile_search_generic PROPERTIES TIMEOUT 120)
    # SG-1000, Game Gear, Master System on the ESPboy: generated projects and
    # the cartridge RAM saved in flash (mock flash).
    add_test(NAME sega_espboy_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sega_espboy.py"
                --recompiler "$<TARGET_FILE:m0recomp>" --gg-host "$<TARGET_FILE:gg_host>"
                --sg-host "$<TARGET_FILE:sg1000_host>")
    set_tests_properties(sega_espboy_target PROPERTIES TIMEOUT 300)
    if(TARGET sg1000_play)
        add_executable(test_pc_frontend tests/test_pc_frontend.c targets/consoles/pc/pc_frontend.c)
        target_include_directories(test_pc_frontend PRIVATE targets/consoles/pc)
        target_link_libraries(test_pc_frontend PRIVATE SDL2::SDL2)
        add_test(NAME pc_frontend_controls_and_filter COMMAND test_pc_frontend)
        add_executable(test_sg1000_input tests/test_sg1000_input.c)
        target_include_directories(test_sg1000_input PRIVATE targets/sg1000/host)
        target_link_libraries(test_sg1000_input PRIVATE SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(test_sg1000_input PRIVATE SDL2::SDL2main)
        endif()
        add_test(NAME sg1000_keyboard_controls COMMAND test_sg1000_input)
        add_test(NAME sg1000_desktop_player
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sg1000_player.py"
                    "$<TARGET_FILE:sg1000_play>" "$<TARGET_FILE:sg1000_host>")
    endif()
    add_test(NAME profile_rom_cache_simulation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_romsim.py"
                "$<TARGET_FILE:m0romsim>" "$<TARGET_FILE:gg_host>")
    add_executable(test_pce_psg tests/test_pce_psg.c targets/pce/runtime/psg.c)
    target_include_directories(test_pce_psg PRIVATE targets/pce/runtime)
    add_test(NAME pce_psg_clock_and_balance COMMAND test_pce_psg)
    add_executable(test_pce_vdc tests/test_pce_vdc.c targets/pce/runtime/vdc.c)
    target_include_directories(test_pce_vdc PRIVATE targets/pce/runtime)
    add_test(NAME pce_vdc_render_and_vram COMMAND test_pce_vdc)
    add_executable(test_pc_input tests/test_pc_input.c)
    target_include_directories(test_pc_input PRIVATE targets/consoles/pc)
    add_test(NAME pc_input_scripts_and_recordings COMMAND test_pc_input)
    # the PC save states of m0_state.cpp: the plain file store, host side
    add_executable(test_pc_state tests/pc_state_check.cpp targets/common/platform/m0_state.cpp)
    target_include_directories(test_pc_state PRIVATE tests/pc_state_stub targets/common/platform)
    add_test(NAME pc_state_file_store COMMAND test_pc_state)
    if(TARGET sg1000_host)
        add_test(NAME profile_gm0_sessions
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_profile_sessions.py"
                    "$<TARGET_FILE:sg1000_host>")
    endif()
    if(TARGET p8c)
        add_test(NAME nes_end_to_end
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_nes_e2e.py"
        )
        set_tests_properties(nes_end_to_end PROPERTIES TIMEOUT 300)
        add_test(NAME pce_end_to_end
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_pce_e2e.py"
        )
        set_tests_properties(pce_end_to_end PROPERTIES TIMEOUT 300)
        add_test(NAME vectrex_end_to_end
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_vectrex_e2e.py"
        )
        set_tests_properties(vectrex_end_to_end PROPERTIES TIMEOUT 600)
        add_test(NAME pico8_native_end_to_end
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_pico8_e2e.py"
                    --recompiler "$<TARGET_FILE:m0recomp>")
        set_tests_properties(pico8_native_end_to_end PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
    add_executable(test_source_systems tests/test_source_systems.cpp)
    target_link_libraries(test_source_systems PRIVATE m0boy_frontends)
    add_test(NAME source_system_profiles COMMAND test_source_systems)
    add_executable(test_sm83_reader tests/test_sm83_reader.cpp)
    target_link_libraries(test_sm83_reader PRIVATE m0boy_cpu_sm83)
    add_test(NAME sm83_console_independent_reader COMMAND test_sm83_reader)
    add_executable(test_cpu_decoders tests/test_cpu_decoders.cpp)
    target_link_libraries(test_cpu_decoders PRIVATE m0boy_cpu_sm83 m0boy_cpu_z80 m0boy_cpu_6502)
    add_test(NAME sm83_z80_shared_encoding COMMAND test_cpu_decoders)
    add_test(NAME source_system_cli
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_source_cli.py"
                "$<TARGET_FILE:m0recomp>")
    add_test(NAME megadrive_m68k_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_megadrive_e2e.py"
                "$<TARGET_FILE:m0recomp>")
    if(EXISTS "${PROJECT_SOURCE_DIR}/roms/md/Sonic The Hedgehog (USA, Europe).md")
        add_test(NAME megadrive_sonic_pc_native
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_megadrive_sonic_pc.py"
                    --recompiler "$<TARGET_FILE:m0recomp>"
                    --rom "${PROJECT_SOURCE_DIR}/roms/md/Sonic The Hedgehog (USA, Europe).md")
    endif()
    # The YM2612 against ymfm (BSD-3, a test oracle only, never linked into a
    # generated project): clone https://github.com/aaronsgiles/ymfm there.
    set(YMFM_DIR "${PROJECT_SOURCE_DIR}/output/deps/ymfm" CACHE PATH "ymfm sources for the YM2612 comparison")
    if(EXISTS "${YMFM_DIR}/src/ymfm_opn.cpp")
        add_executable(md_ym_compare tests/md_ym_compare.cpp targets/megadrive/runtime/md_ym2612.c
            "${YMFM_DIR}/src/ymfm_opn.cpp" "${YMFM_DIR}/src/ymfm_adpcm.cpp"
            "${YMFM_DIR}/src/ymfm_ssg.cpp" "${YMFM_DIR}/src/ymfm_misc.cpp")
        target_include_directories(md_ym_compare PRIVATE "${YMFM_DIR}/src" targets/megadrive/runtime)
        target_link_libraries(md_ym_compare PRIVATE m)
        add_test(NAME megadrive_ym2612_matches_ymfm
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_md_ym_ymfm.py"
                    "$<TARGET_FILE:md_ym_compare>")
        if(EXISTS "${PROJECT_SOURCE_DIR}/roms/md/Sonic The Hedgehog (USA, Europe).md")
            set_property(TEST megadrive_sonic_pc_native APPEND PROPERTY ENVIRONMENT
                "MD_YM_COMPARE=$<TARGET_FILE:md_ym_compare>")
        endif()
    endif()
    add_executable(test_megadrive_pad tests/test_megadrive_pad.c targets/megadrive/runtime/md.c)
    target_include_directories(test_megadrive_pad PRIVATE targets/megadrive/runtime)
    add_test(NAME megadrive_pad_bus COMMAND test_megadrive_pad)
    add_executable(test_megadrive_vdp tests/test_megadrive_vdp.c targets/megadrive/runtime/md.c)
    target_include_directories(test_megadrive_vdp PRIVATE targets/megadrive/runtime)
    target_compile_definitions(test_megadrive_vdp PRIVATE MD_FULL_VDP=1)
    add_test(NAME megadrive_pc_vdp COMMAND test_megadrive_vdp)
    add_executable(test_register_ir tests/test_register_ir.cpp)
    target_link_libraries(test_register_ir PRIVATE gbrecomp_core)
    add_test(NAME meta_register_ir_semantics COMMAND test_register_ir)
    add_test(NAME meta_register_ir_native
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_register_ir_native.py"
                --ir-test "$<TARGET_FILE:test_register_ir>" --runtime "${PROJECT_SOURCE_DIR}/targets/meta/runtime")
    set_tests_properties(meta_register_ir_native PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME meta_register_ir_clang
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_register_ir_native.py"
                --ir-test "$<TARGET_FILE:test_register_ir>" --runtime "${PROJECT_SOURCE_DIR}/targets/meta/runtime"
                --compiler clang)
    set_tests_properties(meta_register_ir_clang PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME meta_compiler_selection
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_arm_compiler.py")
    add_test(NAME meta_cmake_compiler_selection
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_cmake_compiler.py"
                --cmake "${CMAKE_COMMAND}" --selector "${PROJECT_SOURCE_DIR}/targets/meta/host/prefer_compiler.cmake")
    set(GBL_META_DIR "${PROJECT_SOURCE_DIR}/targets/meta")
set(GBL_CONSOLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/targets/consoles")
set(GBL_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/targets/common")
    file(GLOB GBL_META_RUNTIME_SOURCES "${GBL_META_DIR}/runtime/gbl_*.c")
    include_directories("${PROJECT_SOURCE_DIR}/targets/common/runtime") # m0_cache.h, shared with the Game Gear

    add_executable(test_meta_ppu_reference "${GBL_META_DIR}/tests/ppu_reference.c" ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_ppu_reference PRIVATE "${GBL_META_DIR}/runtime" "${GBL_META_DIR}/tests/config")
    add_test(NAME meta_ppu_matches_per_pixel_reference COMMAND test_meta_ppu_reference)

    add_executable(test_meta_ppu_reference_pal32 "${GBL_META_DIR}/tests/ppu_reference.c" ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_ppu_reference_pal32 PRIVATE "${GBL_META_DIR}/runtime" "${GBL_META_DIR}/tests/config")
    target_compile_definitions(test_meta_ppu_reference_pal32 PRIVATE GBL_PPU_PAL32=1 GBL_PPU_BATCH=1)
    add_test(NAME meta_ppu_pal32_matches_per_pixel_reference COMMAND test_meta_ppu_reference_pal32)

    add_executable(test_meta_lcd_queue "${GBL_META_DIR}/tests/lcd_queue.c")
    target_include_directories(test_meta_lcd_queue PRIVATE "${GBL_CONSOLES_DIR}/meta")
    add_test(NAME meta_lcd_queue_is_fifo_and_lossless COMMAND test_meta_lcd_queue)

    add_executable(test_meta_display_sim "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/gbl_display.c" "${GBL_META_DIR}/platform/gbl_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform" "${GBL_CONSOLES_DIR}/meta" "${GBL_META_DIR}/tests")
    add_test(NAME gbl_display_panel_matches_reference COMMAND test_meta_display_sim)

    add_executable(test_meta_display_sim_stream "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/gbl_display.c" "${GBL_META_DIR}/platform/gbl_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim_stream PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform" "${GBL_CONSOLES_DIR}/meta" "${GBL_META_DIR}/tests")
    target_compile_definitions(test_meta_display_sim_stream PRIVATE META_MERGE_MODES=0)
    add_test(NAME gbl_display_stream_profile_matches_reference COMMAND test_meta_display_sim_stream)

    # 12-bit panel (META_LCD_12BPP): rows packed in place, read back as the panel would.
    add_executable(test_meta_display_sim_12bpp "${GBL_META_DIR}/tests/display_sim.c"
        "${GBL_META_DIR}/platform/gbl_display.c" "${GBL_META_DIR}/platform/gbl_palettes.c"
        ${GBL_META_RUNTIME_SOURCES})
    target_include_directories(test_meta_display_sim_12bpp PRIVATE "${GBL_META_DIR}/runtime"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform" "${GBL_CONSOLES_DIR}/meta"
        "${GBL_META_DIR}/tests")
    target_compile_definitions(test_meta_display_sim_12bpp PRIVATE META_LCD_12BPP=1)
    add_test(NAME meta_display_12bpp_matches_reference COMMAND test_meta_display_sim_12bpp)

    # Exercise both compact data structures and the original layout, including
    # short DMA strips, TBLEND fallback, TBLEND with one held line (with and
    # without the merge modes' table) and the 12-bit panel path.
    foreach(variant legacy merge_tblend2 short short_tile short_tblend2 short_12bpp)
        set(test_name test_meta_display_${variant})
        add_executable(${test_name} "${GBL_META_DIR}/tests/display_sim.c"
            "${GBL_META_DIR}/platform/gbl_display.c"
            "${GBL_META_DIR}/platform/gbl_palettes.c" ${GBL_META_RUNTIME_SOURCES})
        target_include_directories(${test_name} PRIVATE "${GBL_META_DIR}/runtime"
            "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform"
            "${GBL_CONSOLES_DIR}/meta" "${GBL_META_DIR}/tests")
        if(variant STREQUAL "legacy")
            target_compile_definitions(${test_name} PRIVATE META_DISPLAY_COMPACT=0 META_STRIP_LINES=4)
        elseif(variant STREQUAL "merge_tblend2")
            target_compile_definitions(${test_name} PRIVATE META_TBLEND=2)
        else()
            target_compile_definitions(${test_name} PRIVATE META_DISPLAY_COMPACT=1
                META_STRIP_LINES=2 META_LCD_BUFFERS=2 META_MERGE_MODES=0)
            if(variant STREQUAL "short_tile")
                target_compile_definitions(${test_name} PRIVATE META_TBLEND=0)
            elseif(variant STREQUAL "short_tblend2")
                target_compile_definitions(${test_name} PRIVATE META_TBLEND=2)
            elseif(variant STREQUAL "short_12bpp")
                target_compile_definitions(${test_name} PRIVATE META_LCD_12BPP=1)
            endif()
        endif()
        add_test(NAME meta_display_${variant}_matches_reference COMMAND ${test_name})
    endforeach()

    add_executable(test_meta_menu_slices "${GBL_META_DIR}/tests/menu_slices.cpp"
        "${GBL_COMMON_DIR}/platform/m0_menu.cpp")
    target_include_directories(test_meta_menu_slices PRIVATE "${GBL_META_DIR}/tests/stubs"
        "${GBL_META_DIR}/tests/config" "${GBL_META_DIR}/runtime" "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform")
    target_compile_options(test_meta_menu_slices PRIVATE -UNDEBUG)
    add_test(NAME meta_menu_short_scratch_matches_full_band COMMAND test_meta_menu_slices)

    # m0_cache.inc against a card that refuses writes, and a menu lending the whole pool.
    add_executable(test_m0_cache_failures tests/test_m0_cache_failures.c)
    target_include_directories(test_m0_cache_failures PRIVATE targets/common/runtime)
    target_compile_options(test_m0_cache_failures PRIVATE -UNDEBUG -fsanitize=address,undefined)
    target_link_options(test_m0_cache_failures PRIVATE -fsanitize=address,undefined)
    add_test(NAME m0_cache_refused_write_back_and_full_lend COMMAND test_m0_cache_failures)

    # Cartridge RAM cache on the card: 8 KiB, and 32 KiB in four MBC5 banks.
    foreach(variant 1 2 8 8_32k 8_32k_dbg 8_stk)
        string(REGEX REPLACE "_.*" "" ways ${variant})
        add_executable(test_meta_cache_${variant} "${GBL_META_DIR}/tests/cache_lru.c" ${GBL_META_RUNTIME_SOURCES})
        target_include_directories(test_meta_cache_${variant} PRIVATE "${GBL_META_DIR}/tests/config_stream"
            "${GBL_META_DIR}/runtime")
        target_compile_definitions(test_meta_cache_${variant} PRIVATE GBL_ERAM_CACHE_SECTORS=${ways} GBL_CACHE_UNIFIED=0)
        if(variant MATCHES "_32k")
            target_compile_definitions(test_meta_cache_${variant} PRIVATE GBL_ERAM_SIZE=32768)
        endif()
        if(variant MATCHES "_dbg")
            target_compile_definitions(test_meta_cache_${variant} PRIVATE GBL_DEBUG_ERAM_MAPS=1 GBL_DEBUG_ERAM_SUM=1)
        endif()
        if(variant MATCHES "_stk")
            target_compile_definitions(test_meta_cache_${variant} PRIVATE GBL_DEBUG_STACK=1)
        endif()
        target_compile_options(test_meta_cache_${variant} PRIVATE -UNDEBUG)
        add_test(NAME meta_cache_lru_${variant}_ways_and_resident_bank0 COMMAND test_meta_cache_${variant})
    endforeach()

    # One pool for the streamed ROM and the cartridge RAM, per block size.
    foreach(block 16 32 64 128 256 512)
        add_executable(test_meta_cache_unified_${block} "${GBL_META_DIR}/tests/cache_unified.c" ${GBL_META_RUNTIME_SOURCES})
        target_include_directories(test_meta_cache_unified_${block} PRIVATE "${GBL_META_DIR}/tests/config_stream"
            "${GBL_META_DIR}/runtime")
        target_compile_definitions(test_meta_cache_unified_${block} PRIVATE GBL_CACHE_UNIFIED=1
            GBL_CACHE_BLOCK=${block} GBL_CACHE_BYTES=3072 GBL_CACHE_MENU_STACK=1024)
        target_compile_options(test_meta_cache_unified_${block} PRIVATE -UNDEBUG)
        add_test(NAME meta_cache_unified_${block}_rom_and_ram_one_pool COMMAND test_meta_cache_unified_${block})
    endforeach()

    # The cache's blocks after its fixed part (GBL_CACHE_DYN) coming and
    # going: the unified pool, and the ROM sectors beside the RAM's cache.
    foreach(variant unified_64 unified_128 unified_512 stream)
        add_executable(test_meta_cache_dyn_${variant} "${GBL_META_DIR}/tests/cache_unified.c" ${GBL_META_RUNTIME_SOURCES})
        target_include_directories(test_meta_cache_dyn_${variant} PRIVATE "${GBL_META_DIR}/tests/config_stream"
            "${GBL_META_DIR}/runtime")
        target_compile_definitions(test_meta_cache_dyn_${variant} PRIVATE GBL_CACHE_DYN=1 GBL_CACHE_DYN_INLINE=1)
        if(variant MATCHES "unified_")
            string(REPLACE "unified_" "" block ${variant})
            target_compile_definitions(test_meta_cache_dyn_${variant} PRIVATE GBL_CACHE_UNIFIED=1
                GBL_CACHE_BLOCK=${block} GBL_CACHE_BYTES=3072 GBL_CACHE_MENU_STACK=1024)
        else()
            target_compile_definitions(test_meta_cache_dyn_${variant} PRIVATE GBL_CACHE_UNIFIED=0
                GBL_STREAM_SECTORS=4 GBL_ERAM_CACHE_SECTORS=2)
        endif()
        target_compile_options(test_meta_cache_dyn_${variant} PRIVATE -UNDEBUG)
        add_test(NAME meta_cache_dyn_${variant}_blocks_come_and_go COMMAND test_meta_cache_dyn_${variant})
    endforeach()

    # The stack pool's policy over a simulated RAM (m0_stack_pool.h).
    add_executable(test_m0_stack_pool "${GBL_META_DIR}/tests/stack_pool.c")
    target_compile_options(test_m0_stack_pool PRIVATE -UNDEBUG)
    add_test(NAME m0_stack_pool_stack_first COMMAND test_m0_stack_pool)

    add_executable(test_meta_lcd_pack "${GBL_META_DIR}/tests/lcd_pack.c")
    target_include_directories(test_meta_lcd_pack PRIVATE "${GBL_CONSOLES_DIR}/meta")
    add_test(NAME meta_lcd_pack444_matches_st7735 COMMAND test_meta_lcd_pack)

    add_executable(test_meta_apu_lfsr "${GBL_META_DIR}/tests/apu_lfsr.c")
    target_include_directories(test_meta_apu_lfsr PRIVATE "${GBL_META_DIR}/runtime")
    add_test(NAME meta_apu_lfsr_matches_per_clock COMMAND test_meta_apu_lfsr)

    add_test(NAME meta_target_end_to_end
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_target_e2e.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_target_end_to_end PROPERTIES TIMEOUT 600)

    # ESPboy: raw/partly packed flash images, precise runtime parity, no SD.
    add_test(NAME meta_espboy_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_espboy_target.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_espboy_target PROPERTIES TIMEOUT 300)

    # PC (SDL2): the firmware is built and run head first on the CI machine;
    # the build is skipped when SDL2 is missing.
    add_test(NAME meta_pc_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_pc_target.py"
                "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_pc_target PROPERTIES TIMEOUT 600)

    # ODROID-GO and Gamebuino AKA: project ownership and host parity; the
    # real ESP32 build runs when ODROID_PIO / AKA_PIO points at pio.
    add_test(NAME meta_odroid_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_odroid_target.py"
                "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_odroid_target PROPERTIES TIMEOUT 600)
    add_test(NAME meta_aka_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_aka_target.py"
                "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_aka_target PROPERTIES TIMEOUT 600)
    add_test(NAME sega_aka_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_sega_aka_target.py"
                "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(sega_aka_target PROPERTIES TIMEOUT 600)

    # Pokitto firmware also links when arm-none-eabi-gcc exists.
    add_test(NAME meta_pokitto_target
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_pokitto_target.py"
                --gbrecomp "$<TARGET_FILE:gbrecomp>")
    set_tests_properties(meta_pokitto_target PROPERTIES TIMEOUT 300)

    add_executable(test_meta_fat_check "${GBL_META_DIR}/tests/fat_check.c"
        "${GBL_COMMON_DIR}/platform/m0_fat.c")
    target_include_directories(test_meta_fat_check PRIVATE "${GBL_META_DIR}/platform" "${GBL_COMMON_DIR}/platform")
    add_test(NAME m0_fat_finds_streamed_rom
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_meta_fat.py"
                --fat-check "$<TARGET_FILE:test_meta_fat_check>")
endif()
