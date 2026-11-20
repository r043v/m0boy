/* Vectrex project generation: the interpreter core (targets/vectrex/runtime)
 * with the cartridge as data, and either the SDL host (pc: the BIOS is read
 * at run time) or a console firmware with the BIOS built in (the META, or
 * the Gamebuino AKA on the ESP32 family's shared core; neither ever enters
 * the repository). */
#include "m0boy/rom_external.h"
#include "m0boy/vectrex_generator.h"
#include "m0boy/vectrex_emitter.h"
#include "m0boy/console_project.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifndef GBRECOMP_META_VERSION
#define GBRECOMP_META_VERSION "dev"
#endif
namespace m0boy::vectrex {
namespace fs = std::filesystem;
namespace {

std::string read(const fs::path &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("Cannot read template: " + path.string());
  return {std::istreambuf_iterator<char>(file), {}};
}
std::vector<unsigned char> read_binary(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(file), {}};
}
void write(const fs::path &path, const std::string &data) {
  std::ofstream file(path);
  if (!file || !(file << data))
    throw std::runtime_error("Cannot write generated file: " + path.string());
}
std::string state_base(const std::string &name) {
  std::string out;
  for (unsigned char c : name)
    if (std::isalnum(c) && out.size() < 8) out += static_cast<char>(std::toupper(c));
  return out.empty() ? "CART" : out;
}
std::string name83(std::string base, std::string ext) {
  base.resize(8, ' ');
  ext.resize(3, ' ');
  return base + ext;
}
void copy_template(const fs::path &from, const fs::path &to) {
  fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}
std::string replace(std::string s, const std::string &what,
                    const std::string &value) {
  size_t pos = 0;
  while ((pos = s.find(what, pos)) != std::string::npos) {
    s.replace(pos, what.size(), value);
    pos += value.size();
  }
  return s;
}
std::string bytes(const unsigned char *data, size_t n) {
  std::ostringstream out;
  for (size_t i = 0; i < n; i++) {
    char b[8];
    snprintf(b, sizeof b, "0x%02x,", data[i]);
    out << b;
    if ((i & 15) == 15 || i + 1 == n)
      out << "\n";
  }
  return out.str();
}
/* --bios, or the one 4/8 KiB file named *bios* beside the cartridge;
 * `required` false: an empty path when there is none (or several) */
fs::path find_bios(const fs::path &cartridge, const fs::path &given, bool required = true) {
  if (!given.empty())
    return given;
  std::vector<fs::path> found;
  const fs::path dir = cartridge.has_parent_path() ? cartridge.parent_path() : ".";
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;
    std::string name = entry.path().filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const auto size = entry.file_size();
    if (name.find("bios") != std::string::npos && (size == 4096 || size == 8192))
      found.push_back(entry.path());
  }
  if (found.size() != 1 && !required)
    return {};
  if (found.size() != 1)
    throw std::runtime_error(
        found.empty() ? "The META firmware needs the Vectrex system ROM: --bios FILE (4 or 8 KiB)"
                      : "Several *bios* files next to the cartridge: choose one with --bios FILE");
  return found.front();
}

/* the system ROM at E000-FFFF, a 4 KiB Executive in the upper half */
std::vector<unsigned char> bios_image(const fs::path &path) {
  const auto bios = read_binary(path);
  if (bios.size() != 4096 && bios.size() != 8192)
    throw std::runtime_error("Vectrex system ROM must be 4096 or 8192 bytes: " + path.string());
  std::vector<unsigned char> image(8192, 0xff);
  std::copy(bios.begin(), bios.end(), image.begin() + (8192 - bios.size()));
  return image;
}
/* the host's --coverage: "XXXX hits" lines after a "# m0boy.vectrex.coverage" header */
std::vector<CoverageEntry> read_coverage(const std::vector<fs::path> &files) {
  std::vector<CoverageEntry> out;
  for (const auto &path : files) {
    std::ifstream in(path);
    if (!in)
      throw std::runtime_error("Cannot read coverage file: " + path.string());
    std::string line;
    if (!std::getline(in, line) || line.rfind("# m0boy.vectrex.coverage", 0) != 0)
      throw std::runtime_error("Not a Vectrex coverage file: " + path.string());
    while (std::getline(in, line)) {
      unsigned pc = 0, hits = 0;
      if (sscanf(line.c_str(), "%x %u", &pc, &hits) == 2 && pc < 0x10000)
        out.push_back({uint16_t(pc), hits});
    }
  }
  return out;
}

} // namespace

