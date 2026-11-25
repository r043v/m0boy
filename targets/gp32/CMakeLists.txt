cmake_minimum_required(VERSION 3.16)
project(m0boy_gp32 LANGUAGES C CXX)

set(GP32EMU_DIR "$ENV{GP32EMU_DIR}" CACHE PATH "Path to the gp32emu sources")
include(gp32_defaults.cmake)
option(M0_GP32_WINDOW "Build the SDL3 frontend" ${M0_GP32_WINDOW_DEFAULT})
if(NOT EXISTS "${GP32EMU_DIR}/include/gp32emu/gp32.h")
    message(FATAL_ERROR "Set GP32EMU_DIR to the gp32emu source checkout. See docs/GP32.fr.md in m0boy.")
endif()

set(GP32EMU_BUILD_SDL3 "${M0_GP32_WINDOW}")
add_subdirectory("${GP32EMU_DIR}" gp32emu)
set_target_properties(gp32_headless PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(M0_GP32_CARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/smartmedia" CACHE PATH "Local GP32 SmartMedia folder")
set(M0_GP32_CARD_IDENTITY "" CACHE FILEPATH "32 MiB GeePee32 SMC to copy the local card identity from")
set(M0_GP32_PDUID "" CACHE STRING "Emulated GP32 console ID as 32 hexadecimal digits")
set(M0_GP32_FRAMES 180 CACHE STRING "Number of frames for run_headless")
set(M0_GP32_BIOS "" CACHE FILEPATH "GP32 BIOS for SmartMedia boot")
set(M0_GP32_SMC "" CACHE FILEPATH "SmartMedia image to boot instead of game.fxe")
option(M0_GP32_DUMP_AUDIO "Capture audio to audio.wav in run_headless" OFF)

# Apply the fixes to generated copies so the pinned checkout can be shared.
function(gp32_read_source name variable)
    set(source "${GP32EMU_DIR}/src/${name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" contents)
    set(${variable} "${contents}" PARENT_SCOPE)
endfunction()

function(gp32_replace variable before after)
    string(FIND "${${variable}}" "${before}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Unsupported gp32emu sources. Use the revision in docs/GP32.fr.md.")
    endif()
    string(REPLACE "${before}" "${after}" contents "${${variable}}")
    set(${variable} "${contents}" PARENT_SCOPE)
endfunction()

function(gp32_use_source name contents)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    file(WRITE "${output}.in" "${contents}")
    configure_file("${output}.in" "${output}" COPYONLY)
    get_target_property(sources gp32emu SOURCES)
    list(REMOVE_ITEM sources "src/${name}")
    set_property(TARGET gp32emu PROPERTY SOURCES ${sources} "${output}")
endfunction()

gp32_read_source(gp32.c gp32_core_source)
# Mix at unity for one source and share the available range when sources overlap.
gp32_replace(gp32_core_source
    "if (direct_hle_sef_sample(g, out_rate, &left, &right)) active = 1;"
    "active += direct_hle_sef_sample(g, out_rate, &left, &right);")
gp32_replace(gp32_core_source
    "if (direct_pcm_channel_sample(g, ch, out_rate, &left, &right)) active = 1;"
    "active += direct_pcm_channel_sample(g, ch, out_rate, &left, &right);")
gp32_replace(gp32_core_source
    "direct_mix_clamp_i16(left), direct_mix_clamp_i16(right)"
    "direct_mix_clamp_i16(left / active), direct_mix_clamp_i16(right / active)")

# Completed GPG reads supersede the fallback that searches for guest pointers.
gp32_replace(gp32_core_source
    "static int direct_buffer_looks_unloaded(gp32_t *g, uint32_t buf, uint32_t len) {"
    [=[
static void direct_complete_asset_read(gp32_t *g, const fpk_asset_t *asset, size_t offset, size_t count) {
    if (offset > 8u || offset + count != asset->size) return;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (g->direct_hle_asset_autoload[i].asset == asset)
            g->direct_hle_asset_autoload[i].copied = 1u;
    }
}

static int direct_buffer_looks_unloaded(gp32_t *g, uint32_t buf, uint32_t len) {
]=])
gp32_replace(gp32_core_source
    "for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);"
    [=[
for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);
            direct_complete_asset_read(g, a, *posp, n);
]=])
gp32_replace(gp32_core_source
    "        char lit[64];\n        if (!direct_read_cstr(g, saddr, lit, sizeof(lit))) continue;"
    [=[
        uint8_t first = (uint8_t)(s3c2400_debug_read32(g->soc, saddr & ~3u) >> ((saddr & 3u) * 8u));
        if (tolower(first) != (unsigned char)basename[0]) continue;
        char lit[64];
        if (!direct_read_cstr(g, saddr, lit, sizeof(lit))) continue;
]=])

# Keep device IRQ delivery close to the LCD lines polled by guest code.
gp32_replace(gp32_core_source
    "uint32_t slice = remaining > 32768u ? 32768u : remaining;"
    "uint32_t slice = remaining > 1024u ? 1024u : remaining;")

# LCDCON1 is polled by guest software during an interpreter slice.
gp32_read_source(arm920t.c gp32_cpu_source)
gp32_replace(gp32_cpu_source [=[
        if (batch) done += batch;
        else { if (thumb(c)) exec_thumb(c); else exec_arm(c); done += 1; }
    }
    c->cycles_total += done;
]=] [=[
        if (!batch) {
            if (thumb(c)) exec_thumb(c);
            else exec_arm(c);
            batch = 1;
        }
        done += batch;
        c->cycles_total += batch;
    }
]=])
gp32_use_source(arm920t.c "${gp32_cpu_source}")

