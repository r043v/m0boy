/* PC Engine project generation, after the NES generator's plan: the
 * HuCard image as C data (the 128-byte header some collections carry is
 * dropped, the rest padded to 8 KiB pages), the runtime with the
 * recompiled HuC6280 code (pce_gen.c, or the interpreter alone with
 * --nes-interp) and the desktop harness or the PC window. */
#include "m0boy/pce_generator.h"
#include "m0boy/pce_emitter.h"
#include "m0boy/console_project.h"
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace m0boy::pce {
namespace fs = std::filesystem;
namespace {

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
/* the ROM as C data, 16 bytes a line */
std::string rom_data(const uint8_t* data, size_t n) {
    std::ostringstream out;
    for (size_t i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof b, "0x%02X,", data[i]);
        out << b;
        if ((i & 15) == 15 || i + 1 == n) out << "\n";
    }
    return out.str();
}

bool hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
/* a coverage line's context: the MPR bytes packed MPR0 first, 16 hex digits */
bool is_ctx(const std::string& t) {
    if (t.size() != 16) return false;
    for (char c : t)
        if (!hex_digit(c)) return false;
    return true;
}

} // namespace

/* the harness's coverage files: "addr [ctx...] hits [d]" lines — the
 * contexts are the MPR bytes the address ran under.  Same-address lines
 * merge (hits summed, contexts kept, dynamic kept). */
std::vector<CoverageEntry> read_coverage(const std::vector<fs::path>& files) {
    std::map<uint64_t, CoverageEntry> sum; /* key: pc in the high bits, then the context */
    auto key_of = [&](uint16_t pc, bool ctxed, uint64_t ctx) {
        return ctxed ? (uint64_t(pc) << 24) ^ ctx : (uint64_t(pc) << 24) ^ (1ULL << 63);
    };
    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot read coverage file: " + path.string());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string addr;
            unsigned long pc;
            if (!(fields >> addr)) throw std::runtime_error("Bad coverage line in " + path.string() + ": " + line);
            pc = std::stoul(addr, nullptr, 16);
            std::vector<uint64_t> ctxs;
            uint64_t hits = 0;
            bool dynamic = false;
            std::string tok;
            while (fields >> tok) {
                if (tok == "d") dynamic = true;
                else if (is_ctx(tok)) ctxs.push_back(std::stoull(tok, nullptr, 16));
                else hits += std::stoull(tok, nullptr, 10);
            }
            if (ctxs.empty()) { /* an older file's address without a context */
                auto& e = sum[key_of(uint16_t(pc), false, 0)];
                e.pc = uint16_t(pc);
                e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
                e.dynamic = e.dynamic || dynamic;
            } else {
                for (uint64_t ctx : ctxs) {
                    auto& e = sum[key_of(uint16_t(pc), true, ctx)];
                    e.pc = uint16_t(pc);
                    e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
                    e.dynamic = e.dynamic || dynamic;
                    e.contexts.push_back(ctx);
                }
            }
        }
    }
    std::vector<CoverageEntry> out;
    for (const auto& [key, e] : sum) out.push_back(e);
    return out;
}