bool generate(const fs::path &cartridge, const GenerateOptions &o,
              GenerateStats &stats, std::string &error) {
  try {
    if (o.target != TargetId::pc && o.target != TargetId::meta && o.target != TargetId::aka)
      throw std::runtime_error("Vectrex projects target the META (--target meta), the PC (--target pc) or the AKA (--target aka)");
    const auto rom = read_binary(cartridge);
    if (rom.empty() || rom.size() > 32768)
      throw std::runtime_error("Vectrex cartridge must be 1 to 32768 bytes");
    const fs::path tpl = o.targets_dir / "vectrex";
    if (!fs::is_regular_file(tpl / "runtime" / "vectrex.c"))
      throw std::runtime_error("Vectrex templates missing in " + o.targets_dir.string());
    const std::string target = o.target == TargetId::meta ? "meta" : o.target == TargetId::aka ? "aka" : "pc";
    const fs::path marker = o.output / "vectrex-project.txt";
    if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
      bool foreign = false; /* compile.sh writes its log first */
      for (const auto &e : fs::directory_iterator(o.output))
        foreign = foreign || e.path().filename() != "compile.log";
      if (foreign)
        throw std::runtime_error("Output directory is not a Vectrex project; choose a fresh directory");
    }
    if (fs::exists(marker) && read(marker) != target + "\n")
      throw std::runtime_error("Use a separate output directory for each destination");
    const fs::path src = o.output / "src";
    fs::create_directories(src);
    for (const char *file : {"vectrex.h", "vectrex.c", "m6809.c", "m6809_run.c", "via.c", "psg.c", "vx_gen.h",
                             "via_asm.S", "vx_via_asm.h"})
      copy_template(tpl / "runtime" / file, src / file);

    /* the system ROM: built into a console firmware; on the PC, when one
     * is at hand, its code is compiled too (the host checks the ROM it
     * loads against it) */
    std::vector<unsigned char> bios;
    if (o.target != TargetId::pc)
      bios = bios_image(find_bios(cartridge, o.bios));
    else if (!o.interpret) {
      const auto path = find_bios(cartridge, o.bios, false);
      if (!path.empty())
        bios = bios_image(path);
    }
    if (!o.interpret) {
      EmitOptions eo;
      eo.precise = o.precise;
      eo.max_instructions = o.max_instructions;
      eo.sweep = o.sweep;
      /* a console's flash less the runtime, the drivers and a margin: the
       * rest for as many instructions as it holds, played ones first.
       * 42 to 50 bytes an instruction in the fast mode (arm-gcc -Os: Vecmania
       * Part 2, Berzerk),
       * the precise mode's check after each one adds ~12 */
      if (o.target != TargetId::pc && !eo.max_instructions) {
        const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : o.precise ? 64u : 46u;
        const size_t budget = o.code_budget_bytes ? o.code_budget_bytes
                              : o.target == TargetId::aka
                                  ? (3520u - 192u) * 1024u - rom.size()
                                  : (240u - 56u - 4u) * 1024u - rom.size();
        eo.max_instructions = budget / bpi;
      }
      EmitStats es;
      const std::vector<uint8_t> cart_bytes(rom.begin(), rom.end()), bios_bytes(bios.begin(), bios.end());
      write(src / "vx_gen.c", emit_code(cart_bytes, bios_bytes, read_coverage(o.coverage), eo, es));
      stats.compiled = es.compiled;
      stats.covered = es.covered;
      stats.regions = es.regions;
      stats.entries = es.entries;
      stats.bios_compiled = !bios.empty();
    } else if (fs::exists(src / "vx_gen.c")) {
      fs::remove(src / "vx_gen.c");
    }
    const std::string native = o.interpret ? "" : "#define VX_NATIVE 1\n";

    std::string cart = "/* The cartridge, generated by m0recomp. */\n#include <stdint.h>\n"
                       "const unsigned vectrex_cart_size = " + std::to_string(rom.size()) +
                       ";\nconst uint8_t vectrex_cart[] = {\n" + bytes(rom.data(), rom.size()) + "};\n";

    if (o.target == TargetId::pc) {
      const bool ext_rom = g_rom_external;
      if (ext_rom) {
        /* --rom-external: no cartridge bytes in the project; the window loads it (m0_rom_ext.h) */
        write(src / "vx_rom_id.h", rom_id_header("VX", rom.data(), rom.size(), o.name));
        copy_template(o.targets_dir / "common/runtime/m0_rom_ext.h", src / "m0_rom_ext.h");
        cart = "#include <stdint.h>\nconst unsigned vectrex_cart_size = " + std::to_string(rom.size()) +
               ";\nconst uint8_t *vectrex_cart; /* set at start-up from the ROM file */\n";
      }
      write(src / "vx_cart.c", cart);
      write(src / "vx_config.h", "#pragma once\n" + native + (ext_rom ? "#define M0_ROM_EXTERNAL 1\n" : "") +
                                     "/* the host's --coverage and the dispatcher's counts */\n"
                                     "#define VX_COVERAGE 1\n#define VX_GEN_STATS 1\n");
      copy_template(tpl / "host" / "vectrex_host.c", src / "vectrex_host.c");
      /* the harness's --explore player */
      copy_template(o.targets_dir / "common/host" / "m0_explore.h", src / "m0_explore.h");
      copy_template(o.targets_dir / "common/host" / "m0_explore_host.h", src / "m0_explore_host.h");
      for (const char* name : {"pc_frontend.c", "pc_frontend.h"})
        copy_template(o.targets_dir / "consoles/pc" / name, src / name);
      write(o.output / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.20)\nproject(vectrex_pc C)\n"
            "set(CMAKE_C_STANDARD 11)\nif(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
            "find_package(PkgConfig REQUIRED)\n"
            "pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)\n"
            "add_executable(vectrex_pc src/vectrex_host.c src/pc_frontend.c src/vectrex.c src/m6809.c src/m6809_run.c "
            "src/via.c src/psg.c src/vx_cart.c" + std::string(o.interpret ? "" : " src/vx_gen.c") + ")\n"
            "target_link_libraries(vectrex_pc PRIVATE PkgConfig::SDL2 m)\n");
      write(o.output / "README.md",
            "# Vectrex PC: " + o.name +
                "\n\n"
                "Build: `cmake -G Ninja -S . -B build && ninja -C build`\n\n"
                "Run: `build/vectrex_pc --bios '/path/to/Vectrex BIOS (1982).vec'`\n\n"
                "Optional: `--frames 120 --ppm shot.ppm --wav sound.wav` for a "
                "headless capture (it prints the strokes' run_hash). `--full-boot` keeps the BIOS intro.\n"
                "`--interp` runs the run loop alone (the reference for the recompiled code); "
                "`--interp --coverage FILE` records the profile m0recomp's `--coverage` takes.\n"
            "Arrows or gamepad stick: stick; X/Z/A/S or gamepad B/A/X/Y: buttons 1-4; "
            "Escape: quit. No filter by default; F8 cycles filters, F10 scaling, "
            "F11 fullscreen. The BIOS is loaded "
                "at run time and never copied into this project.\n");
      write(marker, target + "\n");
      return true;
    }

    /* the META: the system ROM at E000-FFFF in flash, a 4 KiB Executive in
     * the upper half */
    write(src / "vx_cart.c", cart + "\n/* The system ROM (E000-FFFF). */\nconst uint8_t vectrex_bios[8192] = {\n" +
                                 bytes(bios.data(), bios.size()) + "};\n");
    const std::string sd = state_base(o.name);
    /* A state slot: the META's phosphor fits 24 sectors; the AKA's 240x240
     * phosphor (28.8 KiB) asks for 60. */
    write(src / "vx_config.h",
          std::string("#pragma once\n"
                      "/* the panel's colour order: the META's SPI takes the high byte first; the AKA's i80 swaps the bytes in hardware */\n"
                      "#define VX_PANEL_SWAP ") + (o.target == TargetId::aka ? "0" : "1") + "\n" + native +
              std::string(
                      "#define VX_FULL_BOOT ") +
              (o.full_boot ? "1" : "0") + "\n" +
              "#define VX_STATE_NAME \"" + sd + ".ST8\"\n" +
              "#define VX_STATE_NAME83 \"" + name83(sd, "ST8") + "\"\n" +
              "#define VX_BIN_NAME83 \"" + name83(sd, "BIN") + "\"\n" +
              "#define VX_STATE_SLOTS 10\n#define VX_STATE_SLOT_SECTORS " +
              (o.target == TargetId::aka ? "60" : "24") + "\n");
    copy_template(tpl / "platform" / "vx_main.cpp", src / "vx_main.cpp");

    const fs::path common = o.targets_dir / "common";
    const fs::path shared = common / "platform";
    const fs::path platform = o.targets_dir / "consoles" / "meta";
    /* frame pacing at 50 Hz: 30000 cycles of 1.5 MHz */
    write(src / "sg_frontend.h",
          replace(replace(read(shared / "sg_frontend.h"), "#define SG_FRAME_US 16688u", "#define SG_FRAME_US 20000u"),
                  "#define SG_STALL_US 100128", "#define SG_STALL_US 120000"));
    copy_template(shared / "sg_menu.h", src / "sg_menu.h");
    for (const char *name : {"m0_target.h", "m0_lcd.h", "m0_clock.h", "m0_pad.h", "m0_menu.h", "m0_font.h", "m0_state.h", "m0_state.cpp",
                             "m0_menu.cpp", "m0_sd.c", "m0_sd.h", "m0_fat.c", "m0_fat.h", "m0_card.h"})
      copy_template(shared / name, src / name);
    copy_template(o.targets_dir / "meta" / "runtime" / "gbl_board.h", src / "gbl_board.h");
    /* the audio driver: a frame's samples are 30000 cycles at 1.5 MHz */
    write(src / "m0_audio.h",
          replace(replace(read(shared / "m0_audio.h"), "70224u", "30000u"), "4194304u", "1500000u"));
    if (o.target == TargetId::aka) {
      /* the ESP32 family's shared core and the AKA's pins; the card rides
       * its own SDMMC bus (m0_sd.c over esp32_sd.cpp's hooks), the phosphor
       * rows go to the panel by the AKA's i80 driver */
      const fs::path core = common / "consoles" / "esp32";
      const fs::path aka_platform = o.targets_dir / "consoles" / "aka";
      copy_template(core / "esp32_hw.h", src / "esp32_hw.h");
      copy_template(core / "esp32_sd.cpp", src / "esp32_sd.cpp");
      copy_template(aka_platform / "esp32_config.h", src / "esp32_config.h");
      copy_template(aka_platform / "aka_hw.h", src / "aka_hw.h");
      {
        /* m0_audio_frame renders a 50 Hz frame (220 samples at 11025 Hz) */
        auto hw = read(core / "esp32_hw.cpp");
        const std::string frame = "uint8_t samples[192];";
        if (hw.find(frame) == std::string::npos)
          throw std::runtime_error("esp32_hw.cpp changed: the Vectrex patch no longer applies");
        hw = replace(hw, frame, "uint8_t samples[256]; /* a 50 Hz frame at 11025 Hz */");
        write(src / "esp32_hw.cpp", hw);
      }
      copy_template(aka_platform / "aka_hw.cpp", src / "aka_hw.cpp");
      /* the panel's resample (aka_hw.cpp samples m0_nearest) */
      copy_template(common / "runtime" / "m0_scale.h", src / "m0_scale.h");
      /* the audio hooks the ESP32 core finds by its sg_audio.h */
      write(src / "sg_audio.h", "#pragma once\n#include \"vectrex.h\"\n"
                                "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                                "void vx_audio_render(uint8_t *, unsigned);\n"
                                "void vx_audio_set_rate(unsigned);\n"
                                "#ifdef __cplusplus\n}\n#endif\n"
                                "#define sg_platform_audio_rate vx_audio_set_rate\n"
                                "#define sg_platform_audio_render vx_audio_render\n");
      fs::create_directories(o.output / "boards");
      copy_template(aka_platform / "boards" / "gamebuino_aka.json",
                    o.output / "boards" / "gamebuino_aka.json");
      std::error_code list_error;
      for (const auto &entry : fs::directory_iterator(aka_platform, list_error))
        if (entry.is_regular_file() && entry.path().extension() == ".in")
          write(o.output / entry.path().stem(), replace(read(entry.path()), "@NAME@", o.name));
      project::write_config(o.target, src);
      project::write_profile(o.target, src, {});
    } else {
      copy_template(platform / "meta_sd_bus.cpp", src / "meta_sd_bus.cpp");
      auto audio = read(platform / "meta_audio.cpp");
      audio = replace(audio, "#include \"gbl.h\"", "#include \"vectrex.h\"\nvoid vx_audio_render(uint8_t *, unsigned);\n"
                                                   "void vx_audio_set_rate(unsigned);");
      audio = replace(replace(audio, "gbl_apu_render", "vx_audio_render"), "gbl_apu_set_rate", "vx_audio_set_rate");
      write(src / "meta_audio.cpp", audio);
      write(src / "gbl_config.h", std::string("#pragma once\n#define GBL_TARGET_POKITTO 0\n#define GBL_GEN_VERSION \"") +
                                      GBRECOMP_META_VERSION + "\"\n");
      write(src / "gbl_profile.h",
            "#pragma once\n#include \"gbl_board.h\"\n"
            "#ifndef META_OVERCLOCK\n#define META_OVERCLOCK 1\n#endif\n"
            "#define POKITTO_LCD_ASM 0\n#define POKITTO_RAM_BANKS 0\n#define POKITTO_AUDIO_ASM 0\n"
            "#ifndef META_AUDIO_DMA\n#define META_AUDIO_DMA 0\n#endif\n"
            "#ifndef META_AUDIO_RING\n#define META_AUDIO_RING 512u\n#endif\n");
      for (const char *name : {"meta_startup.c", "meta_clock.cpp", "meta_lcd.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                               "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h",
                               "meta_usb_msc.c", "meta_usb_msc.h"})
        copy_template(platform / name, src / name);
      write(o.output / "platformio.ini",
            replace(replace(read(platform / "platformio.ini.in"), "@NAME@", o.name), "@COMPILER@", o.compiler));
      write(o.output / "gamebuino_meta.ld", replace(read(platform / "gamebuino_meta.ld.in"), "@NAME@", o.name));
      fs::create_directories(o.output / "boards");
      copy_template(platform / "boards" / "gamebuino_meta.json", o.output / "boards" / "gamebuino_meta.json");
      fs::create_directories(o.output / "toolchain");
      for (const auto &entry : fs::directory_iterator(common / "toolchain"))
        if (entry.is_regular_file())
          copy_template(entry.path(), o.output / "toolchain" / entry.path().filename());
      if (!fs::exists(o.output / (sd + ".ST8")))
        write(o.output / (sd + ".ST8"), std::string(10u * 24u * 512u, char(0xff)));
    }
    write(o.output / "README.md",
          o.target == TargetId::aka
              ? "# Vectrex AKA: " + o.name +
                    "\n\n" +
                    std::string(o.interpret ? "The cartridge interpreted (6809, VIA, AY-3-8912), the beam drawn\n"
                                              "1:1 at the panel's 240x240 (320-column rows).\n\n"
                                            : "The 6809 recompiled to C (src/vx_gen.c) in front of the interpreter, the VIA\n"
                                              "and the AY-3-8912 emulated, the beam drawn 1:1 at the panel's 240x240\n"
                                              "(320-column rows).\n\n") +
                    "The system ROM is built into this firmware: do not distribute it.\n\n"
                    "Firmware: pio run  (flash with pio run -t upload)\n\n"
                    "Save states live on the card, at its root, as " + sd + ".ST8.\n"
                    "D-pad or joystick: stick; A, B, RUN, MENU: buttons 1-4; MENU held: options.\n"
              : "# Vectrex META: " + o.name +
                    "\n\n" +
                    std::string(o.interpret ? "The cartridge interpreted (6809, VIA, AY-3-8912) and drawn at 128x128.\n"
                                            : "The 6809 recompiled to C (src/vx_gen.c) in front of the interpreter, the VIA\n"
                                              "and the AY-3-8912 emulated, the beam drawn at 128x128.\n") +
                    "The system ROM is built into this firmware: do not distribute it.\n\n"
                    "Firmware: pio run  (flash with pio run -t upload, or copy\n"
                    ".pio/build/meta/firmware.bin to the SD card)\n\n"
                    "Copy " + sd + ".ST8 beside " + sd + ".BIN on the SD card for ten save-state slots.\n"
                    "D-pad: stick; A, B, MENU, HOME: buttons 1-4; HOME held: options.\n");
    write(marker, target + "\n");
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace m0boy::vectrex