gp32_read_source(s3c2400.c gp32_soc_source)
gp32_replace(gp32_soc_source
    "return h ? h : f;"
    "return f;")
gp32_replace(gp32_soc_source
    "(cnt - cmp + 1u)"
    "(cnt - cmp)")

# Convert the TFT pixel clock and programmed blanking intervals to CPU cycles.
gp32_replace(gp32_soc_source
    "static uint64_t lcd_panel_frame_cycles(const s3c2400_t *s) {"
    [=[
static bool lcd_tft_timing(const s3c2400_t *s) {
    return GP32_BITS(s->lcd_regs[0], 6, 5) == 3u && GP32_BITS(s->lcd_regs[0], 17, 8);
}

static uint32_t lcd_scan_lines(const s3c2400_t *s, uint32_t visible) {
    if (!lcd_tft_timing(s)) return lcd_total_lines_for_visible(visible);
    return visible + GP32_BITS(s->lcd_regs[1], 31, 24)
        + GP32_BITS(s->lcd_regs[1], 13, 6) + GP32_BITS(s->lcd_regs[1], 5, 0) + 3u;
}

static uint64_t lcd_panel_frame_cycles(const s3c2400_t *s) {
]=])
gp32_replace(gp32_soc_source
    "uint64_t frame_cycles = ((uint64_t)runclk + 30u) / 60u;"
    [=[
uint32_t hclk = clk_hclk(s, MPLLCON);
    if (hclk && lcd_tft_timing(s)) {
        uint32_t pixels = GP32_BITS(s->lcd_regs[2], 25, 19) + GP32_BITS(s->lcd_regs[2], 18, 8)
            + GP32_BITS(s->lcd_regs[2], 7, 0) + GP32_BITS(s->lcd_regs[3], 7, 0) + 4u;
        uint32_t lines = lcd_scan_lines(s, lcd_visible_lines(s));
        uint32_t divider = 2u * (GP32_BITS(s->lcd_regs[0], 17, 8) + 1u);
        return (uint64_t)pixels * lines * divider * runclk / hclk;
    }
    uint64_t frame_cycles = ((uint64_t)runclk + 30u) / 60u;
]=])
gp32_replace(gp32_soc_source
    "uint32_t total_lines = lcd_total_lines_for_visible(visible);"
    "uint32_t total_lines = lcd_scan_lines(s, visible);")
gp32_replace(gp32_soc_source
    "uint64_t line64 = (s->lcd_line_accum % frame_cycles) / line_cycles;"
    [=[
uint64_t cpu_cycles = arm920t_get_cycles(s->cpu_irq_sink);
    uint64_t elapsed = cpu_cycles > s->lcd_line_accum ? cpu_cycles : s->lcd_line_accum;
    uint64_t line64 = (elapsed % frame_cycles) / line_cycles;
]=])

# LCDCON5.FRM565 selects RGB565 for 16-bit TFT pixels. Indexed modes
# still use the LCD palette's RGB555 with the shared intensity bit.
gp32_replace(gp32_soc_source
    "static uint8_t *ram_ptr(s3c2400_t *s, uint32_t addr, size_t bytes) {"
    [=[
static uint32_t color_rgb565(uint16_t data) {
    uint32_t r = (data >> 11) & 31u;
    uint32_t b = data & 31u;
    return 0xff000000u | ((r << 3 | r >> 2) << 16)
        | ((uint32_t)expand6(data >> 5) << 8) | (b << 3 | b >> 2);
}

static uint8_t *ram_ptr(s3c2400_t *s, uint32_t addr, size_t bytes) {
]=])
gp32_replace(gp32_soc_source
    "if(bits==16){ color=color_lcd5551((uint16_t)(d>>16)); d<<=16; }"
    [=[
if (bits == 16) {
                uint16_t pixel = (uint16_t)(d >> 16);
                color = GP32_BIT(s->lcd_regs[4], 11) ? color_rgb565(pixel) : color_lcd5551(pixel);
                d <<= 16;
            }
]=])

# Present LCD memory and palette writes made since the last frame boundary.
gp32_replace(gp32_soc_source
    "if (s->frame_counter == 0u && (s->lcd_regs[0] & 1u)) s3c2400_render_lcd(s);"
    "if (s->lcd_regs[0] & 1u) s3c2400_render_lcd(s);")

if(NOT M0_GP32_PDUID STREQUAL "")
    string(LENGTH "${M0_GP32_PDUID}" gp32_pduid_length)
    if(NOT gp32_pduid_length EQUAL 32 OR NOT M0_GP32_PDUID MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "M0_GP32_PDUID must contain exactly 32 hexadecimal digits.")
    endif()

    # The 1.57E firmware decodes the EEPROM at 0x10 with a repeated XOR mask.
    set(gp32_pduid_mask 0x53 0x41 0x4e 0x47 0x48 0x59 0x55 0x4b)
    set(gp32_pduid_eeprom "")
    foreach(index RANGE 0 15)
        math(EXPR position "${index} * 2")
        math(EXPR mask_index "${index} % 8")
        string(SUBSTRING "${M0_GP32_PDUID}" ${position} 2 byte)
        list(GET gp32_pduid_mask ${mask_index} mask)
        math(EXPR encoded "0x${byte} ^ ${mask}" OUTPUT_FORMAT HEXADECIMAL)
        string(APPEND gp32_pduid_eeprom "${encoded}, ")
    endforeach()

    set(gp32_eeprom_init "memset(s->eeprom, 0xff, sizeof(s->eeprom));")
    string(FIND "${gp32_soc_source}" "${gp32_eeprom_init}" gp32_eeprom_init_offset)
    if(gp32_eeprom_init_offset EQUAL -1)
        message(FATAL_ERROR "Unsupported gp32emu sources. Use the revision in docs/GP32.fr.md.")
    endif()
    string(REPLACE "${gp32_eeprom_init}"
        "${gp32_eeprom_init}\n    static const uint8_t pduid_eeprom[] = { ${gp32_pduid_eeprom}};\n    memcpy(s->eeprom + 0x10, pduid_eeprom, sizeof(pduid_eeprom));"
        gp32_soc_source "${gp32_soc_source}")
    # The firmware changes from transmit to receive without clearing START.
    # Its next ACK clocks the receive address before the first data byte.
    set(gp32_iic_start "if (!(old & 0x20u) && (now & 0x20u)) iic_start(s);")
    string(FIND "${gp32_soc_source}" "${gp32_iic_start}" gp32_iic_start_offset)
    if(gp32_iic_start_offset EQUAL -1)
        message(FATAL_ERROR "Unsupported gp32emu sources. Use the revision in docs/GP32.fr.md.")
    endif()
    string(REPLACE "${gp32_iic_start}"
        "${gp32_iic_start}\n                else if ((old & 0xe0u) == 0xe0u && (now & 0xe0u) == 0xa0u) s->iic_data_index = 0;"
        gp32_soc_source "${gp32_soc_source}")
endif()
gp32_use_source(s3c2400.c "${gp32_soc_source}")

set(gp32_media_args --fxe "${CMAKE_CURRENT_SOURCE_DIR}/game.fxe")
if(M0_GP32_SMC)
    set(gp32_media_args --smc "${M0_GP32_SMC}")
    if(M0_GP32_BIOS)
        list(APPEND gp32_media_args --bios "${M0_GP32_BIOS}")
    endif()
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(M0_GP32_MKFS mkfs.fat)
    find_program(M0_GP32_MCOPY mcopy)
    if(NOT M0_GP32_MKFS OR NOT M0_GP32_MCOPY)
        message(FATAL_ERROR "The local GP32 card requires mkfs.fat (dosfstools) and mcopy (mtools).")
    endif()
    set(gp32_card_image "${CMAKE_BINARY_DIR}/local.smc")
    set(gp32_card_identity_args)
    if(M0_GP32_CARD_IDENTITY)
        list(APPEND gp32_card_identity_args --identity "${M0_GP32_CARD_IDENTITY}")
    endif()
    add_custom_target(gp32_local_card
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_card.py"
                "${M0_GP32_CARD_DIR}" "${gp32_card_image}" "${M0_GP32_MKFS}" "${M0_GP32_MCOPY}" ${gp32_card_identity_args}
        BYPRODUCTS "${gp32_card_image}" VERBATIM)
    set(gp32_card_dependency gp32_local_card)
    if(M0_GP32_BIOS)
        set(gp32_media_args --bios "${M0_GP32_BIOS}" --smc "${gp32_card_image}")
    else()
        set(gp32_launch ${CMAKE_COMMAND} -E env "M0_GP32_LOCAL_CARD=${gp32_card_image}")

        # Attach the card before FXE setup, without selecting gp32emu's SMC boot path.
        set(gp32_fxe_load "return gp32_load_fxe_image_internal(g, img, 1, 0, 0, 0);")
        string(FIND "${gp32_core_source}" "${gp32_fxe_load}" gp32_fxe_offset)
        if(gp32_fxe_offset EQUAL -1)
            message(FATAL_ERROR "Unsupported gp32emu sources. Use the revision in docs/GP32.fr.md.")
        endif()
        string(REPLACE "${gp32_fxe_load}" [=[
        const char *card = getenv("M0_GP32_LOCAL_CARD");
        if (!card || !*card) return gp32_load_fxe_image_internal(g, img, 1, 0, 0, 0);
        gp32_status_t status = gp32_load_smartmedia(g, card);
        if (status != GP32_OK) return status;
        status = gp32_load_fxe_image_internal(g, img, 1, 0, 1, 0);
        if (status == GP32_OK) direct_init_smc_gpio(g);
        return status;
    ]=] gp32_core_source "${gp32_core_source}")
    endif()
endif()
gp32_use_source(gp32.c "${gp32_core_source}")

set(gp32_audio_args)
if(M0_GP32_DUMP_AUDIO)
    list(APPEND gp32_audio_args --dump-wav audio.wav)
endif()

add_custom_target(run_headless
    COMMAND ${gp32_launch} $<TARGET_FILE:gp32_headless> ${gp32_media_args} ${gp32_audio_args}
            --no-jit --no-bios-auto-start --frames "${M0_GP32_FRAMES}" --rotate-ccw --dump-frame frame.ppm
    DEPENDS gp32_headless ${gp32_card_dependency} VERBATIM)

if(M0_GP32_WINDOW)
    get_target_property(gp32_sdl_sources gp32emu_sdl3 SOURCES)
    list(REMOVE_ITEM gp32_sdl_sources src/platform/sdl3/input.c)
    set_property(TARGET gp32emu_sdl3 PROPERTY SOURCES
        ${gp32_sdl_sources} "${CMAKE_CURRENT_SOURCE_DIR}/sdl_input.c")

    # A startup replay may launch a game while the player uses live controls.
    set(gp32_sdl_main "${GP32EMU_DIR}/src/sdl3_main.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gp32_sdl_main}")
    file(READ "${gp32_sdl_main}" gp32_sdl_source)
    string(REPLACE "input_script ? gp32_input_script_frame(input_script, frame_index) : physical_buttons"
        "physical_buttons | (input_script ? gp32_input_script_frame(input_script, frame_index) : 0)"
        gp32_sdl_source "${gp32_sdl_source}")
    string(REPLACE "    o->rotate = 1;"
        "    o->rotate = 1;\n    o->enable_joystick = 1;\n    o->enable_joystick_axis = 1;"
        gp32_sdl_source "${gp32_sdl_source}")
    # A quit request can arrive with a full frame pending. Leave before
    # pacing subtracts the pending time from an unsigned frame interval.
    gp32_replace(gp32_sdl_source
        "        uint32_t actions = gp32_input_take_actions(input);"
        "        if (quit) break;\n        uint32_t actions = gp32_input_take_actions(input);")
    set(gp32_sdl_patched "${CMAKE_BINARY_DIR}/gp32_sdl_main.c")
    file(WRITE "${gp32_sdl_patched}.in" "${gp32_sdl_source}")
    configure_file("${gp32_sdl_patched}.in" "${gp32_sdl_patched}" COPYONLY)
    set_property(TARGET gp32_sdl3 PROPERTY SOURCES "${gp32_sdl_patched}")

    set_target_properties(gp32_sdl3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_custom_target(run
        COMMAND ${gp32_launch} $<TARGET_FILE:gp32_sdl3> ${gp32_media_args} --no-jit
        DEPENDS gp32_sdl3 ${gp32_card_dependency} VERBATIM)
endif()