bool generate(const fs::path& cartridge, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    stats = {};
    try {
        if (o.target != TargetId::desktop && o.target != TargetId::pc && o.target != TargetId::aka)
            throw std::runtime_error("PC Engine projects target the desktop harness, the PC window or the Gamebuino AKA (--target desktop|pc|aka)");
        const auto tpl = o.meta_templates.parent_path() / "pce";
        if (!fs::is_regular_file(tpl / "runtime" / "pce.c"))
            throw std::runtime_error("PC Engine templates missing next to targets/meta");

        /* the HuCard: the 128-byte header some collections carry, then
         * the image, padded to the MPR's 8 KiB pages */
        std::ifstream file(cartridge, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read " + cartridge.string());
        file.seekg(0, std::ios::end);
        const size_t file_bytes = size_t(file.tellg());
        const size_t header = file_bytes % 0x20000 == 128 ? 128 : 0;
        const size_t image_bytes = file_bytes - header;
        if (image_bytes < 0x2000) throw std::runtime_error("PC Engine image too small: " + cartridge.string());
        const size_t rom_bytes = (image_bytes + 0x1FFFu) & ~size_t(0x1FFFu);
        std::vector<uint8_t> rom(rom_bytes, 0xFF);
        file.seekg(std::streamoff(header));
        file.read(reinterpret_cast<char*>(rom.data()), std::streamsize(image_bytes));
        if (size_t(file.gcount()) != image_bytes)
            throw std::runtime_error("Truncated image: " + cartridge.string());

        const auto marker = o.output / "pce-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false; /* compile.sh writes its log into the project directory before generation */
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a PC Engine project; choose a fresh directory");
        }
        const std::string target_name = o.target == TargetId::pc ? "pc"
                                      : o.target == TargetId::aka ? "aka" : "desktop";
        if (fs::exists(marker)) {
            if (read(marker) != target_name + "\n")
                throw std::runtime_error("Use a separate output directory for each destination");
        }
        const bool pc_window = o.target == TargetId::pc;
        const bool console = o.target == TargetId::aka;
        const auto src = o.output / "src";
        fs::create_directories(src);
        fs::create_directories(o.output / "host");

        write(src / "pce_cart.c",
              "/* The HuCard, generated by gbrecomp (padded to the MPR's pages). */\n"
              "#include <stdint.h>\n\n"
              "const uint8_t pce_rom[] = {\n" + rom_data(rom.data(), rom_bytes) + "};\n");
        std::string config = "#pragma once\n"
              "#define PCE_ROM_SIZE " + std::to_string(rom_bytes) + "u\n";
        /* the recompiled code's dispatcher (huc6280.c) */
        if (!o.interpret) config += "#ifndef PCE_NATIVE\n#define PCE_NATIVE 1\n#endif\n";
        if (console) {
            /* the HuCard rides in the app partition (0x2EE000) with the
             * firmware: above ~2.5 MiB the image no longer fits whole */
            if (rom_bytes > 0x280000u)
                throw std::runtime_error("HuCard too large for the AKA's app partition whole (" +
                                         std::to_string(rom_bytes) + " bytes; the streaming plan follows)");
            /* the panel doubles the 120 planned rows (aka_hw.cpp's zoom) */
            config += "#define M0_SCREEN_ROWS 120\n";
            /* the second core takes the VCE + box filter + LCD submits
             * (pce_main.cpp's render task, the Mega Drive's MD_RENDER_THREAD) */
            config += "#ifndef PCE_RENDER_THREAD\n#define PCE_RENDER_THREAD 1\n#endif\n";
        }
        write(src / "pce_config.h", config);

        for (const char* name :
             { "huc6280.c", "huc6280.h", "vdc.c", "vdc.h", "psg.c", "psg.h", "pce.c", "pce.h", "pce_gen.h" })
            copy_template(tpl / "runtime" / name, src / name);
        /* the idle loops' count (pce_gen.h, the dispatcher) */
        copy_template(o.meta_templates.parent_path() / "common" / "runtime" / "m0_idle.h", src / "m0_idle.h");
        /* the input scripts' reader (the harness's --input, the window's
         * --input / --record-input) */
        copy_template(o.meta_templates.parent_path() / "consoles" / "pc" / "pc_input.h", src / "pc_input.h");
        /* the harness's --explore player */
        copy_template(o.meta_templates.parent_path() / "common" / "host" / "m0_explore.h", src / "m0_explore.h");
        copy_template(o.meta_templates.parent_path() / "common" / "host" / "m0_explore_host.h", src / "m0_explore_host.h");

        /* the recompiled HuC6280 code: the vectors, the profile, what they
         * reach under the MPR contexts the walk can prove */
        const bool native = !o.interpret;
        std::string gen_parts_pc;
        std::vector<std::string> gen_parts; /* pce_gen_N.c: the regions of a split build */
        if (native) {
            EmitOptions emit;
            emit.rom_bytes = uint32_t(rom_bytes);
            emit.precise = o.precise;
            emit.sweep = o.sweep;
            const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : 24u;
            emit.max_instructions = o.code_budget_bytes ? o.code_budget_bytes / bpi : 0;
            /* The AKA's app slot (3000K, the official partitions.csv) holds the ROM, a runtime of ~0x30000 and the code: ~133 bytes
             * per instruction measured on Xtensa (Raiden: 63670 instructions = 9.6 MB), 147 here for margin (the flag fixes grew it: 3.16 MB for 15473 instructions, about 105 bytes more per instruction at the margin); the runtime measures ~0x1B000. */
            if (!emit.max_instructions && o.target == TargetId::aka) {
                const size_t app = 0x2EE000, runtime = 0x30000;
                emit.max_instructions = app > rom_bytes + runtime ? (app - rom_bytes - runtime) / 147 : 1;
            }
            /* thousands of regions: one C file of tens of MB needs more RAM than the compiler has */
            emit.regions_per_file = 4000;
            EmitStats es;
            write(src / "pce_gen.c", emit_code(rom, read_coverage(o.coverage), emit, es));
            for (size_t k = 0; k < es.parts.size(); k++) {
                write(src / ("pce_gen_" + std::to_string(k + 1) + ".c"), es.parts[k]);
                gen_parts.push_back("pce_gen_" + std::to_string(k + 1) + ".c");
                gen_parts_pc += " ${CMAKE_CURRENT_SOURCE_DIR}/src/" + gen_parts.back();
            }
            stats.compiled = es.compiled;
            stats.covered = es.covered;
            stats.regions = es.regions;
            stats.entries = es.entries;
        }

        if (console) {
            /* The Gamebuino AKA (ESP32-S3): the shared frontend, the ESP32
             * core and the AKA's pins; PlatformIO builds and flashes the
             * project (every *.in rendered to the root).  The HuCard rides
             * in flash whole. */
            const auto t = project::Templates::from_meta(o.meta_templates);
            copy_template(tpl / "platform" / "pce_main.cpp", src / "pce_main.cpp");
            project::copy_frontend(t, src);
            /* the scaling helpers the core's adapter samples with
             * (m0_scale.h: m0_nearest, m0_row_of) */
            copy_template(o.meta_templates.parent_path() / "common" / "runtime" / "m0_scale.h",
                          src / "m0_scale.h");
            /* the card driver (m0_sd.c over esp32_sd.cpp's hooks) and the
             * save-state machinery's headers stay with the family */
            for (const char* name : { "m0_sd.c", "m0_sd.h", "m0_fat.c", "m0_fat.h", "m0_card.h" })
                copy_template(t.frontend() / name, src / name);
            const auto core = o.meta_templates.parent_path() / "common" / "consoles" / "esp32";
            for (const char* name : { "esp32_hw.cpp", "esp32_hw.h", "esp32_sd.cpp" })
                copy_template(core / name, src / name);
            const auto platform = t.console(TargetId::aka);
            copy_template(platform / "esp32_config.h", src / "esp32_config.h");
            copy_template(platform / "aka_hw.cpp", src / "aka_hw.cpp");
            copy_template(platform / "aka_hw.h", src / "aka_hw.h");
            fs::create_directories(o.output / "boards");
            copy_template(platform / "boards" / "gamebuino_aka.json",
                          o.output / "boards" / "gamebuino_aka.json");
            std::error_code list_error;
            for (const auto& entry : fs::directory_iterator(platform, list_error)) {
                if (entry.is_directory() || entry.path().extension() != ".in") continue;
                write(o.output / entry.path().stem(), replace(read(entry.path()), "@NAME@", o.name));
            }
            /* the audio hooks the ESP32 core finds by its sg_audio.h:
             * pce_audio_render/pce_audio_set_rate at the fixed 11025 Hz */
            write(src / "sg_audio.h", "#pragma once\n#include \"pce.h\"\n"
                                      "#define sg_platform_audio_rate pce_audio_set_rate\n"
                                      "#define sg_platform_audio_render pce_audio_render\n");
            /* m0_audio.h's constants: 119210 CPU cycles per frame (CSH) at
             * 7159090 Hz master */
            project::write_audio(t, o.target, src, "119210u", "7159090u", "pce.h",
                                 "pce_audio_render", "pce_audio_set_rate");
            project::write_config(o.target, src);
            project::write_profile(o.target, src, {false, false, false});
            write(marker, target_name + "\n");
            write(o.output / "README.md",
                  "# PC Engine project: " + o.name + " (Gamebuino AKA)\n\n" +
                  (native ? std::string("The HuCard's HuC6280 code recompiled to C (src/pce_gen.c) on the HuC6280/VDC\n"
                                        "runtime, the interpreter for the rest.\n\n")
                          : std::string("The HuCard interpreted on the HuC6280/VDC runtime.\n\n")) +
                  "Firmware: pio run  (flash with pio run -t upload)\n");
            return true;
        }

        /* the harness sits next to src/ in a generated project, not next to
         * ../runtime as in the checkout */
        write(o.output / "host" / "pce_host.c",
              replace(replace(read(tpl / "host" / "pce_host.c"),
                              "#include \"../runtime/pce.h\"", "#include \"../src/pce.h\""),
                      "#include \"../../consoles/pc/pc_input.h\"", "#include \"../src/pc_input.h\""));
        std::string sources = "${SRC}/pce.c ${SRC}/huc6280.c ${SRC}/vdc.c ${SRC}/psg.c ${SRC}/pce_cart.c" +
                                    std::string(native ? " ${SRC}/pce_gen.c" : "");
        for (const auto& part : gen_parts) sources += " ${SRC}/" + part;
        write(o.output / "host" / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_pce_host C)\nset(CMAKE_C_STANDARD 11)\n"
              "if(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
              "set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n"
              "add_executable(" + o.name + "_host pce_host.c " + sources + ")\n"
              "target_include_directories(" + o.name + "_host PRIVATE ${SRC})\n"
              "target_compile_definitions(" + o.name + "_host PRIVATE PCE_EMBEDDED PCE_TRACE)\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        if (pc_window) {
            /* the SDL2 window: the shell next to src/, the ROM embedded */
            copy_template(tpl / "platform" / "pce_pc.c", src / "pce_pc.c");
            for (const char* name : { "pc_frontend.c", "pc_frontend.h" })
                copy_template(o.meta_templates.parent_path() / "consoles" / "pc" / name, src / name);
            write(o.output / "CMakeLists.txt",
                  "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_pce_pc C)\n"
                  "set(CMAKE_C_STANDARD 11)\n"
                  "find_package(PkgConfig REQUIRED)\n"
                  "pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)\n"
                  "add_executable(" + o.name + "_pc src/pce_pc.c src/pc_frontend.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/pce.c ${CMAKE_CURRENT_SOURCE_DIR}/src/huc6280.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/vdc.c ${CMAKE_CURRENT_SOURCE_DIR}/src/psg.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/pce_cart.c" +
                  std::string(native ? " ${CMAKE_CURRENT_SOURCE_DIR}/src/pce_gen.c" : "") + gen_parts_pc + ")\n"
                  "target_include_directories(" + o.name + "_pc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)\n"
                  "target_compile_definitions(" + o.name + "_pc PRIVATE PCE_EMBEDDED)\n"
                  "target_compile_options(" + o.name + "_pc PRIVATE -O2 -w)\n"
                  "target_link_libraries(" + o.name + "_pc PRIVATE PkgConfig::SDL2 m)\n");
            write(marker, "pc\n");
            write(o.output / "README.md",
                  "# PC Engine project: " + o.name + "\n\n" +
                  (native ? std::string("The HuCard's HuC6280 code recompiled to C (src/pce_gen.c) on the HuC6280/VDC\n"
                                        "runtime, the interpreter for the rest.\n\n")
                          : std::string("The HuCard interpreted on the HuC6280/VDC runtime; "
                                        "SDL2 supplies the window, the keyboard and the sound.\n\n")) +
                  "Build: cmake -G Ninja -S . -B build && ninja -C build\n"
                  "Run: build/" + o.name + "_pc (arrows, X = I, Z = II, Enter = RUN, "
                  "Shift = SELECT, Esc quits; gamepad B/A = I/II, Start/Back = RUN/SELECT). "
                  "No filter by default; F8 cycles filters, F10 scaling, F11 fullscreen. "
                  "--frames N and --ppm shot.ppm provide a headless run. "
                  "--record-input session.input records what the game receives "
                  "(--input script.txt plays a script); the host harness replays it: "
                  "--input session.input.\n");
            return true;
        }

        write(marker, "desktop\n");
        write(o.output / "README.md",
              "# PC Engine project: " + o.name + "\n\n" +
              (native ? std::string("The HuCard's HuC6280 code recompiled to C (src/pce_gen.c) on the HuC6280/VDC\n"
                                    "runtime, the interpreter for the rest.  A profile: host/build/" + o.name +
                                    "_host --interp --coverage cov.txt\n"
                                    "(then gbrecomp --system pce ... --coverage cov.txt).\n\n")
                      : std::string("The HuCard interpreted on the HuC6280/VDC runtime.\n\n")) +
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"
              "Run: host/build/" + o.name + "_host --frames 300 [--ppm shot.ppm] [--input script.txt] [--interp] [--coverage f]\n");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::pce
