/* The console project files the SG-1000, PICO-8 and NES generators share
 * (m0boy/console_project.h). */
#include "m0boy/console_project.h"
#include <fstream>
#include <stdexcept>

namespace m0boy::project {
namespace fs = std::filesystem;

std::string read(const fs::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read template: " + path.string());
    return { std::istreambuf_iterator<char>(file), {} };
}

void write(const fs::path& path, const std::string& data) {
    std::ofstream file(path);
    if (!file || !(file << data)) throw std::runtime_error("Cannot write generated file: " + path.string());
}

void copy_template(const fs::path& from, const fs::path& to) {
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

std::string replace(std::string s, const std::string& what, const std::string& value) {
    size_t pos = 0;
    while ((pos = s.find(what, pos)) != std::string::npos) {
        s.replace(pos, what.size(), value);
        pos += value.size();
    }
    return s;
}

fs::path Templates::console(TargetId target) const {
    const auto* info = m0boy::consoles::find(target);
    if (!info || info->console_dir.empty()) throw std::runtime_error("No console directory for this target");
    return targets / "consoles" / std::string(info->console_dir);
}

void copy_frontend(const Templates& t, const fs::path& src) {
    const auto shared = t.frontend();
    for (const char* name : { "sg_frontend.h", "sg_menu.h", "m0_target.h", "m0_lcd.h", "m0_clock.h", "m0_pad.h",
                              "m0_rows.h", "m0_menu.h", "m0_font.h", "m0_menu.cpp" })
        copy_template(shared / name, src / name);
    copy_template(t.targets / "meta" / "runtime" / "gbl_board.h", src / "gbl_board.h");
}

void write_audio(const Templates& t, TargetId target, const fs::path& src, const std::string& frame_cycles,
                 const std::string& cpu_hz, const std::string& header, const std::string& render,
                 const std::string& set_rate) {
    /* m0_audio_frame_samples' constants are the Game Boy's */
    write(src / "m0_audio.h",
          replace(replace(read(t.frontend() / "m0_audio.h"), "70224u", frame_cycles), "4194304u", cpu_hz));
    const char* driver = target == TargetId::meta ? "meta_audio.cpp" : target == TargetId::pokitto ? "pokitto_audio.c" : nullptr;
    if (!driver) return;
    auto audio = replace(read(t.console(target) / driver), "#include \"gbl.h\"", "#include \"" + header + "\"");
    audio = replace(replace(audio, "gbl_apu_render", render), "gbl_apu_set_rate", set_rate);
    write(src / driver, audio);
}

void write_config(TargetId target, const fs::path& src) {
    /* One version stamp for every frontend: the target console runtime's
     * version, compiled into m0recomp from the VERSION files
     * (targets/meta/VERSION, consoles/<console>/VERSION) by CMake — one
     * table, m0boy::consoles.  m0recomp --meta-version/--pokitto-version
     * print it and compile.sh reads it back from firmware.bin. */
    write(src / "gbl_config.h", "#pragma once\n#define GBL_TARGET_POKITTO " + std::to_string(target == TargetId::pokitto) +
                                (target == TargetId::espboy ? "\n#define GBL_TARGET_ESPBOY 1" : "") +
                                (target == TargetId::pc ? "\n#define GBL_TARGET_PC 1" : "") +
                                (target == TargetId::odroid ? "\n#define GBL_TARGET_ODROID 1" : "") +
                                (target == TargetId::aka ? "\n#define GBL_TARGET_AKA 1" : "") +
                                "\n#define GBL_GEN_VERSION \"" + std::string(m0boy::consoles::version(target)) + "\"\n");
}

void write_profile(TargetId target, const fs::path& src, PokittoSwitches pokitto, bool ring_everywhere) {
    /* gbl_board.h carries the META's SPI rates and display pipeline
     * switches, shared with the Game Boy profile; only what this build
     * decides differently is written here.  META_OVERCLOCK: on by default
     * on the META (70 MHz, as in the Game Boy profile), off on the Pokitto
     * unless POKITTO_HC. */
    const bool esp32 = target == TargetId::odroid || target == TargetId::aka;
    const std::string overclock = target == TargetId::pokitto
        ? "#ifndef META_OVERCLOCK\n#if defined(POKITTO_HC) && POKITTO_HC\n#define META_OVERCLOCK 1\n#else\n#define META_OVERCLOCK 0\n#endif\n#endif\n"
        : target == TargetId::pc || esp32 ? "#define META_OVERCLOCK 0\n"
        : "#ifndef META_OVERCLOCK\n#define META_OVERCLOCK 1\n#endif\n";
    /* META: one ring of META_AUDIO_RING samples of 2 bytes (512, about 1.4
     * frames, lowered so the card cache can grow) feeds both output paths,
     * the TC4 interrupt by default and, at run time (the menus' Audio line)
     * or at build (META_AUDIO_DMA=1, compile.sh AUDIO_DMA=1), a DMAC beat.
     * The Pokitto has its own driver. */
    const std::string audio = target == TargetId::meta || ring_everywhere
        ? "#ifndef META_AUDIO_DMA\n#define META_AUDIO_DMA 0\n#endif\n#ifndef META_AUDIO_RING\n#define META_AUDIO_RING 512u\n#endif\n"
        : "#define META_AUDIO_DMA 0\n";
    auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
    write(src / "gbl_profile.h", "#pragma once\n#include \"gbl_board.h\"\n" + overclock +
                                 "#define POKITTO_LCD_ASM " + flag(pokitto.lcd_asm) +
                                 "\n#define POKITTO_RAM_BANKS " + flag(pokitto.ram_banks) +
                                 "\n#define POKITTO_AUDIO_ASM " + flag(pokitto.audio_asm) + "\n" + audio);
}

void copy_toolchain(const Templates& t, const fs::path& output) {
    fs::create_directories(output / "toolchain");
    for (const auto& entry : fs::directory_iterator(t.common() / "toolchain"))
        if (entry.is_regular_file()) copy_template(entry.path(), output / "toolchain" / entry.path().filename());
}

void copy_meta_drivers(const Templates& t, const fs::path& src, bool lcd) {
    const auto platform = t.console(TargetId::meta);
    for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                              "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h",
                              "meta_usb_msc.c", "meta_usb_msc.h" })
        copy_template(platform / name, src / name);
    if (lcd) copy_template(platform / "meta_lcd.cpp", src / "meta_lcd.cpp");
}

