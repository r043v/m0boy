/* SNES project generation, after the PCE generator's plan: the cartridge
 * image as C data, the runtime with the recompiled 65C816 code (snes_gen.c,
 * or the interpreter alone with --nes-interp) and the desktop harness. */
#include "m0boy/snes_generator.h"
#include "m0boy/snes_emitter.h"
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace m0boy::snes {
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
/* a coverage line's context: K, DBR, the direct page and the M/X/E flags
 * packed K lowest, ten hex digits */
bool is_ctx(const std::string& t) {
    if (t.size() != 10) return false;
    for (char c : t)
        if (!hex_digit(c)) return false;
    return true;
}

} // namespace

/* the harness's coverage files: "addr [ctx...] hits [d]" lines — the
 * contexts are the K/DBR/D/flag bytes the address ran under.  Same-address
 * lines merge (hits summed, contexts kept, dynamic kept). */
std::vector<CoverageEntry> read_coverage(const std::vector<fs::path>& files) {
    std::map<uint64_t, CoverageEntry> sum; /* key: pc24 in the high bits, then the context */
    auto key_of = [&](uint32_t pc, bool ctxed, uint64_t ctx) {
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
                auto& e = sum[key_of(uint32_t(pc), false, 0)];
                e.pc = uint32_t(pc);
                e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
                e.dynamic = e.dynamic || dynamic;
            } else {
                for (uint64_t ctx : ctxs) {
                    auto& e = sum[key_of(uint32_t(pc), true, ctx)];
                    e.pc = uint32_t(pc);
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

/* the header's map nibble: $20/$22/$23 LoROM, $21/$25 HiROM; the checksum
 * complement pairing tells a real header from noise.  Returns 0 LoROM,
 * 1 HiROM, 2 ExHiROM (the header at $40FFC0, a paired checksum and a map
 * nibble of 5). */
static int header_map(const std::vector<uint8_t>& rom, size_t at) {
    if (at + 0x40 > rom.size()) return -1;
    const unsigned comp = rom[at + 0x3C] | rom[at + 0x3D] << 8;
    const unsigned cksum = rom[at + 0x3E] | rom[at + 0x3F] << 8;
    const unsigned map = rom[at + 0x15] & 0x0F;
    if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) return (int)map;
    if (map == 0 || map == 1 || map == 2 || map == 3 || map == 5) return (int)map;
    return -1;
}
static int detect_map(const std::vector<uint8_t>& rom) {
    if (rom.size() > 0x40FFC0u + 0x40) {
        const int ex = header_map(rom, 0x40FFC0);
        if (ex >= 0 && (ex & 0x0F) == 5) return 2;
    }
    const int hi = header_map(rom, 0xFFC0);
    const int lo = header_map(rom, 0x7FC0);
    if (hi == 1 || hi == 5) return 1;
    if (lo == 0 || lo == 2 || lo == 3) return 0;
    return hi >= 0 && lo < 0 ? 1 : 0;
}

bool generate(const fs::path& cartridge, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    stats = {};
    try {
        if (o.target != TargetId::desktop && o.target != TargetId::pc)
            throw std::runtime_error("SNES projects target the desktop harness or the PC window (--target desktop|pc)");
        const auto tpl = o.meta_templates.parent_path() / "snes";
        if (!fs::is_regular_file(tpl / "runtime" / "snes.c"))
            throw std::runtime_error("SNES templates missing next to targets/meta");

        /* the cartridge: the 512-byte header some collections carry, then
         * the image as the bus sees it */
        std::ifstream file(cartridge, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read " + cartridge.string());
        file.seekg(0, std::ios::end);
        const size_t file_bytes = size_t(file.tellg());
        if (file_bytes < 0x8000) throw std::runtime_error("SNES image too small: " + cartridge.string());
        const size_t skip = file_bytes % 0x8000 == 512 ? 512 : 0;
        const size_t image_bytes = file_bytes - skip;
        std::vector<uint8_t> rom(image_bytes);
        file.seekg(std::streamoff(skip));
        file.read(reinterpret_cast<char*>(rom.data()), std::streamsize(image_bytes));
        if (size_t(file.gcount()) != image_bytes)
            throw std::runtime_error("Truncated image: " + cartridge.string());
        const int map = detect_map(rom);
        /* the header's RAM byte: $01-$03 are 2/8/32 KiB, the runtime's
         * window mirrors 32 KiB at most */
        static const uint32_t ram_table[4] = {0, 2048, 8192, 32768};
        const size_t head_at = map == 0 ? 0x7FC0 : (map == 1 ? 0xFFC0 : 0x40FFC0);
        const uint8_t ram_byte = head_at + 0x18 < image_bytes ? rom[head_at + 0x18] : 0;
        const uint32_t sram_bytes = ram_byte < 4 ? ram_table[ram_byte] : 0;
        /* the header's DSP-1 mark: the map byte's high nibble $3 or a low
         * nibble of 3 (LoROM + SRAM + DSP-1, $23) */
        const unsigned map_byte = head_at + 0x16 <= image_bytes ? rom[head_at + 0x15] : 0;
        const int dsp1 = ((map_byte & 0xF0) == 0x30 || (map_byte & 0x0F) == 3) ? 1 : 0;
        /* the header's SuperFX mark: type $15, or the map byte $13/$14 */
        const unsigned type_byte = head_at + 0x17 <= image_bytes ? rom[head_at + 0x16] : 0;
        const int fx = type_byte == 0x15 || map_byte == 0x13 || map_byte == 0x14 ? 1 : 0;

        const auto marker = o.output / "snes-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false;
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a SNES project; choose a fresh directory");
        }
        const std::string target_name = o.target == TargetId::pc ? "pc" : "desktop";
        if (fs::exists(marker)) {
            if (read(marker) != target_name + "\n")
                throw std::runtime_error("Use a separate output directory for each destination");
        }
        const auto src = o.output / "src";
        fs::create_directories(src);
        fs::create_directories(o.output / "host");

        write(src / "snes_cart.c",
              "/* The cartridge, generated by gbrecomp. */\n"
              "#include <stdint.h>\n\n"
              "const uint8_t snes_rom[] = {\n" + rom_data(rom.data(), image_bytes) + "};\n");
        std::string config = "#pragma once\n"
              "#define SNES_ROM_SIZE " + std::to_string(image_bytes) + "u\n"
              "#define SNES_MAP " + std::to_string(map) + "\n"
              "#define SNES_SRAM_BYTES " + std::to_string(sram_bytes) + "u\n"
              "#define SNES_DSP1 " + std::to_string(dsp1) + "\n"
              "#define SNES_FX " + std::to_string(fx) + "\n";
        /* the recompiled code's dispatcher (snes_cpu.c) */
        if (!o.interpret) config += "#ifndef SNES_NATIVE\n#define SNES_NATIVE 1\n#endif\n";
        write(src / "snes_config.h", config);

        for (const char* name :
             { "snes_cpu.c", "snes.c", "snes_ppu.c", "snes_smp.c", "snes_dsp.c", "snes_dsp1.c",
               "snes_fx.c", "snes.h", "snes_gen.h" })
            copy_template(tpl / "runtime" / name, src / name);
        /* the idle loops' count (snes_gen.h, the dispatcher) */
        copy_template(o.meta_templates.parent_path() / "common" / "runtime" / "m0_idle.h", src / "m0_idle.h");

        /* the recompiled 65C816 code: the reset vector, the profile, what
         * they reach under the contexts the walk can prove */
        const bool native = !o.interpret;
        if (native) {
            EmitOptions emit;
            emit.rom_bytes = uint32_t(image_bytes);
            emit.hirom = map == 1;
            emit.exhirom = map == 2;
            emit.precise = o.precise;
            emit.superfx = fx != 0;
            emit.dsp1 = dsp1 != 0;
            const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : 24u;
            emit.max_instructions = o.code_budget_bytes ? o.code_budget_bytes / bpi : 0;
            EmitStats es;
            write(src / "snes_gen.c", emit_code(rom, read_coverage(o.coverage), emit, es));
            stats.compiled = es.compiled;
            stats.covered = es.covered;
            stats.regions = es.regions;
            stats.entries = es.entries;
        }

        /* the harness sits next to src/ in a generated project, not next to
         * ../runtime as in the checkout */
        write(o.output / "host" / "snes_host.c",
              replace(replace(read(tpl / "host" / "snes_host.c"),
                              "#include \"../runtime/snes.h\"", "#include \"../src/snes.h\""),
                      "#ifdef SNES_EMBEDDED", "#include \"../src/snes_config.h\"\n#ifdef SNES_EMBEDDED"));
        copy_template(tpl / "host" / "snes_hash.h", o.output / "host" / "snes_hash.h");
        copy_template(o.meta_templates.parent_path() / "consoles" / "pc" / "pc_input.h", o.output / "host" / "pc_input.h");
        const std::string sources =
            "${SRC}/snes.c ${SRC}/snes_cpu.c ${SRC}/snes_ppu.c ${SRC}/snes_smp.c ${SRC}/snes_dsp.c"
            " ${SRC}/snes_dsp1.c ${SRC}/snes_fx.c ${SRC}/snes_cart.c" + std::string(native ? " ${SRC}/snes_gen.c" : "");
        write(o.output / "host" / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_snes_host C)\nset(CMAKE_C_STANDARD 11)\n"
              "if(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
              "set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n"
              "add_executable(" + o.name + "_host snes_host.c " + sources + ")\n"
              "target_include_directories(" + o.name + "_host PRIVATE ${SRC})\n"
              "target_compile_definitions(" + o.name + "_host PRIVATE SNES_EMBEDDED SNES_TRACE)\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        if (o.target == TargetId::pc) {
            /* the SDL2 window: the shell next to src/, the ROM embedded,
             * the window at the source's native 256x224 */
            copy_template(tpl / "platform" / "snes_pc.c", src / "snes_pc.c");
            for (const char* name : { "pc_frontend.c", "pc_frontend.h", "pc_input.h" })
                copy_template(o.meta_templates.parent_path() / "consoles" / "pc" / name, src / name);
            copy_template(tpl / "host" / "snes_hash.h", src / "snes_hash.h");
            write(o.output / "CMakeLists.txt",
                  "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_snes_pc C)\n"
                  "set(CMAKE_C_STANDARD 11)\n"
                  "find_package(PkgConfig REQUIRED)\n"
                  "pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)\n"
                  "add_executable(" + o.name + "_pc src/snes_pc.c src/pc_frontend.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/snes.c ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_cpu.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/snes_ppu.c ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_smp.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/snes_dsp.c ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_dsp1.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/snes_fx.c "
                  "${CMAKE_CURRENT_SOURCE_DIR}/src/snes_cart.c" +
                  std::string(native ? " ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_gen.c" : "") + ")\n"
                  "target_include_directories(" + o.name + "_pc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)\n"
                  "target_compile_definitions(" + o.name + "_pc PRIVATE SNES_EMBEDDED)\n"
                  "target_compile_options(" + o.name + "_pc PRIVATE -O2 -w)\n"
                  "target_link_libraries(" + o.name + "_pc PRIVATE PkgConfig::SDL2 m)\n");
            write(marker, "pc\n");
            write(o.output / "README.md",
                  "# SNES project: " + o.name + "\n\n" +
                  (native ? std::string("The cartridge's 65C816 code recompiled to C (src/snes_gen.c) on the\n"
                                        "5A22/PPU/APU runtime, the interpreter for the rest.\n\n")
                          : std::string("The cartridge interpreted on the 5A22/PPU/APU runtime; "
                                        "SDL2 supplies the window, the keyboard and the sound.\n\n")) +
                  "Build: cmake -G Ninja -S . -B build && ninja -C build\n"
                  "Run: build/" + o.name + "_pc (arrows, Z = B, X = A, A = Y, S = X, Q/E = L/R, "
                  "Enter = Start, Shift = Select, Esc quits; the gamepad maps A/B/X/Y and the "
                  "shoulders the same). No filter by default; F8 cycles filters, F10 scaling, "
                  "F11 fullscreen. --frames N and --ppm shot.ppm provide a headless run; "
                  "--wav f writes the S-DSP's 32000 Hz stereo.\n");
            return true;
        }

        write(marker, "desktop\n");
        write(o.output / "README.md",
              "# SNES project: " + o.name + "\n\n" +
              (native ? std::string("The cartridge's 65C816 code recompiled to C (src/snes_gen.c) on the\n"
                                    "5A22/PPU/APU runtime, the interpreter for the rest.  A profile: host/build/" + o.name +
                                    "_host --interp --coverage cov.txt\n"
                                    "(then gbrecomp --system snes ... --coverage cov.txt).\n\n")
                      : std::string("The cartridge interpreted on the 5A22/PPU/APU runtime.\n\n")) +
              "Build: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"
              "Run: host/build/" + o.name + "_host --frames N [--interp] [--coverage f] [--input script.txt] "
              "[--capture screen.ppm] [--wav f]\n");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::snes