void write_meta_project(const Templates& t, const fs::path& output, const std::string& name,
                        const std::string& compiler, const std::string& build_flags) {
    const auto platform = t.console(TargetId::meta);
    auto ini = replace(replace(read(platform / "platformio.ini.in"), "@NAME@", name), "@COMPILER@", compiler);
    if (!build_flags.empty()) ini = replace(ini, "build_flags = ", "build_flags = " + build_flags + " ");
    write(output / "platformio.ini", ini);
    write(output / "gamebuino_meta.ld", replace(read(platform / "gamebuino_meta.ld.in"), "@NAME@", name));
    fs::create_directories(output / "boards");
    copy_template(platform / "boards" / "gamebuino_meta.json", output / "boards" / "gamebuino_meta.json");
    copy_toolchain(t, output);
}

void copy_pokitto_drivers(const Templates& t, const fs::path& src, std::initializer_list<const char*> extra) {
    const auto platform = t.console(TargetId::pokitto);
    for (const char* name : { "pokitto_startup.c", "pokitto_clock.c", "pokitto_buttons.c", "pokitto_lcd.c", "pokitto_hw.h", "lpc11u6x.h" })
        copy_template(platform / name, src / name);
    for (const char* name : extra) copy_template(platform / name, src / name);
}

std::string pokitto_ld(const Templates& t, const std::string& name) {
    return replace(read(t.console(TargetId::pokitto) / "pokitto.ld.in"), "@NAME@", name);
}

void write_pokitto_project(const Templates& t, const fs::path& output, const std::string& name,
                           const std::string& compiler, const std::string& system, const std::string& sources,
                           const std::string& platform_extra, const std::string& ld) {
    write(output / "pokitto.ld", ld);
    /* the LPC boot checksum and the ARM toolchain file: the Sega projects' */
    copy_template(t.targets / "sg1000" / "checksum.py", output / "checksum.py");
    copy_template(t.targets / "sg1000" / "arm-toolchain.cmake", output / "arm-toolchain.cmake");
    copy_toolchain(t, output);
    std::string cmake = read(t.common() / "cmake" / "firmware.txt.in");
    cmake = replace(cmake, "@NAME@", name);
    cmake = replace(cmake, "@SYSTEM@", system);
    cmake = replace(cmake, "@SOURCES@", sources);
    cmake = replace(cmake, "@PLATFORM_EXTRA@", platform_extra);
    cmake = replace(cmake, "@COMPILER@", compiler);
    write(output / "CMakeLists.txt", cmake);
}

} // namespace m0boy::project
