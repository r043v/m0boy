/**
 * @file main.cpp
 * @brief GameBoy Recompiler CLI entry point
 */

#include "m0boy/system.h"
#include "m0boy/m0plus_generator.h"
#include "m0boy/sg1000.h"
#include "m0_msx_layout.h"
#include "m0boy/sg1000_generator.h"
#include "m0boy/pico8_generator.h"
#include "m0boy/nes_generator.h"
#include "m0boy/pce_generator.h"
#include "m0boy/snes_generator.h"
#include "m0boy/megadrive_generator.h"
#include "m0boy/vectrex_generator.h"
#include "m0boy/cpu/huc6280/decoder.h"
#include "m0boy/cpu/m0plus/decoder.h"
#include "m0boy/cpu/m6502/decoder.h"
#include "m0boy/cpu/z80/decoder.h"
#include "recompiler/rom.h"
#include "recompiler/decoder.h"
#include "recompiler/analyzer.h"
#include "recompiler/symbol_table.h"
#include "recompiler/ir/ir.h"
#include "recompiler/ir/ir_builder.h"
#include "recompiler/codegen/c_emitter.h"
#include "recompiler/codegen/meta_emitter.h"
#include "recompiler/native_patch.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

#ifndef GBRECOMP_VERSION
#define GBRECOMP_VERSION "0.0.0"
#endif
#ifndef GBRECOMP_META_VERSION
#define GBRECOMP_META_VERSION "0.0.0"
#endif
#ifndef GBRECOMP_ESPBOY_VERSION
#define GBRECOMP_ESPBOY_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_POKITTO_VERSION
#define GBRECOMP_POKITTO_VERSION "0.0.0"
#endif
#ifndef GBRECOMP_PC_VERSION
#define GBRECOMP_PC_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_ODROID_VERSION
#define GBRECOMP_ODROID_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_AKA_VERSION
#define GBRECOMP_AKA_VERSION "0.1.0"
#endif

static void print_version_json() {
    std::cout
        << "{\"schema\":\"gbrecomp.version\",\"schema_version\":1,"
        << "\"version\":\"" << GBRECOMP_VERSION << "\","
        << "\"abis\":{"
        << "\"data_mod\":1,"
        << "\"gameplay_mutation\":1,"
        << "\"native_patch\":1,"
        << "\"port_extension\":1,"
        << "\"port_module\":3,"
        << "\"presentation\":1,"
        << "\"semantic\":1"
        << "},"
        << "\"features\":["
        << "\"annotations\","
        << "\"bounded-gameplay-mutations\","
        << "\"exact-rom-native-patches\","
        << "\"external-runtime-snapshot\","
        << "\"machine-readable-generation-progress\","
        << "\"source-built-port-extensions\""
        << "]}\n";
}

class ProgressReporter {
public:
    bool open(const fs::path& path) {
        stream_.open(path, std::ios::out | std::ios::trunc);
        return stream_.is_open();
    }

    bool enabled() const {
        return stream_.is_open();
    }

    void stage(const char* stage_name, unsigned completed) {
        emit("stage", stage_name, completed, nullptr);
    }

    void failure(const char* stage_name, const char* code) {
        emit("failure", stage_name, completed_, code);
    }

    void complete() {
        emit("complete", "complete", kTotalStages, nullptr);
    }

private:
    static constexpr unsigned kTotalStages = 6;
    std::ofstream stream_;
    unsigned completed_ = 0;

    void emit(const char* event,
              const char* stage_name,
              unsigned completed,
              const char* code) {
        if (!stream_.is_open()) {
            return;
        }
        completed_ = completed;
        stream_
            << "{\"schema\":\"gbrecomp.progress\",\"schema_version\":1,"
            << "\"event\":\"" << event << "\","
            << "\"stage\":\"" << stage_name << "\","
            << "\"completed\":" << completed << ","
            << "\"total\":" << kTotalStages;
        if (code != nullptr) {
            stream_ << ",\"code\":\"" << code << "\"";
        }
        stream_ << "}\n";
        stream_.flush();
    }
};

void print_banner() {
    std::cout << R"(
   ____                      ____              ____                                _ _          _ 
  / ___| __ _ _ __ ___   ___| __ )  ___  _   _|  _ \ ___  ___ ___  _ __ ___  _ __ (_) | ___  __| |
 | |  _ / _` | '_ ` _ \ / _ \  _ \ / _ \| | | | |_) / _ \/ __/ _ \| '_ ` _ \| '_ \| | |/ _ \/ _` |
 | |_| | (_| | | | | | |  __/ |_) | (_) | |_| |  _ <  __/ (_| (_) | | | | | | |_) | | |  __/ (_| |
  \____|\__,_|_| |_| |_|\___|____/ \___/ \__, |_| \_\___|\___\___/|_| |_| |_| .__/|_|_|\___|\__,_|
                                         |___/                              |_|                   
)" << '\n';
    std::cout << "  GameBoy Static Recompiler v" << GBRECOMP_VERSION << "\n";
    std::cout << "  https://github.com/arcanite24/gb-recompiled\n\n";
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <rom.gb|rom.sg|rom_directory> [options]\n\n";
    std::cout << "Directory mode recompiles every .gb/.gbc/.sgb file under the folder,\n";
    std::cout << "emits one module per ROM, and generates a launcher to choose the game.\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o, --output <dir>    Output directory (default: <rom>_output)\n";
    std::cout << "  --runtime-dir <dir>   Runtime snapshot to embed (normally auto-detected)\n";
    std::cout << "  --native-patch <json> Apply an exact-ROM native replacement manifest\n";
    std::cout << "  --output-prefix <id>  Stable generated target/file prefix (single-ROM only)\n";
    std::cout << "  --sd-name <NAME>      META/Pokitto: 8.3 base of the game's files on the SD card\n"
                 "                        (NAME.BIN, NAME.GB/.GG/.SMS, NAME.SAV, NAME.STA)\n";
    std::cout << "  --progress-json <file> Write privacy-safe JSON Lines generation progress\n";
    std::cout << "  -d, --disasm          Disassemble only (don't generate code)\n";
    std::cout << "  -a, --analyze         Analyze control flow only\n";
    std::cout << "  -v, --verbose         Verbose output\n";
    std::cout << "  --trace               Trace execution analysis (very verbose)\n";
    std::cout << "  --limit <n>           Limit analysis to n instructions\n";
    std::cout << "  -j, --jobs <n>        Parallel workers for codegen and batch generation (0=auto)\n";
    std::cout << "  --system auto|gameboy|sg1000|gg|sms|pico8|msx|nes|pce|vectrex|megadrive|snes|meta|pokitto  Source console (default auto; raw firmware needs explicit system)\n";
    std::cout << "  --cpu sm83|z80|6502|huc6280|6809|m68k|m0plus  Check the CPU required by the source console\n";
    std::cout << "  --list-systems       List source consoles, CPUs and available pipelines\n";
    std::cout << "  --list-targets       List destination consoles and their hardware facts\n";
    std::cout << "  --info               Inspect a cartridge without generating code\n";
    std::cout << "  --target meta         Emit a compact gbl project for the Gamebuino META\n";
    std::cout << "                        (coverage-driven; see targets/meta/README.md)\n";
    std::cout << "  --target espboy       ESP8266, 128x128 LCD, no SD, 4 MiB flash (1 MiB firmware, ROM above 256 KiB beside it)\n";
    std::cout << "  --target pokitto      Same gbl project for the Pokitto (bare-metal Makefile build;\n";
    std::cout << "                        every --meta-* option applies)\n";
    std::cout << "  --target pc           PC: the same firmware as a SDL2 window (SD card as a file image)\n";
    std::cout << "  --target odroid       ODROID-GO (ESP32, ILI9341 320x240, SD, 4 MiB PSRAM)\n";
    std::cout << "  --target aka          Gamebuino AKA (ESP32-S3, 320x240, SD, 8 MiB PSRAM)\n";
    std::cout << "  --coverage <file>     gbl coverage profile for --target meta (repeatable)\n";
    std::cout << "  --meta-compiler <gcc|clang>  Firmware compiler (default gcc)\n";
    std::cout << "  --meta-thumb          Experimental register-IR / Thumb-1 code generation\n";
    std::cout << "  --meta-precise        Poll interrupts before every instruction (differential tests)\n";
    std::cout << "  --meta-sync-cycles <n>  SG-1000/GG/SMS: also poll once n T-states ran since the last poll (default 40, 0: loop heads only)\n";
    std::cout << "  --nes-interp          NES, Vectrex: no recompiled code, the interpreter only (alias --interp)\n";
    std::cout << "  --pce-sweep           PC Engine: the card swept for JSR/JMP targets the reachability walk cannot prove (pairs with --meta-code-budget)\n";
    std::cout << "  --nes-sweep           NES: the PRG swept the same way (pairs with --meta-code-budget)\n";
    std::cout << "  --vectrex-sweep       Vectrex: the cartridge swept for JSR/JMP extended targets\n";
    std::cout << "  --megadrive-sweep     Mega Drive: the ROM swept for JSR/JMP absolute long targets\n";
    std::cout << "  --meta-sweep          Game Boy: every bank's CALL/JP abs targets offered as speculative entries\n";
    std::cout << "  --bios <file>         Vectrex: the system ROM (4 or 8 KiB) a META firmware embeds\n";
    std::cout << "  --full-boot           Vectrex: keep the BIOS intro\n";
    std::cout << "  --meta-static         Add the code found by static analysis (no play session needed)\n";
    std::cout << "  --meta-bytes-per-instruction <n>  Flash estimate per compiled instruction (default 20)\n";
    std::cout << "  --meta-fill-bytes <n>  Game Gear/SMS: spend n more bytes of flash on ROM sectors (code unchanged)\n";
    std::cout << "  --rom-plan <file>      Game Gear/SMS: SD reads saved per ROM block, from the simulated card cache\n";
    std::cout << "  --meta-code-budget <bytes>        Flash for generated code (default: 240 KiB - runtime - ROM)\n";
    std::cout << "  --meta-stream auto|on|off         Read the ROM from the SD card (auto: ROMs of 256 KiB or more)\n";
    std::cout << "  --rom-profile <file>              ROM data reads recorded by the host (--rom-profile-out)\n";
    std::cout << "  --meta-rom-resident-kib <n>       Streamed ROM kept in flash (default: up to half the free flash)\n";
    std::cout << "  --meta-pack                       Streamed ROM LZSS-compressed in flash instead of the SD card\n";
    std::cout << "  --meta-dir <dir>      gbl target templates (normally auto-detected)\n";
    std::cout << "  --android             Also emit an Android project scaffold (single-ROM only)\n";
    std::cout << "  --android-package <p> Android Java package (default: io.gbrecompiled.<game>)\n";
    std::cout << "  --android-app-name <n> Android app name (default: ROM title)\n";
    std::cout << "  --single-function     Generate all code in a single function\n";
    std::cout << "  --comments            Include the disassembly comments (default: off, they\n"
                 "                        more than double the size of the generated C)\n";
    std::cout << "  --bank <n>            Only process bank n\n";
    std::cout << "  --add-entry-point b:a Add manual entry point (e.g. 1:4000)\n";
    std::cout << "  --no-scan             Disable speculative code seeding (enabled by default)\n";
    std::cout << "  --no-infer-data       Disable the static code/data sweep (enabled by default)\n";
    std::cout << "  --reachable-only      Analyze only reachable and explicitly annotated entry points\n";
    std::cout << "  --symbols <file>      Load a .sym symbol file and use names for generated functions and labels\n";
    std::cout << "  --symbol-policy <p>   Symbol analyzer policy: infer-boundaries (default) or names-only\n";
    std::cout << "  --annotations <file>  Load analyzer guidance (function/label/data ranges) from a text file\n";
    std::cout << "  --use-trace <file>    Use runtime trace to find entry points\n";
    std::cout << "  --version             Print the GB Recompiled version\n";
    std::cout << "  --version-json        Print machine-readable version and ABI identity\n";
    std::cout << "  --meta-version        Print the Gamebuino META edition version (stamped into firmwares)\n";
    std::cout << "  --espboy-version      Print the ESPboy edition version\n";
    std::cout << "  --pokitto-version     Print the Pokitto edition version (stamped into firmwares)\n";
    std::cout << "  -h, --help            Show this help\n";
}

static std::vector<uint16_t> find_oam_dma_overlay_destinations(const std::vector<uint8_t>& data,
                                                               uint16_t source_addr,
                                                               uint16_t overlay_size) {
    std::vector<uint16_t> destinations;
    if (overlay_size == 0 || overlay_size > 0xFF || data.size() < 14) {
        return destinations;
    }

    for (size_t i = 0; i + 13 < data.size(); ++i) {
        if (data[i] != 0x0E ||       // LD C,imm8
            data[i + 2] != 0x06 ||   // LD B,imm8
            data[i + 4] != 0x21 ||   // LD HL,imm16
            data[i + 7] != 0x2A ||   // LDI A,(HL)
            data[i + 8] != 0xE2 ||   // LD (C),A
            data[i + 9] != 0x0C ||   // INC C
            data[i + 10] != 0x05 ||  // DEC B
            data[i + 11] != 0x20 ||  // JR NZ,rel8
            data[i + 12] != 0xFA ||  // rel8 = -6
            data[i + 13] != 0xC9) {  // RET
            continue;
        }

        uint8_t count = data[i + 3];
        uint16_t copied_source = static_cast<uint16_t>(data[i + 5] | (data[i + 6] << 8));
        if (count != overlay_size || copied_source != source_addr) {
            continue;
        }

        destinations.push_back(static_cast<uint16_t>(0xFF00 | data[i + 1]));
    }

    std::sort(destinations.begin(), destinations.end());
    destinations.erase(std::unique(destinations.begin(), destinations.end()), destinations.end());
    return destinations;
}

static std::vector<gbrecomp::AnalyzerOptions::RamOverlay> detect_oam_dma_overlays(const gbrecomp::ROM& rom) {
    const std::vector<uint8_t>& data = rom.bytes();
    const std::vector<uint8_t> wait_pattern = {0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9};
    std::vector<gbrecomp::AnalyzerOptions::RamOverlay> overlays;

    auto it = std::search(data.begin(), data.end(), wait_pattern.begin(), wait_pattern.end());
    if (it == data.end()) {
        return overlays;
    }

    size_t rom_idx = static_cast<size_t>(std::distance(data.begin(), it));
    size_t overlay_start = rom_idx;
    uint16_t overlay_size = static_cast<uint16_t>(wait_pattern.size());

    // Many games copy the full helper starting with LD A,imm8 before the
    // standard wait loop body. If that prefix is present, compile the actual
    // copied entry point instead of the inner loop.
    if (rom_idx >= 2 && data[rom_idx - 2] == 0x3E) {
        overlay_start = rom_idx - 2;
        overlay_size += 2;
    }

    gbrecomp::BankId bank = static_cast<gbrecomp::BankId>(overlay_start / 0x4000);
    uint16_t offset = static_cast<uint16_t>(overlay_start % 0x4000);
    if (bank > 0) {
        offset = static_cast<uint16_t>(offset + 0x4000);
    }

    std::vector<uint16_t> destinations = find_oam_dma_overlay_destinations(data, offset, overlay_size);
    if (destinations.empty()) {
        destinations.push_back(0xFF80);
    }

    for (uint16_t ram_addr : destinations) {
        gbrecomp::AnalyzerOptions::RamOverlay overlay;
        overlay.ram_addr = ram_addr;
        overlay.rom_addr = gbrecomp::AnalysisResult::make_addr(bank, offset);
        overlay.size = overlay_size;
        overlays.push_back(overlay);
    }

    return overlays;
}

static void append_codegen_ram_overlays(
    const gbrecomp::ROM& rom,
    const std::vector<gbrecomp::AnalyzerOptions::RamOverlay>& overlays,
    gbrecomp::codegen::GeneratorOptions& gen_opts) {
    const uint8_t* rom_bytes = rom.data();
    for (const auto& overlay : overlays) {
        gbrecomp::BankId bank = static_cast<gbrecomp::BankId>(overlay.rom_addr >> 16);
        uint16_t offset = static_cast<uint16_t>(overlay.rom_addr & 0xFFFF);
        size_t rom_idx = (offset < 0x4000)
            ? offset
            : static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);
        if (rom_idx >= rom.size()) {
            continue;
        }

        size_t available = std::min<size_t>(overlay.size, rom.size() - rom_idx);
        if (available == 0) {
            continue;
        }

        gbrecomp::codegen::GeneratorOptions::RamOverlay codegen_overlay;
        codegen_overlay.ram_addr = overlay.ram_addr;
        codegen_overlay.bytes.assign(rom_bytes + rom_idx, rom_bytes + rom_idx + available);
        gen_opts.ram_overlays.push_back(std::move(codegen_overlay));
    }
}

std::string sanitize_prefix(const std::string& name) {
    std::string result = name;
    for (char& c : result) {
        if (!isalnum(c) && c != '_') {
            c = '_';
        }
    }
    if (!result.empty() && isdigit(result[0])) {
        result = "_" + result;
    }
    return result;
}

struct GenerationOptions {
    bool verbose = false;
    bool trace_log = false;
    size_t limit_instructions = 0;
    size_t codegen_jobs = 0;
    bool single_function = false;
    bool emit_comments = false;
    bool aggressive_scan = true;
    bool infer_data_zones = true;
    bool analyze_all_banks = true;
    std::vector<uint32_t> manual_entry_points;
    std::string trace_file_path;
    std::string symbol_file_path;
    gbrecomp::SymbolAnalysisPolicy symbol_analysis_policy =
        gbrecomp::SymbolAnalysisPolicy::INFER_BOUNDARIES;
    std::string annotation_file_path;
    bool emit_android_project = false;
    std::string android_package;
    std::string android_app_name;
    fs::path runtime_dir;
};

static fs::path resolve_runtime_dir(const char* argv0,
                                    const fs::path& explicit_dir) {
    std::vector<fs::path> candidates;
    if (!explicit_dir.empty()) {
        candidates.push_back(explicit_dir);
    }
    if (const char* environment_dir = std::getenv("GBRECOMP_RUNTIME_DIR")) {
        if (*environment_dir) {
            candidates.emplace_back(environment_dir);
        }
    }

    std::error_code error;
    const fs::path executable = fs::absolute(argv0, error);
    if (!error) {
        candidates.push_back(executable.parent_path() / "runtime");
        candidates.push_back(executable.parent_path().parent_path().parent_path() / "runtime");
    }
    candidates.push_back(fs::current_path() / "runtime");
#ifdef GBRECOMP_DEFAULT_RUNTIME_DIR
    candidates.emplace_back(GBRECOMP_DEFAULT_RUNTIME_DIR);
#endif

    for (const fs::path& candidate : candidates) {
        error.clear();
        if (fs::is_regular_file(candidate / "include" / "gbrt.h", error) &&
            fs::is_regular_file(candidate / "src" / "gbrt.c", error) &&
            fs::is_regular_file(candidate / "vendor" / "imgui" / "imgui.cpp", error)) {
            const fs::path canonical = fs::weakly_canonical(candidate, error);
            if (!error) {
                return canonical;
            }
        }
    }
    return {};
}

static bool copy_runtime_snapshot(const fs::path& runtime_dir,
                                  const fs::path& output_dir) {
    if (runtime_dir.empty()) {
        return false;
    }
    std::error_code error;
    const fs::path destination = output_dir / "runtime";
    fs::create_directories(destination, error);
    if (error) {
        return false;
    }

    for (fs::recursive_directory_iterator it(runtime_dir, error), end;
         !error && it != end;
         it.increment(error)) {
        const fs::path relative = fs::relative(it->path(), runtime_dir, error);
        if (error) {
            return false;
        }
        const auto first_component = relative.begin();
        const std::string root_name = first_component == relative.end()
                                          ? std::string()
                                          : first_component->string();
        const bool included_root = root_name == "include" ||
                                   root_name == "src" ||
                                   root_name == "vendor" ||
                                   root_name == "third_party" ||
                                   root_name == "CMakeLists.txt" ||
                                   root_name == "README.md" ||
                                   root_name == "LICENSE";
        if (!included_root) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const fs::path target = destination / relative;
        if (it->is_directory()) {
            fs::create_directories(target, error);
        } else if (it->is_regular_file()) {
            fs::create_directories(target.parent_path(), error);
            if (!error) {
                fs::copy_file(it->path(), target,
                              fs::copy_options::overwrite_existing,
                              error);
            }
        }
        if (error) {
            return false;
        }
    }

    const fs::path project_license = runtime_dir.parent_path() / "LICENSE";
    if (fs::is_regular_file(project_license, error)) {
        fs::copy_file(project_license,
                      destination / "LICENSE",
                      fs::copy_options::overwrite_existing,
                      error);
    } else {
        error.clear();
    }
    return !error;
}

struct MultiRomModule {
    std::string output_prefix;
    std::string display_name;
    std::string source_rom;
    gbrecomp::codegen::GeneratedOutput output;
};

static size_t resolve_parallel_job_count(size_t requested_jobs, size_t work_items) {
    if (work_items <= 1) {
        return 1;
    }
    if (requested_jobs == 1) {
        return 1;
    }

    size_t jobs = requested_jobs;
    if (jobs == 0) {
        jobs = std::thread::hardware_concurrency();
        if (jobs == 0) {
            jobs = 1;
        }
    }

    return std::min(jobs, work_items);
}

static std::mutex g_multi_rom_log_mutex;

static void log_multi_rom_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_multi_rom_log_mutex);
    std::cout << line << "\n";
}

static std::string escape_c_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                ss << "\\\\";
                break;
            case '"':
                ss << "\\\"";
                break;
            case '\n':
                ss << "\\n";
                break;
            case '\r':
                ss << "\\r";
                break;
            case '\t':
                ss << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    ss << '?';
                } else {
                    ss << static_cast<char>(ch);
                }
                break;
        }
    }
    return ss.str();
}

static std::string escape_json_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                ss << "\\\\";
                break;
            case '"':
                ss << "\\\"";
                break;
            case '\n':
                ss << "\\n";
                break;
            case '\r':
                ss << "\\r";
                break;
            case '\t':
                ss << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    ss << "\\u"
                       << std::hex << std::setfill('0') << std::setw(4)
                       << static_cast<unsigned>(ch)
                       << std::dec;
                } else {
                    ss << static_cast<char>(ch);
                }
                break;
        }
    }
    return ss.str();
}

static std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

static bool is_rom_file(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        return false;
    }

    const std::string ext = to_lower_ascii(path.extension().string());
    return ext == ".gb" || ext == ".gbc" || ext == ".sgb";
}

static std::vector<fs::path> collect_rom_paths(const fs::path& root) {
    std::vector<fs::path> rom_paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (is_rom_file(entry.path())) {
            rom_paths.push_back(entry.path());
        }
    }
    std::sort(rom_paths.begin(), rom_paths.end());
    return rom_paths;
}

static std::string make_unique_prefix(const std::string& base, std::set<std::string>& used_prefixes) {
    std::string candidate = sanitize_prefix(base);
    if (candidate.empty()) {
        candidate = "rom";
    }

    std::string unique = candidate;
    unsigned suffix = 2;
    while (!used_prefixes.insert(unique).second) {
        unique = candidate + "_" + std::to_string(suffix++);
    }
    return unique;
}

static std::string build_runtime_relative_path(const fs::path& output_dir) {
    (void)output_dir;
    return "runtime";
}

static bool write_text_file(const fs::path& path, const std::string& content) {
    if (!path.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

static std::string sanitize_java_package_segment(const std::string& name) {
    std::string result;
    result.reserve(name.size() + 4);

    for (unsigned char ch : name) {
        if (std::isalnum(ch)) {
            result.push_back((char)std::tolower(ch));
        } else {
            result.push_back('_');
        }
    }

    if (result.empty()) {
        result = "game";
    }
    if (std::isdigit((unsigned char)result.front())) {
        result.insert(result.begin(), '_');
    }

    return result;
}

static bool is_valid_java_package(const std::string& package_name) {
    if (package_name.empty()) {
        return false;
    }

    size_t start = 0;
    while (start < package_name.size()) {
        size_t end = package_name.find('.', start);
        if (end == std::string::npos) {
            end = package_name.size();
        }
        if (end == start) {
            return false;
        }

        const unsigned char first = (unsigned char)package_name[start];
        if (!(std::isalpha(first) || first == '_')) {
            return false;
        }

        for (size_t i = start + 1; i < end; ++i) {
            const unsigned char ch = (unsigned char)package_name[i];
            if (!(std::isalnum(ch) || ch == '_')) {
                return false;
            }
        }

        start = end + 1;
    }

    return true;
}

static std::string make_default_android_package(const std::string& output_prefix) {
    return "io.gbrecompiled." + sanitize_java_package_segment(output_prefix);
}

static std::string escape_xml_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char ch : input) {
        switch (ch) {
            case '&':
                ss << "&amp;";
                break;
            case '<':
                ss << "&lt;";
                break;
            case '>':
                ss << "&gt;";
                break;
            case '"':
                ss << "&quot;";
                break;
            case '\'':
                ss << "&apos;";
                break;
            default:
                if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
                    ss << '?';
                } else {
                    ss << static_cast<char>(ch);
                }
                break;
        }
    }
    return ss.str();
}

static std::string make_relative_path(const fs::path& from_dir, const fs::path& to_path) {
    try {
        return fs::relative(fs::absolute(to_path), fs::absolute(from_dir)).generic_string();
    } catch (...) {
        return fs::absolute(to_path).generic_string();
    }
}

static std::string java_package_to_path(const std::string& package_name) {
    std::string path = package_name;
    std::replace(path.begin(), path.end(), '.', '/');
    return path;
}

static std::vector<std::string> collect_generated_source_files(const gbrecomp::codegen::GeneratedOutput& output) {
    std::vector<std::string> source_files;
    if (!output.main_file.empty()) {
        source_files.push_back(output.main_file);
    }
    if (!output.source_file.empty()) {
        source_files.push_back(output.source_file);
    }
    for (const auto& extra_file : output.extra_files) {
        if (extra_file.is_source) {
            source_files.push_back(extra_file.filename);
        }
    }
    if (!output.rom_data_file.empty()) {
        source_files.push_back(output.rom_data_file);
    }
    return source_files;
}

static std::string make_android_settings_gradle(const std::string& project_name) {
    std::ostringstream ss;
    ss << "pluginManagement {\n";
    ss << "    repositories {\n";
    ss << "        google()\n";
    ss << "        mavenCentral()\n";
    ss << "        gradlePluginPortal()\n";
    ss << "    }\n";
    ss << "}\n\n";
    ss << "dependencyResolutionManagement {\n";
    ss << "    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)\n";
    ss << "    repositories {\n";
    ss << "        google()\n";
    ss << "        mavenCentral()\n";
    ss << "    }\n";
    ss << "}\n\n";
    ss << "rootProject.name = \"" << escape_c_string(project_name) << "-android\"\n";
    ss << "include(\":app\")\n";
    return ss.str();
}

static std::string make_android_root_build_gradle(void) {
    return R"GRADLE(plugins {
    id 'com.android.application' version '8.5.2' apply false
}
)GRADLE";
}

static std::string make_android_gradle_properties(void) {
    return R"PROPS(org.gradle.jvmargs=-Xmx4096m -Dfile.encoding=UTF-8
android.useAndroidX=true
android.nonTransitiveRClass=true
)PROPS";
}

static std::string make_android_gitignore(void) {
    return R"GITIGNORE(.gradle/
build/
app/build/
local.properties
captures/
)GITIGNORE";
}

static std::string make_android_app_build_gradle(const std::string& android_package) {
    std::ostringstream ss;
    ss << "plugins {\n";
    ss << "    id 'com.android.application'\n";
    ss << "}\n\n";
    ss << "def sdl2SourceDir = providers.gradleProperty(\"SDL2_SOURCE_DIR\").orNull ?: System.getenv(\"SDL2_SOURCE_DIR\")\n";
    ss << "if (!sdl2SourceDir) {\n";
    ss << "    throw new GradleException(\"SDL2_SOURCE_DIR is required. Set the Gradle property or environment variable SDL2_SOURCE_DIR to an SDL2 source checkout.\")\n";
    ss << "}\n\n";
    ss << "android {\n";
    ss << "    namespace \"" << android_package << "\"\n";
    ss << "    compileSdk 34\n\n";
    ss << "    defaultConfig {\n";
    ss << "        applicationId \"" << android_package << "\"\n";
    ss << "        minSdk 24\n";
    ss << "        targetSdk 34\n";
    ss << "        versionCode 1\n";
    ss << "        versionName \"1.0\"\n";
    ss << "        externalNativeBuild {\n";
    ss << "            cmake {\n";
    ss << "                arguments \"-DANDROID_STL=c++_shared\", \"-DSDL2_SOURCE_DIR=${sdl2SourceDir}\"\n";
    ss << "                abiFilters \"arm64-v8a\"\n";
    ss << "            }\n";
    ss << "        }\n";
    ss << "    }\n\n";
    ss << "    buildTypes {\n";
    ss << "        debug {\n";
    ss << "            debuggable true\n";
    ss << "        }\n";
    ss << "        release {\n";
    ss << "            minifyEnabled false\n";
    ss << "            proguardFiles getDefaultProguardFile(\"proguard-android-optimize.txt\"), \"proguard-rules.pro\"\n";
    ss << "        }\n";
    ss << "    }\n\n";
    ss << "    externalNativeBuild {\n";
    ss << "        cmake {\n";
    ss << "            path file(\"jni/CMakeLists.txt\")\n";
    ss << "        }\n";
    ss << "    }\n\n";
    ss << "    sourceSets {\n";
    ss << "        main {\n";
    ss << "            java.srcDirs += [file(\"${sdl2SourceDir}/android-project/app/src/main/java\")]\n";
    ss << "        }\n";
    ss << "    }\n\n";
    ss << "    lint {\n";
    ss << "        abortOnError false\n";
    ss << "    }\n";
    ss << "}\n";
    return ss.str();
}

static std::string make_android_proguard_rules(void) {
    return "# Intentionally empty for generated debug-friendly builds.\n";
}

static std::string make_android_manifest(const std::string& android_package) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    ss << "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">\n\n";
    ss << "    <uses-feature android:name=\"android.hardware.touchscreen\" android:required=\"false\" />\n";
    ss << "    <uses-feature android:name=\"android.hardware.gamepad\" android:required=\"false\" />\n";
    ss << "    <uses-feature android:name=\"android.hardware.bluetooth\" android:required=\"false\" />\n";
    ss << "    <uses-feature android:name=\"android.hardware.usb.host\" android:required=\"false\" />\n";
    ss << "    <uses-feature android:name=\"android.hardware.type.pc\" android:required=\"false\" />\n";
    ss << "    <uses-permission android:name=\"android.permission.VIBRATE\" />\n\n";
    ss << "    <application\n";
    ss << "        android:allowBackup=\"true\"\n";
    ss << "        android:hardwareAccelerated=\"true\"\n";
    ss << "        android:label=\"@string/app_name\">\n";
    ss << "        <meta-data android:name=\"SDL_ENV.SDL_ANDROID_TRAP_BACK_BUTTON\" android:value=\"1\" />\n";
    ss << "        <activity\n";
    ss << "            android:name=\"" << escape_xml_string(android_package) << ".GameActivity\"\n";
    ss << "            android:alwaysRetainTaskState=\"true\"\n";
    ss << "            android:configChanges=\"layoutDirection|locale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation\"\n";
    ss << "            android:exported=\"true\"\n";
    ss << "            android:label=\"@string/app_name\"\n";
    ss << "            android:launchMode=\"singleTask\"\n";
    ss << "            android:preferMinimalPostProcessing=\"true\"\n";
    ss << "            android:screenOrientation=\"landscape\">\n";
    ss << "            <intent-filter>\n";
    ss << "                <action android:name=\"android.intent.action.MAIN\" />\n";
    ss << "                <category android:name=\"android.intent.category.LAUNCHER\" />\n";
    ss << "            </intent-filter>\n";
    ss << "        </activity>\n";
    ss << "    </application>\n";
    ss << "</manifest>\n";
    return ss.str();
}

static std::string make_android_strings_xml(const std::string& app_name) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    ss << "<resources>\n";
    ss << "    <string name=\"app_name\">" << escape_xml_string(app_name) << "</string>\n";
    ss << "</resources>\n";
    return ss.str();
}

static std::string make_android_activity_java(const std::string& android_package) {
    std::ostringstream ss;
    ss << "package " << android_package << ";\n\n";
    ss << "import org.libsdl.app.SDLActivity;\n\n";
    ss << "public final class GameActivity extends SDLActivity {\n";
    ss << "}\n";
    return ss.str();
}

static std::string make_android_jni_cmake(void) {
    return R"CMAKE(cmake_minimum_required(VERSION 3.16)

project(gbrecomp_android_host C CXX)

if(NOT DEFINED SDL2_SOURCE_DIR OR SDL2_SOURCE_DIR STREQUAL "")
    if(DEFINED ENV{SDL2_SOURCE_DIR} AND NOT "$ENV{SDL2_SOURCE_DIR}" STREQUAL "")
        set(SDL2_SOURCE_DIR "$ENV{SDL2_SOURCE_DIR}")
    endif()
endif()

if(NOT DEFINED SDL2_SOURCE_DIR OR SDL2_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SDL2_SOURCE_DIR is required. Set the Gradle property or environment variable SDL2_SOURCE_DIR to an SDL2 source checkout.")
endif()

get_filename_component(SDL2_SOURCE_DIR "${SDL2_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${SDL2_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SDL2_SOURCE_DIR='${SDL2_SOURCE_DIR}' does not point to an SDL2 source tree.")
endif()

add_subdirectory("${SDL2_SOURCE_DIR}" SDL EXCLUDE_FROM_ALL)
add_subdirectory(src)
)CMAKE";
}

static std::string make_android_jni_src_cmake(const fs::path& output_dir,
                                              const std::string& output_prefix,
                                              const gbrecomp::codegen::GeneratedOutput& output) {
    const fs::path src_dir = output_dir / "android" / "app" / "jni" / "src";
    const fs::path runtime_dir = fs::absolute(output_dir / build_runtime_relative_path(output_dir));
    const std::string output_rel = make_relative_path(src_dir, output_dir);
    const std::string runtime_rel = make_relative_path(src_dir, runtime_dir);
    const std::vector<std::string> generated_source_files = collect_generated_source_files(output);

    std::ostringstream ss;
    ss << "cmake_minimum_required(VERSION 3.16)\n\n";
    ss << "project(" << output_prefix << "_android_runtime C CXX)\n\n";
    ss << "set(CMAKE_C_STANDARD 11)\n";
    ss << "set(CMAKE_C_STANDARD_REQUIRED ON)\n";
    ss << "set(CMAKE_CXX_STANDARD 20)\n";
    ss << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    ss << "set(GBRECOMP_GENERATED_COMPILE_JOBS \"8\" CACHE STRING \"Maximum concurrent generated-source compiles under Ninja; 0 disables the pool\")\n\n";
    ss << "set(GBRECOMP_OUT_DIR \"${CMAKE_CURRENT_LIST_DIR}/" << output_rel << "\")\n";
    ss << "set(GBRT_DIR \"${CMAKE_CURRENT_LIST_DIR}/" << runtime_rel << "\")\n\n";
    ss << "if(TARGET SDL2::SDL2)\n";
    ss << "    set(GBRECOMP_SDL_TARGET SDL2::SDL2)\n";
    ss << "elseif(TARGET SDL2)\n";
    ss << "    set(GBRECOMP_SDL_TARGET SDL2)\n";
    ss << "elseif(TARGET SDL2-static)\n";
    ss << "    set(GBRECOMP_SDL_TARGET SDL2-static)\n";
    ss << "else()\n";
    ss << "    message(FATAL_ERROR \"Unable to find an SDL2 CMake target after adding SDL2_SOURCE_DIR\")\n";
    ss << "endif()\n\n";
    ss << "if(TARGET SDL2::SDL2main)\n";
    ss << "    set(GBRECOMP_SDL_MAIN_TARGET SDL2::SDL2main)\n";
    ss << "elseif(TARGET SDL2main)\n";
    ss << "    set(GBRECOMP_SDL_MAIN_TARGET SDL2main)\n";
    ss << "else()\n";
    ss << "    set(GBRECOMP_SDL_MAIN_TARGET \"\")\n";
    ss << "endif()\n\n";
    ss << "add_library(gbrt STATIC\n";
    ss << "    ${GBRT_DIR}/src/gbrt.c\n";
    ss << "    ${GBRT_DIR}/src/differential.c\n";
    ss << "    ${GBRT_DIR}/src/ppu.c\n";
    ss << "    ${GBRT_DIR}/src/audio.c\n";
    ss << "    ${GBRT_DIR}/src/audio_stats.c\n";
    ss << "    ${GBRT_DIR}/src/interpreter.c\n";
    ss << "    ${GBRT_DIR}/src/platform_sdl.cpp\n";
    if (output.has_native_patch) {
        ss << "    ${GBRT_DIR}/src/gbrt_native_patch.c\n";
    }
    ss << ")\n\n";
    ss << "target_sources(gbrt PRIVATE\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_draw.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_tables.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_widgets.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/backends/imgui_impl_sdl2.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp\n";
    ss << ")\n";
    ss << "target_include_directories(gbrt PUBLIC\n";
    ss << "    ${GBRT_DIR}/include\n";
    ss << "    ${GBRT_DIR}/vendor/imgui\n";
    ss << ")\n";
    ss << "target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2";
    if (output.has_native_patch) {
        ss << " GBRT_ENABLE_NATIVE_PATCHES";
    }
    ss << ")\n";
    ss << "target_compile_features(gbrt PUBLIC c_std_11 cxx_std_20)\n";
    ss << "target_link_libraries(gbrt PUBLIC ${GBRECOMP_SDL_TARGET})\n\n";
    ss << "add_library(main SHARED\n";
    for (const auto& filename : generated_source_files) {
        ss << "    ${GBRECOMP_OUT_DIR}/" << filename << "\n";
    }
    ss << ")\n\n";
    ss << "if(CMAKE_GENERATOR MATCHES \"Ninja\" AND GBRECOMP_GENERATED_COMPILE_JOBS MATCHES \"^[1-9][0-9]*$\")\n";
    ss << "    set_property(GLOBAL APPEND PROPERTY JOB_POOLS \"gbrecomp_generated_compile_pool=${GBRECOMP_GENERATED_COMPILE_JOBS}\")\n";
    ss << "    set_property(TARGET main PROPERTY JOB_POOL_COMPILE gbrecomp_generated_compile_pool)\n";
    ss << "endif()\n\n";
    ss << "target_include_directories(main PRIVATE\n";
    ss << "    ${GBRECOMP_OUT_DIR}\n";
    ss << "    ${GBRT_DIR}/include\n";
    ss << "    ${GBRT_DIR}/vendor/imgui\n";
    ss << ")\n";
    ss << "set(GBRECOMP_GENERATED_SOURCES\n";
    for (const auto& filename : generated_source_files) {
        ss << "    ${GBRECOMP_OUT_DIR}/" << filename << "\n";
    }
    ss << ")\n";
    ss << "if(CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    ss << "    target_compile_options(gbrt PRIVATE -O0 -g)\n";
    ss << "else()\n";
    ss << "    target_compile_options(gbrt PRIVATE -O3)\n";
    ss << "    set_source_files_properties(${GBRECOMP_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS \"-O3\")\n";
    ss << "endif()\n";
    ss << "target_link_libraries(main PRIVATE gbrt ${GBRECOMP_SDL_TARGET})\n";
    ss << "if(GBRECOMP_SDL_MAIN_TARGET)\n";
    ss << "    target_link_libraries(main PRIVATE ${GBRECOMP_SDL_MAIN_TARGET})\n";
    ss << "endif()\n";
    return ss.str();
}

static std::string make_android_readme(const fs::path& output_dir,
                                       const std::string& app_name,
                                       const std::string& android_package) {
    std::ostringstream ss;
    ss << "# " << app_name << " Android Scaffold\n\n";
    ss << "This directory was generated by `gbrecomp --android`.\n\n";
    ss << "Requirements:\n";
    ss << "- Android Studio or Gradle with the Android SDK/NDK installed\n";
    ss << "- An SDL2 source checkout available through `SDL2_SOURCE_DIR`\n\n";
    ss << "Package: `" << android_package << "`\n";
    ss << "ABI: `arm64-v8a`\n";
    ss << "Min SDK: `24`\n";
    ss << "Target SDK: `34`\n";
    ss << "Orientation: landscape\n\n";
    ss << "Default controller mapping:\n";
    ss << "- D-pad or left stick: move\n";
    ss << "- Bottom face button (`Xbox A` / `Switch B` / `Cross`): Game Boy `B`\n";
    ss << "- Right face button (`Xbox B` / `Switch A` / `Circle`): Game Boy `A`\n";
    ss << "- Left shoulder: Game Boy `B`\n";
    ss << "- Right shoulder: Game Boy `A`\n";
    ss << "- Start / Menu: `Start`\n";
    ss << "- Back / View / Share: `Select`\n";
    ss << "- Guide / Home or Android Back: open the runtime settings menu\n\n";
    ss << "Build from the repo root:\n";
    ss << "```bash\n";
    ss << "SDL2_SOURCE_DIR=/path/to/SDL gradle -p " << (output_dir / "android").generic_string() << " :app:assembleDebug\n";
    ss << "```\n\n";
    ss << "Open `" << (output_dir / "android").generic_string() << "` in Android Studio if you prefer the IDE flow.\n";
    ss << "The native build will fail fast if `SDL2_SOURCE_DIR` is missing.\n";
    ss << "This v1 Android scaffold is controller-first and does not include touch gameplay controls.\n";
    return ss.str();
}

static bool write_android_project(const fs::path& output_dir,
                                  const std::string& output_prefix,
                                  const gbrecomp::codegen::GeneratedOutput& output,
                                  const std::string& android_package,
                                  const std::string& android_app_name) {
    const fs::path android_dir = output_dir / "android";
    const fs::path package_java_dir = android_dir / "app" / "src" / "main" / "java" / java_package_to_path(android_package);

    return write_text_file(android_dir / "settings.gradle", make_android_settings_gradle(output_prefix)) &&
           write_text_file(android_dir / "build.gradle", make_android_root_build_gradle()) &&
           write_text_file(android_dir / "gradle.properties", make_android_gradle_properties()) &&
           write_text_file(android_dir / ".gitignore", make_android_gitignore()) &&
           write_text_file(android_dir / "README.md", make_android_readme(output_dir, android_app_name, android_package)) &&
           write_text_file(android_dir / "app" / "build.gradle", make_android_app_build_gradle(android_package)) &&
           write_text_file(android_dir / "app" / "proguard-rules.pro", make_android_proguard_rules()) &&
           write_text_file(android_dir / "app" / "src" / "main" / "AndroidManifest.xml", make_android_manifest(android_package)) &&
           write_text_file(android_dir / "app" / "src" / "main" / "res" / "values" / "strings.xml", make_android_strings_xml(android_app_name)) &&
           write_text_file(package_java_dir / "GameActivity.java", make_android_activity_java(android_package)) &&
           write_text_file(android_dir / "app" / "jni" / "CMakeLists.txt", make_android_jni_cmake()) &&
           write_text_file(android_dir / "app" / "jni" / "src" / "CMakeLists.txt",
                           make_android_jni_src_cmake(output_dir, output_prefix, output));
}

static std::string make_launcher_source(const std::string& launcher_name,
                                        const std::vector<MultiRomModule>& modules) {
    std::ostringstream ss;
    ss << "/* Multi-ROM launcher generated by gbrecomp */\n";
    ss << "#define SDL_MAIN_HANDLED\n";
    ss << "extern \"C\" {\n";
    for (const MultiRomModule& module : modules) {
        ss << "#include \"" << module.output_prefix << ".h\"\n";
    }
    ss << "}\n\n";
    ss << R"LAUNCHER(#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sdl.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

typedef int (*GBLauncherMainFn)(int argc, char* argv[]);

typedef struct {
    const char* id;
    const char* title;
    const char* rom_path;
    GBLauncherMainFn main_fn;
} GBLauncherGame;

)LAUNCHER";
    for (const MultiRomModule& module : modules) {
        ss << "static int launch_" << module.output_prefix << "(int argc, char* argv[]) {\n";
        ss << "    return " << module.output_prefix << "_main(argc, argv);\n";
        ss << "}\n";
    }
    ss << "\n";
    ss << "static GBLauncherGame g_games[] = {\n";
    for (const MultiRomModule& module : modules) {
        ss << "    {\""
           << escape_c_string(module.output_prefix) << "\", \""
           << escape_c_string(module.display_name) << "\", \""
           << escape_c_string(module.source_rom) << "\", "
           << "launch_" << module.output_prefix << "},\n";
    }
    ss << "};\n\n";
    ss << "static const char* g_launcher_name = \"" << escape_c_string(launcher_name) << "\";\n";
    ss << R"LAUNCHER(static const size_t g_game_count = sizeof(g_games) / sizeof(g_games[0]);

static char* trim_ascii(char* text) {
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    if (!text || !*text) {
        return text;
    }
    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [--list-games] [--game <id>] [game arguments...]\n", program);
    fprintf(stderr, "Run without --game to open the graphical launcher.\n");
}

static void print_games(void) {
    fprintf(stderr, "Available games in %s:\n", g_launcher_name);
    for (size_t i = 0; i < g_game_count; i++) {
        fprintf(stderr, "  %zu. %s [%s]\n", i + 1, g_games[i].title, g_games[i].id);
    }
}

static const GBLauncherGame* find_game_by_id(const char* id) {
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < g_game_count; i++) {
        if (strcmp(g_games[i].id, id) == 0) {
            return &g_games[i];
        }
    }
    return NULL;
}

static const GBLauncherGame* prompt_for_game(void) {
    char line[256];
    print_games();
    fprintf(stderr, "Select a game by number or id: ");
    if (!fgets(line, sizeof(line), stdin)) {
        return NULL;
    }
    char* selection = trim_ascii(line);
    if (!selection || !*selection) {
        return NULL;
    }
    char* end = NULL;
    long index = strtol(selection, &end, 10);
    end = trim_ascii(end);
    if (selection != end && end && !*end) {
        if (index >= 1 && (size_t)index <= g_game_count) {
            return &g_games[index - 1];
        }
    }
    return find_game_by_id(selection);
}

static bool draw_game_row(const GBLauncherGame* game, int index, bool is_selected, bool* out_double_clicked) {
    const float row_height = ImGui::GetTextLineHeight() * 2.8f + 10.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImGui::PushID(index);
    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, row_height);
    if (size.x < 1.0f) {
        size.x = 1.0f;
    }

    ImVec2 min = ImGui::GetCursorScreenPos();
    bool activated = ImGui::InvisibleButton("game_row", size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(0);
    ImVec2 max = ImGui::GetItemRectMax();
    ImVec2 title_pos = ImVec2(min.x + 12.0f, min.y + 8.0f);
    ImVec2 id_pos = ImVec2(title_pos.x, title_pos.y + ImGui::GetTextLineHeight() + 4.0f);

    ImU32 bg_color = ImGui::GetColorU32(is_selected ? ImGuiCol_Header : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    if (held) {
        bg_color = ImGui::GetColorU32(ImGuiCol_HeaderActive);
    }
    draw_list->AddRectFilled(min, max, bg_color, 10.0f);
    if (is_selected) {
        draw_list->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 6.0f),
                                 ImVec2(min.x + 6.0f, max.y - 6.0f),
                                 ImGui::GetColorU32(ImGuiCol_Button),
                                 3.0f);
    }

    ImGui::PushClipRect(ImVec2(min.x + 10.0f, min.y), ImVec2(max.x - 10.0f, max.y), true);
    draw_list->AddText(title_pos, ImGui::GetColorU32(ImGuiCol_Text), game->title);
    draw_list->AddText(id_pos, ImGui::GetColorU32(ImGuiCol_TextDisabled), game->id);
    ImGui::PopClipRect();
    ImGui::PopID();

    if (out_double_clicked) {
        *out_double_clicked = double_clicked;
    }
    return activated;
}

static void apply_launcher_style(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.ChildRounding = 16.0f;
    style.FrameRounding = 12.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 12.0f;
    style.ScrollbarRounding = 12.0f;
    style.WindowPadding = ImVec2(20.0f, 20.0f);
    style.FramePadding = ImVec2(14.0f, 10.0f);
    style.ItemSpacing = ImVec2(12.0f, 12.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.12f, 0.16f, 0.95f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.27f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.11f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.45f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.56f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.37f, 0.33f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.31f, 0.41f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.22f, 0.31f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.31f, 0.40f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.24f, 0.31f, 0.40f, 0.55f);
}

static int init_graphical_launcher(SDL_Window** out_window, SDL_Renderer** out_renderer) {
    *out_window = NULL;
    *out_renderer = NULL;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[LAUNCH] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window* window = SDL_CreateWindow(
        "GameBoy Recompiled Launcher",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1040,
        640,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        fprintf(stderr, "[LAUNCH] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "[LAUNCH] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    apply_launcher_style();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    *out_window = window;
    *out_renderer = renderer;
    return 1;
}

static void shutdown_graphical_launcher(SDL_Window* window, SDL_Renderer* renderer) {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

static int run_graphical_launcher(const GBLauncherGame** out_selected) {
    *out_selected = NULL;
    if (g_game_count == 0) {
        return 1;
    }

    SDL_Window* window = NULL;
    SDL_Renderer* renderer = NULL;
    if (!init_graphical_launcher(&window, &renderer)) {
        return 0;
    }

    int selected_index = 0;
    bool running = true;
    bool accepted = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("Launcher", NULL, window_flags)) {
            const GBLauncherGame* game = &g_games[selected_index];
            float list_width = ImGui::GetContentRegionAvail().x * 0.46f;

            ImGui::TextUnformatted("GameBoy Recompiled");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_launcher_name);
            ImGui::Separator();
            ImGui::TextWrapped("Choose one of the generated ROMs and press Launch Game. Any additional command line arguments will be forwarded to the selected runtime.");
            ImGui::Spacing();

            ImGui::BeginChild("game_list", ImVec2(list_width, -72.0f), true);
            ImGui::TextDisabled("Available Games");
            ImGui::Separator();
            for (size_t i = 0; i < g_game_count; i++) {
                bool is_selected = selected_index == (int)i;
                bool double_clicked = false;
                if (draw_game_row(&g_games[i], (int)i, is_selected, &double_clicked)) {
                    selected_index = (int)i;
                    if (double_clicked) {
                        accepted = true;
                        running = false;
                    }
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("game_details", ImVec2(0.0f, -72.0f), true);
            ImGui::TextDisabled("Selected Game");
            ImGui::Separator();
            ImGui::Text("%s", game->title);
            ImGui::Spacing();
            ImGui::TextDisabled("Game ID");
            ImGui::TextUnformatted(game->id);
            ImGui::Spacing();
            ImGui::TextDisabled("Source ROM");
            ImGui::TextWrapped("%s", game->rom_path);
            ImGui::Spacing();
            ImGui::TextDisabled("Controls");
            ImGui::BulletText("Double-click a game to launch immediately.");
            ImGui::BulletText("Press Enter to launch the current selection.");
            ImGui::BulletText("Press Escape or Quit to close the launcher.");
            ImGui::EndChild();

            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                accepted = true;
                running = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                running = false;
            }

            if (ImGui::Button("Launch Game", ImVec2(180.0f, 0.0f))) {
                accepted = true;
                running = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(120.0f, 0.0f))) {
                running = false;
            }
        }
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 11, 14, 19, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    if (accepted && selected_index >= 0 && (size_t)selected_index < g_game_count) {
        *out_selected = &g_games[selected_index];
    }

    shutdown_graphical_launcher(window, renderer);
    return 1;
}

int main(int argc, char* argv[]) {
    const GBLauncherGame* selected = NULL;
    char** forwarded_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    int forwarded_argc = 1;
    if (!forwarded_argv) {
        fprintf(stderr, "Failed to allocate launcher argument buffer\n");
        return 1;
    }

    forwarded_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-games") == 0) {
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if (strcmp(argv[i], "--game") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --game\n");
                print_usage(argv[0]);
                free(forwarded_argv);
                return 1;
            }
            selected = find_game_by_id(argv[++i]);
            if (!selected) {
                fprintf(stderr, "Unknown game id '%s'\n", argv[i]);
                print_games();
                free(forwarded_argv);
                return 1;
            }
            continue;
        }
        forwarded_argv[forwarded_argc++] = argv[i];
    }
    forwarded_argv[forwarded_argc] = NULL;

    for (;;) {
        if (!selected) {
            if (run_graphical_launcher(&selected)) {
                if (!selected) {
                    free(forwarded_argv);
                    return 0;
                }
            } else {
                fprintf(stderr, "[LAUNCH] Graphical launcher unavailable, falling back to terminal selection.\n");
                selected = prompt_for_game();
            }
            if (!selected) {
                fprintf(stderr, "No game selected\n");
                free(forwarded_argv);
                return 1;
            }
        }

        fprintf(stderr, "[LAUNCH] Starting %s [%s]\n", selected->title, selected->id);
        gb_platform_set_launcher_return_enabled(true);
        int rc = selected->main_fn(forwarded_argc, forwarded_argv);
        if (rc == GB_PLATFORM_RETURN_TO_LAUNCHER_EXIT_CODE) {
            selected = NULL;
            continue;
        }
        free(forwarded_argv);
        return rc;
    }
}
)LAUNCHER";
    return ss.str();
}

static std::string make_launcher_manifest(const std::string& launcher_name,
                                          const std::vector<MultiRomModule>& modules) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"launcher\": \"" << escape_json_string(launcher_name) << "\",\n";
    ss << "  \"games\": [\n";
    for (size_t i = 0; i < modules.size(); i++) {
        const MultiRomModule& module = modules[i];
        ss << "    {\n";
        ss << "      \"id\": \"" << escape_json_string(module.output_prefix) << "\",\n";
        ss << "      \"title\": \"" << escape_json_string(module.display_name) << "\",\n";
        ss << "      \"rom_path\": \"" << escape_json_string(module.source_rom) << "\",\n";
        ss << "      \"metadata_file\": \""
           << escape_json_string(module.output_prefix + "_metadata.json") << "\"\n";
        ss << "    }" << (i + 1 < modules.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

static std::string make_multi_rom_cmake(const std::string& project_name,
                                        const fs::path& output_dir,
                                        const std::vector<MultiRomModule>& modules) {
    const std::string runtime_path = build_runtime_relative_path(output_dir);

    std::vector<std::string> executable_sources = {"launcher_main.cpp"};
    std::vector<std::string> generated_sources;

    for (const MultiRomModule& module : modules) {
        executable_sources.push_back(module.output.main_file);
        executable_sources.push_back(module.output.source_file);
        executable_sources.push_back(module.output.rom_data_file);
        generated_sources.push_back(module.output.main_file);
        generated_sources.push_back(module.output.source_file);
        for (const auto& extra_file : module.output.extra_files) {
            if (extra_file.is_source) {
                executable_sources.push_back(extra_file.filename);
                generated_sources.push_back(extra_file.filename);
            }
        }
    }

    std::ostringstream ss;
    ss << "cmake_minimum_required(VERSION 3.16)\n";
    ss << "project(" << project_name << " C CXX)\n\n";
    ss << "include(CheckIPOSupported)\n\n";
    ss << "set(CMAKE_C_STANDARD 11)\n";
    ss << "set(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    ss << "set(CMAKE_CXX_STANDARD 20)\n";
    ss << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    ss << "set(GBRECOMP_GENERATED_COMPILE_JOBS \"8\" CACHE STRING \"Maximum concurrent generated-source compiles under Ninja; 0 disables the pool\")\n\n";
    ss << "if(NOT CMAKE_BUILD_TYPE)\n";
    ss << "    set(CMAKE_BUILD_TYPE Release)\n";
    ss << "endif()\n";
    ss << "if(CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n";
    ss << "    if(CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    ss << "        add_compile_options(-O0 -g)\n";
    ss << "    else()\n";
    ss << "        add_compile_options(-O3)\n";
    ss << "    endif()\n";
    ss << "endif()\n\n";
    ss << "set(GBRT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/" << runtime_path << "\")\n\n";
    ss << "find_package(SDL2 REQUIRED)\n\n";
    ss << "add_library(gbrt STATIC\n";
    ss << "    ${GBRT_DIR}/src/gbrt.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_data_mod.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_hash.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_host_configuration.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_port.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_presentation.c\n";
    ss << "    ${GBRT_DIR}/src/gbrt_semantic.c\n";
    ss << "    ${GBRT_DIR}/src/differential.c\n";
    ss << "    ${GBRT_DIR}/src/ppu.c\n";
    ss << "    ${GBRT_DIR}/src/audio.c\n";
    ss << "    ${GBRT_DIR}/src/audio_stats.c\n";
    ss << "    ${GBRT_DIR}/src/interpreter.c\n";
    ss << "    ${GBRT_DIR}/src/platform_sdl.cpp\n";
    ss << ")\n\n";
    ss << "target_sources(gbrt PRIVATE\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_draw.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_tables.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/imgui_widgets.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/backends/imgui_impl_sdl2.cpp\n";
    ss << "    ${GBRT_DIR}/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp\n";
    ss << ")\n";
    ss << "target_include_directories(gbrt PUBLIC\n";
    ss << "    ${GBRT_DIR}/include\n";
    ss << "    ${GBRT_DIR}/vendor/imgui\n";
    ss << ")\n";
    ss << "target_link_libraries(gbrt PUBLIC SDL2::SDL2)\n";
    ss << "target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2)\n\n";
    ss << "add_executable(" << project_name << "\n";
    for (const std::string& filename : executable_sources) {
        ss << "    " << filename << "\n";
    }
    ss << ")\n\n";
    ss << "if(CMAKE_GENERATOR MATCHES \"Ninja\" AND GBRECOMP_GENERATED_COMPILE_JOBS MATCHES \"^[1-9][0-9]*$\")\n";
    ss << "    set_property(GLOBAL APPEND PROPERTY JOB_POOLS \"gbrecomp_generated_compile_pool=${GBRECOMP_GENERATED_COMPILE_JOBS}\")\n";
    ss << "    set_property(TARGET " << project_name << " PROPERTY JOB_POOL_COMPILE gbrecomp_generated_compile_pool)\n";
    ss << "endif()\n\n";
    ss << "set(GBRECOMP_GENERATED_OPT_LEVEL \"3\" CACHE STRING \"Optimization level for generated ROM source files\")\n";
    ss << "set_property(CACHE GBRECOMP_GENERATED_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)\n";
    ss << "option(GBRECOMP_ENABLE_IPO \"Enable interprocedural optimization/LTO for non-Debug builds\" ON)\n";
    ss << "set(GBRECOMP_GENERATED_SOURCES\n";
    for (const std::string& filename : generated_sources) {
        ss << "    " << filename << "\n";
    }
    ss << ")\n";
    ss << "if(NOT CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    ss << "    if(MSVC)\n";
    ss << "        if(GBRECOMP_GENERATED_OPT_LEVEL STREQUAL \"0\")\n";
    ss << "            set(GBRECOMP_GENERATED_OPT_FLAG \"/Od\")\n";
    ss << "        elseif(GBRECOMP_GENERATED_OPT_LEVEL STREQUAL \"1\")\n";
    ss << "            set(GBRECOMP_GENERATED_OPT_FLAG \"/O1\")\n";
    ss << "        else()\n";
    ss << "            set(GBRECOMP_GENERATED_OPT_FLAG \"/O2\")\n";
    ss << "        endif()\n";
    ss << "        set_source_files_properties(${GBRECOMP_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS \"${GBRECOMP_GENERATED_OPT_FLAG}\")\n";
    ss << "    else()\n";
    ss << "        set_source_files_properties(${GBRECOMP_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS \"-O${GBRECOMP_GENERATED_OPT_LEVEL}\")\n";
    ss << "    endif()\n";
    ss << "endif()\n\n";
    ss << "if(GBRECOMP_ENABLE_IPO AND NOT CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    ss << "    check_ipo_supported(RESULT GBRECOMP_IPO_SUPPORTED OUTPUT GBRECOMP_IPO_ERROR)\n";
    ss << "    if(GBRECOMP_IPO_SUPPORTED)\n";
    ss << "        set_property(TARGET gbrt PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)\n";
    ss << "        set_property(TARGET " << project_name << " PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)\n";
    ss << "    else()\n";
    ss << "        message(STATUS \"GB Recompiled: IPO/LTO not enabled (${GBRECOMP_IPO_ERROR})\")\n";
    ss << "    endif()\n";
    ss << "endif()\n\n";
    ss << "target_link_libraries(" << project_name << " gbrt)\n";
    ss << "if(TARGET SDL2::SDL2main)\n";
    ss << "    target_link_libraries(" << project_name << " SDL2::SDL2main)\n";
    ss << "elseif(TARGET SDL2main)\n";
    ss << "    target_link_libraries(" << project_name << " SDL2main)\n";
    ss << "endif()\n";
    return ss.str();
}

static bool generate_multi_rom_module(const fs::path& rom_path,
                                      const GenerationOptions& options,
                                      const fs::path& output_dir,
                                      const std::string& output_prefix,
                                      MultiRomModule& module) {
    {
        std::ostringstream ss;
        ss << "\n[" << output_prefix << "] Loading ROM: " << rom_path;
        log_multi_rom_line(ss.str());
    }

    auto rom_opt = gbrecomp::ROM::load(rom_path);
    if (!rom_opt) {
        std::cerr << "Error: Failed to load ROM " << rom_path << "\n";
        return false;
    }

    auto& rom = *rom_opt;
    if (!rom.is_valid()) {
        std::cerr << "Error: " << rom.error() << "\n";
        return false;
    }

    const std::string display_name = !rom.header().title.empty() ? rom.header().title : rom.name();
    {
        std::ostringstream ss;
        ss << "[" << output_prefix << "] Title: " << display_name;
        log_multi_rom_line(ss.str());
    }
    if (options.verbose) {
        gbrecomp::print_rom_info(rom);
    }

    gbrecomp::SymbolTable symbol_table;
    if (!options.symbol_file_path.empty()) {
        std::string error;
        if (!symbol_table.load_sym_file(options.symbol_file_path, &rom, &error)) {
            std::cerr << "Error: " << error << "\n";
            return false;
        }
    }
    if (!options.annotation_file_path.empty()) {
        std::string error;
        if (!symbol_table.load_annotation_file(options.annotation_file_path, &error)) {
            std::cerr << "Error: " << error << "\n";
            return false;
        }
    }

    gbrecomp::AnalyzerOptions analyze_opts;
    analyze_opts.trace_log = options.trace_log;
    analyze_opts.verbose = options.verbose;
    analyze_opts.max_instructions = options.limit_instructions;
    analyze_opts.entry_points = options.manual_entry_points;
    analyze_opts.analyze_all_banks = options.analyze_all_banks;
    analyze_opts.aggressive_scan = options.aggressive_scan;
    analyze_opts.infer_data_zones = options.infer_data_zones;
    analyze_opts.trace_file_path = options.trace_file_path;
    analyze_opts.annotations = gbrecomp::build_analysis_annotations(
        symbol_table, options.symbol_analysis_policy);

    const std::vector<gbrecomp::AnalyzerOptions::RamOverlay> dma_overlays =
        detect_oam_dma_overlays(rom);
    for (const auto& overlay : dma_overlays) {
        gbrecomp::BankId bank = static_cast<gbrecomp::BankId>(overlay.rom_addr >> 16);
        uint16_t offset = static_cast<uint16_t>(overlay.rom_addr & 0xFFFF);
        size_t rom_idx = (offset < 0x4000)
            ? offset
            : static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);

        std::ostringstream ss;
        ss << "[" << output_prefix << "] Detected OAM DMA routine at ROM 0x"
           << std::hex << rom_idx
           << " (Bank " << (int)bank << ":0x" << offset << ", size "
           << std::dec << overlay.size << "). Mapping to HRAM "
           << std::hex << std::showbase << overlay.ram_addr
           << std::noshowbase << std::dec << ".";
        log_multi_rom_line(ss.str());
        analyze_opts.ram_overlays.push_back(overlay);
    }

    auto analysis = gbrecomp::analyze(rom, analyze_opts);
    if (symbol_table.size() > 0) {
        gbrecomp::apply_symbols_to_analysis(symbol_table, analysis);
    }

    {
        std::ostringstream ss;
        ss << "[" << output_prefix << "] Found "
           << analysis.stats.total_functions << " functions, "
           << analysis.stats.total_blocks << " basic blocks";
        log_multi_rom_line(ss.str());
    }

    gbrecomp::ir::BuilderOptions ir_opts;
    ir_opts.emit_comments = options.emit_comments;

    gbrecomp::ir::IRBuilder builder(ir_opts);
    auto ir_program = builder.build(
        analysis, output_prefix.empty() ? rom.name() : output_prefix);

    gbrecomp::codegen::GeneratorOptions gen_opts;
    gen_opts.output_prefix = output_prefix;
    gen_opts.output_dir = output_dir.string();
    gen_opts.emit_comments = options.emit_comments;
    gen_opts.single_function_mode = options.single_function;
    gen_opts.use_prefixed_symbols = true;
    gen_opts.emit_main_entry_point = false;
    gen_opts.emit_cmake = false;
    gen_opts.parallel_codegen_jobs = options.codegen_jobs;
    append_codegen_ram_overlays(rom, analyze_opts.ram_overlays, gen_opts);

    auto output = gbrecomp::codegen::generate_output(
        ir_program, rom.data(), rom.size(), gen_opts);

    if (!gbrecomp::codegen::write_output(output, output_dir.string())) {
        std::cerr << "Error: Failed to write output files for " << rom_path << "\n";
        return false;
    }

    module.output_prefix = output_prefix;
    module.display_name = display_name;
    module.source_rom = rom_path.lexically_normal().string();
    module.output = std::move(output);
    return true;
}

static fs::path resolve_meta_dir(const char* argv0, const fs::path& explicit_dir) {
    std::vector<fs::path> candidates;
    if (!explicit_dir.empty()) candidates.push_back(explicit_dir);
    if (const char* env = std::getenv("GBRECOMP_META_DIR")) {
        if (*env) candidates.emplace_back(env);
    }
    std::error_code error;
    const fs::path executable = fs::absolute(argv0, error);
    if (!error) {
        candidates.push_back(executable.parent_path() / "targets" / "meta");
        candidates.push_back(executable.parent_path().parent_path().parent_path() / "targets" / "meta");
    }
    candidates.push_back(fs::current_path() / "targets" / "meta");
#ifdef GBRECOMP_DEFAULT_META_DIR
    candidates.emplace_back(GBRECOMP_DEFAULT_META_DIR);
#endif
    for (const fs::path& candidate : candidates) {
        error.clear();
        if (fs::is_regular_file(candidate / "runtime" / "gbl.h", error) &&
            fs::is_regular_file(candidate / "platform" / "gb_main.cpp", error)) {
            return fs::weakly_canonical(candidate, error);
        }
    }
    return {};
}

/* --meta-code-budget: flash bytes for generated code (0 = derived from the
 * ROM size, see MetaTargetOptions::code_budget_bytes). */
static size_t g_meta_code_budget = 0;
/* Vectrex: the system ROM a console firmware embeds, the BIOS intro kept */
static fs::path g_vectrex_bios;
static bool g_vectrex_full_boot = false;
static long g_meta_sync_cycles = -1; /* SG/GG fast mode; -1: the emitter's default */
static int g_meta_stream = -1;                  /* --meta-stream auto|on|off */
static std::vector<fs::path> g_rom_profiles;    /* --rom-profile FILE */
static fs::path g_rom_plan;                     /* --rom-plan FILE (Game Boy too) */
static size_t g_meta_resident = 0;              /* --meta-rom-resident-kib */
static bool g_meta_pack = false;                /* --meta-pack */
static std::string g_sd_name;                   /* --sd-name */

/* Refuse a (system, target) pair: when the hardware cannot run the runtime,
 * say so with the numbers; otherwise name the missing pipeline. */
static int refuse_target(const m0boy::SystemDescriptor& source, m0boy::TargetId id,
                         const std::string& target) {
    const auto& facts = m0boy::target_descriptor(id);
    const std::string misfit = m0boy::target_misfit_reason(source, facts);
    if (!misfit.empty()) {
        std::cerr << "Error: --target " << target << " cannot run " << source.name
                  << ": " << misfit << "\n";
        return 1;
    }
    std::cerr << "Error: " << source.name << " code generation is not available for --target "
              << target << "\n";
    return 1;
}

static int run_meta_target(const char* argv0, const std::string& rom_path,
                           const std::string& output_dir, const std::string& output_prefix,
                           const fs::path& explicit_meta_dir,
                           const std::vector<fs::path>& coverage_paths, bool precise,
                           bool static_coverage, size_t bytes_per_instruction,
                           bool emit_comments, m0boy::TargetId target_id, bool thumb, const std::string& compiler,
                           bool sweep) {
    gbrecomp::MetaTargetOptions options;
    options.sweep = sweep;
    options.pokitto = target_id == m0boy::TargetId::pokitto;
    options.espboy = target_id == m0boy::TargetId::espboy;
    options.pc = target_id == m0boy::TargetId::pc;
    options.odroid = target_id == m0boy::TargetId::odroid;
    options.aka = target_id == m0boy::TargetId::aka;
    options.template_dir = resolve_meta_dir(argv0, explicit_meta_dir);
    if (options.template_dir.empty()) {
        std::cerr << "Error: could not locate targets/meta. Use --meta-dir or GBRECOMP_META_DIR.\n";
        return 1;
    }
    auto rom = gbrecomp::ROM::load(rom_path);
    if (!rom || !rom->is_valid()) {
        std::cerr << "Error: cannot load ROM " << rom_path;
        if (rom) std::cerr << ": " << rom->error();
        std::cerr << "\n";
        return 1;
    }
    options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string())
                                         : output_prefix;
    options.sd_name = g_sd_name;
    options.output_dir = output_dir.empty()
                             ? fs::path(fs::path(rom_path).stem().string() + ("_" + std::string(m0boy::target_descriptor(target_id).name)))
                             : fs::path(output_dir);
    options.coverage_files = coverage_paths;
    options.bytes_per_instruction = bytes_per_instruction;
    options.code_budget_bytes = g_meta_code_budget;
    options.stream = g_meta_stream;
    options.rom_profile_files = g_rom_profiles;
    options.rom_plan = g_rom_plan;
    options.rom_resident_bytes = g_meta_resident;
    options.pack = g_meta_pack;
    if (static_coverage) {
        /* Everything the control-flow analysis reaches, without the
         * aggressive data scan: a play-free profile that the emitter merges
         * with any recorded sessions. */
        gbrecomp::AnalyzerOptions analyze_opts;
        analyze_opts.aggressive_scan = false;
        for (const auto& overlay : detect_oam_dma_overlays(*rom))
            analyze_opts.ram_overlays.push_back(overlay);
        auto analysis = gbrecomp::analyze(*rom, analyze_opts);
        std::set<uint32_t> entries;
        for (const auto& [key, block] : analysis.blocks) entries.insert(key);
        for (uint32_t key : analysis.label_addresses) entries.insert(key);
        for (uint32_t key : analysis.call_targets) entries.insert(key);
        std::error_code ec;
        fs::create_directories(options.output_dir, ec);
        const fs::path static_path = options.output_dir / "static-analysis.cov";
        std::ofstream cov(static_path);
        if (!cov) {
            std::cerr << "Error: cannot write " << static_path.string() << "\n";
            return 1;
        }
        cov << "# gbl coverage v1 from static analysis\n";
        size_t written = 0;
        for (const auto& ins : analysis.instructions) {
            if (ins.address >= 0x8000 || ins.bank == gbrecomp::UNKNOWN_BANK) continue;
            const unsigned bank = ins.address < 0x4000 ? 0u : ins.bank;
            const uint32_t key = (static_cast<uint32_t>(ins.bank) << 16) | ins.address;
            const unsigned flags = 5u | (entries.count(key) ? 2u : 0u); /* 4: speculative */
            char line[32];
            std::snprintf(line, sizeof(line), "%02x %04x %x\n", bank, ins.address, flags);
            cov << line;
            written++;
        }
        cov.close();
        std::cout << std::dec << "Static analysis: " << written << " instructions\n";
        options.coverage_files.push_back(static_path);
    }
    options.compiler = compiler;
    options.thumb = thumb;
    options.precise = precise;
    options.emit_comments = emit_comments;

    gbrecomp::MetaTargetStats stats;
    std::string error;
    if (!gbrecomp::generate_meta_project(*rom, options, stats, error)) {
        std::cerr << "Error: " << error << "\n";
        return 1;
    }
    if (coverage_paths.empty() && !static_coverage) {
        std::cout << "No --coverage profile: the project runs everything in the gbl interpreter.\n"
                     "Record a profile with host/build/" << options.name << "_profile --interp --coverage <file>\n"
                     "and regenerate with --coverage <file> (see targets/meta/README.md).\n";
    }
    const size_t cart_ram = rom->header().ram_size_bytes;
    if (cart_ram > 4096 && options.espboy) {
        std::cout << "Warning: " << cart_ram << " bytes of cartridge RAM are allocated in the "
                  << "ESPboy's 80 KiB; check the PlatformIO RAM report.\n";
    } else if (cart_ram > 4096 && (options.pokitto || options.espboy == false && options.pc == false &&
                                   options.odroid == false && options.aka == false)) {
        std::cout << "Warning: " << cart_ram << " bytes of cartridge RAM are allocated in the "
                  << (options.pokitto ? "Pokitto's 32 KiB; check the link map.\n"
                                      : "META's 32 KiB; check the PlatformIO RAM report.\n");
    }
    std::cout << std::dec;
    std::cout << "Flash budget: about " << stats.budget_instructions << " compiled instructions";
    if (stats.speculative)
        std::cout << " (static analysis contributes " << stats.speculative_kept << " of "
                  << stats.speculative << ")";
    std::cout << "\n";
    std::cout << "gbl project written to " << options.output_dir.string() << "\n"
              << "  coverage records:   " << stats.covered << "\n"
              << "  compiled:           " << stats.compiled << " instructions in "
              << stats.regions << " regions\n"
              << "  interpreter only:   " << stats.dropped << "\n"
              << (stats.data_skipped ? "  proven data:        " + std::to_string(stats.data_skipped) +
                                             " coverage records left as data (cov flag 8)\n"
                                     : "")
              << "  dispatch entries:   " << stats.dispatch_entries << "\n"
              << (thumb ? "  register IR:        " + std::to_string(stats.register_blocks) +
                          " blocks, " + std::to_string(stats.thumb_blocks) + " Thumb blocks\n" : "")
              << "  wait loops:         " << stats.spin_loops << " polling, " << stats.counted_loops
              << " delay (fast-forwarded)\n"
              << (stats.stream && stats.sd
                      ? "  ROM streaming:      " + std::to_string(stats.resident_chunks * 4) +
                            " KiB of banks 1+ in flash, " + std::to_string(stats.streamed_chunks * 4) +
                            " KiB read from the SD card as " + stats.stream_file + "\n"
                      : std::string())
              << (stats.packed
                      ? "  ROM packed:         " + std::to_string(stats.resident_chunks * 4) +
                            " KiB of banks 1+ raw, " + std::to_string(stats.packed_raw_bytes / 1024) +
                            " KiB LZSS-packed in flash into " + std::to_string((stats.packed_bytes + 1023) / 1024) +
                            " KiB\n"
                      : std::string())
              << (!stats.pack_note.empty() ? "  --meta-pack:        not applied, " + stats.pack_note + "\n"
                                           : std::string())
              << (precise ? "  mode:               precise (interrupt poll per instruction)\n" : "")
              << (options.pc ? std::string("Build firmware:  cmake -B ") + (options.output_dir / "build").string() +
                                  " -S " + options.output_dir.string() + " && cmake --build " +
                                  (options.output_dir / "build").string() + "\n"
                     : options.pokitto ? "Build firmware:  make -C " + options.output_dir.string() + "\n"
                                       : "Build firmware:  pio run -d " + options.output_dir.string() + "\n")
              << "Build host test: cmake -G Ninja -S " << (options.output_dir / "host").string()
              << " -B " << (options.output_dir / "host" / "build").string() << "\n";
    return 0;
}

int main(int argc, char* argv[]) {
    // Parse command line
    if (argc < 2) {
        print_banner();
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << GBRECOMP_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--meta-version") {
        std::cout << GBRECOMP_META_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--espboy-version") {
        std::cout << GBRECOMP_ESPBOY_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--pokitto-version") {
        std::cout << GBRECOMP_POKITTO_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--pc-version") {
        std::cout << GBRECOMP_PC_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--odroid-version") {
        std::cout << GBRECOMP_ODROID_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--aka-version") {
        std::cout << GBRECOMP_AKA_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version-json") {
        print_version_json();
        return 0;
    }
    
    std::string rom_path;
    std::string output_dir;
    std::string native_patch_manifest_path;
    std::string output_prefix;
    fs::path progress_json_path;
    fs::path explicit_runtime_dir;
    bool disasm_only = false;
    bool analyze_only = false;
    bool verbose = false;
    bool trace_log = false;
    size_t limit_instructions = 0;
    size_t requested_jobs = 0;
    bool single_function = false;
    bool emit_comments = false;
    bool aggressive_scan = true;
    bool infer_data_zones = true;
    bool analyze_all_banks = true;
    int specific_bank = -1;
    std::vector<uint32_t> manual_entry_points;
    std::string trace_file_path;
    std::string symbol_file_path;
    gbrecomp::SymbolAnalysisPolicy symbol_analysis_policy =
        gbrecomp::SymbolAnalysisPolicy::INFER_BOUNDARIES;
    std::string annotation_file_path;
    bool emit_android_project = false;
    std::string android_package;
    std::string android_app_name;
    std::string system_name = "auto";
    std::optional<m0boy::CpuId> requested_cpu;
    bool list_systems = false;
    bool list_targets = false;
    bool info_only = false;
    std::string target = "desktop";
    std::vector<fs::path> coverage_paths;
    std::string meta_compiler = "gcc";
    bool meta_thumb = false;
    bool meta_precise = false;
    bool nes_interp = false;
    bool pce_sweep = false;
    bool nes_sweep = false;
    bool vectrex_sweep = false;
    bool megadrive_sweep = false;
    bool meta_sweep = false;
    bool meta_static = false;
    size_t meta_bpi = 20;
    bool meta_bpi_set = false;
    size_t meta_fill = 0;
    fs::path rom_plan;
    fs::path meta_dir;
    std::string msx_mapper_name;
    bool msx_pack = false;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            }
        } else if (arg == "--runtime-dir") {
            if (i + 1 < argc) {
                explicit_runtime_dir = argv[++i];
            }
        } else if (arg == "--native-patch") {
            if (i + 1 < argc) {
                native_patch_manifest_path = argv[++i];
            }
        } else if (arg == "--sd-name") {
            if (i + 1 < argc) {
                g_sd_name = argv[++i];
            }
        } else if (arg == "--output-prefix") {
            if (i + 1 < argc) {
                output_prefix = argv[++i];
            }
        } else if (arg == "--progress-json") {
            if (i + 1 < argc) {
                progress_json_path = argv[++i];
            }
        } else if (arg == "-d" || arg == "--disasm") {
            disasm_only = true;
        } else if (arg == "-a" || arg == "--analyze") {
            analyze_only = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--trace") {
            trace_log = true;
        } else if (arg == "--limit") {
            if (i + 1 < argc) {
                limit_instructions = std::stoul(argv[++i]);
            }
        } else if (arg == "-j" || arg == "--jobs") {
            if (i + 1 < argc) {
                requested_jobs = std::stoul(argv[++i]);
            }
        } else if (arg == "--android") {
            emit_android_project = true;
        } else if (arg == "--android-package") {
            if (i + 1 < argc) {
                android_package = argv[++i];
            }
        } else if (arg == "--android-app-name") {
            if (i + 1 < argc) {
                android_app_name = argv[++i];
            }
        } else if (arg == "--single-function") {
            single_function = true;
        } else if (arg == "--comments") {
            emit_comments = true;
        } else if (arg == "--bank") {
            if (i + 1 < argc) {
                specific_bank = std::stoi(argv[++i]);
            }
        } else if (arg == "--add-entry-point") {
            if (i + 1 < argc) {
                std::string param = argv[++i];
                size_t colon = param.find(':');
                if (colon != std::string::npos) {
                    try {
                        int bank = std::stoi(param.substr(0, colon), nullptr, 0);
                        int addr = std::stoi(param.substr(colon + 1), nullptr, 16);
                        manual_entry_points.push_back(gbrecomp::AnalysisResult::make_addr(bank, addr));
                        std::cout << "Added manual entry point: Bank " << bank << " Address 0x" << std::hex << addr << std::dec << "\n";
                    } catch (...) {
                        std::cerr << "Invalid entry point format: " << param << "\n";
                    }
                } else {
                     std::cerr << "Invalid entry point format (expected bank:addr): " << param << "\n";
                }
            }
        } else if (arg == "--no-scan") {
            aggressive_scan = false;
        } else if (arg == "--no-infer-data") {
            infer_data_zones = false;
        } else if (arg == "--reachable-only") {
            analyze_all_banks = false;
        } else if (arg == "--use-trace") {
            if (i + 1 < argc) {
                trace_file_path = argv[++i];
            }
        } else if (arg == "--symbols") {
            if (i + 1 < argc) {
                symbol_file_path = argv[++i];
            }
        } else if (arg == "--symbol-policy") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --symbol-policy requires a value\n";
                return 1;
            }
            const std::string policy = argv[++i];
            if (policy == "names-only") {
                symbol_analysis_policy =
                    gbrecomp::SymbolAnalysisPolicy::NAMES_ONLY;
            } else if (policy == "infer-boundaries") {
                symbol_analysis_policy =
                    gbrecomp::SymbolAnalysisPolicy::INFER_BOUNDARIES;
            } else {
                std::cerr << "Error: unknown symbol policy '" << policy
                          << "' (expected infer-boundaries or names-only)\n";
                return 1;
            }
        } else if (arg == "--annotations") {
            if (i + 1 < argc) {
                annotation_file_path = argv[++i];
            }
        } else if (arg == "--system" || arg == "--cpu") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a value\n";
                return 1;
            }
            const std::string value = argv[++i];
            if (arg == "--system") {
                if (value != "auto" && !m0boy::parse_system(value)) {
                    std::cerr << "Error: unknown --system '" << value << "'\n";
                    return 1;
                }
                system_name = value;
            } else {
                requested_cpu = m0boy::parse_cpu(value);
                if (!requested_cpu) {
                    std::cerr << "Error: unknown --cpu '" << value << "'\n";
                    return 1;
                }
            }
        } else if (arg == "--list-systems") {
            list_systems = true;
        } else if (arg == "--list-targets") {
            list_targets = true;
        } else if (arg == "--info") {
            info_only = true;
        } else if (arg == "--target") {
            if (i + 1 < argc) {
                target = argv[++i];
            }
        } else if (arg == "--coverage") {
            if (i + 1 < argc) {
                coverage_paths.emplace_back(argv[++i]);
            }
        } else if (arg == "--meta-compiler") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --meta-compiler requires gcc or clang\n";
                return 1;
            }
            meta_compiler = argv[++i];
            if (meta_compiler != "gcc" && meta_compiler != "clang") {
                std::cerr << "Error: --meta-compiler requires gcc or clang\n";
                return 1;
            }
        } else if (arg == "--meta-thumb") {
            meta_thumb = true;
        } else if (arg == "--meta-precise") {
            meta_precise = true;
        } else if (arg == "--nes-interp" || arg == "--interp") {
            nes_interp = true;
        } else if (arg == "--pce-sweep") {
            pce_sweep = true;
        } else if (arg == "--nes-sweep") {
            nes_sweep = true;
        } else if (arg == "--vectrex-sweep") {
            vectrex_sweep = true;
        } else if (arg == "--megadrive-sweep") {
            megadrive_sweep = true;
        } else if (arg == "--meta-sweep") {
            meta_sweep = true;
        } else if (arg == "--bios") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --bios requires a file\n";
                return 1;
            }
            g_vectrex_bios = argv[++i];
        } else if (arg == "--full-boot") {
            g_vectrex_full_boot = true;
        } else if (arg == "--meta-static") {
            meta_static = true;
        } else if (arg == "--meta-bytes-per-instruction") {
            if (i + 1 < argc) {
                meta_bpi = std::stoul(argv[++i]);
                meta_bpi_set = true;
            }
        } else if (arg == "--meta-fill-bytes") {
            if (i + 1 < argc) meta_fill = std::stoul(argv[++i]);
        } else if (arg == "--rom-plan") {
            if (i + 1 < argc) g_rom_plan = rom_plan = argv[++i];
        } else if (arg == "--meta-pack") {
            g_meta_pack = true;
        } else if (arg == "--meta-stream") {
            if (i + 1 < argc) {
                const std::string v = argv[++i];
                g_meta_stream = v == "on" ? 1 : v == "off" ? 0 : -1;
            }
        } else if (arg == "--rom-profile") {
            if (i + 1 < argc) g_rom_profiles.emplace_back(argv[++i]);
        } else if (arg == "--meta-rom-resident-kib") {
            if (i + 1 < argc) g_meta_resident = std::stoul(argv[++i]) * 1024u;
        } else if (arg == "--meta-code-budget") {
            if (i + 1 < argc) {
                g_meta_code_budget = std::stoul(argv[++i]);
            }
        } else if (arg == "--meta-sync-cycles") {
            if (i + 1 < argc) g_meta_sync_cycles = std::stol(argv[++i]);
        } else if (arg == "--msx-pack") {
            msx_pack = true;
        } else if (arg == "--msx-mapper") {
            if (i + 1 < argc) msx_mapper_name = argv[++i];
        } else if (arg == "--meta-dir") {
            if (i + 1 < argc) {
                meta_dir = argv[++i];
            }
        } else if (arg[0] != '-') {
            rom_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }
    
    if (list_systems) {
        for (const auto& system : m0boy::systems()) {
            std::cout << system.name << " cpu=" << m0boy::cpu_descriptor(system.cpu).name
                      << " inspect=yes disasm="
                      << (m0boy::cpu_descriptor(system.cpu).decoder_available ? "yes" : "no")
                      << " codegen=";
            bool any = false;
            for (const auto& facts : m0boy::targets()) {
                if (!system.supports(facts.id)) continue;
                std::cout << (any ? "," : "") << facts.name;
                any = true;
            }
            if (!any) std::cout << "unavailable";
            std::cout << "\n";
        }
        return 0;
    }
    if (list_targets) {
        for (const auto& facts : m0boy::targets()) {
            std::cout << facts.name
                      << (facts.host ? " host=yes" : " host=no")
                      << " ram=" << facts.ram_bytes / 1024 << "K"
                      << " psram=" << facts.psram_bytes / 1024 << "K"
                      << " flash=" << facts.flash_app_bytes / 1024 << "K"
                      << " screen=" << facts.screen_cols << "x" << facts.screen_rows
                      << " sd=" << (facts.has_sd ? "yes" : "no")
                      << " \"" << facts.description << "\"\n";
        }
        return 0;
    }
    if (rom_path.empty()) {
        std::cerr << "Error: No ROM file specified\n";
        print_usage(argv[0]);
        return 1;
    }
    if (!m0boy::parse_target(target)) {
        std::cerr << "Error: unknown --target '" << target << "' (expected";
        for (const auto& facts : m0boy::targets()) std::cerr << " " << facts.name;
        std::cerr << ")\n";
        return 1;
    }
    // Resolve the source before invoking any Game Boy parser or emitter.
    // Unknown extensions and directory batches keep the historical GB default.
    const auto system_id = system_name == "auto"
        ? m0boy::detect_system(rom_path).value_or(m0boy::SystemId::gameboy)
        : *m0boy::parse_system(system_name);
    const auto& source = m0boy::system_descriptor(system_id);
    if (requested_cpu && *requested_cpu != source.cpu) {
        std::cerr << "Error: source system " << source.name << " requires CPU "
                  << m0boy::cpu_descriptor(source.cpu).name << "\n";
        return 1;
    }
    if (info_only) {
        if (disasm_only || analyze_only) {
            std::cerr << "Error: --info cannot be combined with --disasm or --analyze\n";
            return 1;
        }
        std::string error;
        const auto cartridge = source.inspect(rom_path, error);
        if (!cartridge) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "system=" << source.name
                  << "\ncpu=" << m0boy::cpu_descriptor(source.cpu).name
                  << "\ntitle=" << cartridge->title
                  << "\nrom_bytes=" << cartridge->rom_bytes
                  << "\nwork_ram_bytes=" << source.work_ram_bytes
                  << "\ncartridge_ram_bytes=" << cartridge->cartridge_ram_bytes
                  << "\nmapper=" << cartridge->mapper << "\n";
        return 0;
    }
    if (source.cpu == m0boy::CpuId::m0plus) {
        std::string error;
        if (!source.inspect(rom_path, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        if (disasm_only && !analyze_only) {
            if (specific_bank > 0) {
                std::cerr << "Error: M0+ firmware images have no banks\n";
                return 1;
            }
            std::ifstream file(rom_path, std::ios::binary);
            const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{file}, {});
            const uint32_t base = source.id == m0boy::SystemId::meta ? 0x4000u : 0u;
            const uint32_t reset = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) |
                                   (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
            size_t offset = (reset & ~1u) - base, count = 0;
            while (offset < bytes.size() && (!limit_instructions || count < limit_instructions)) {
                try {
                    const auto instruction = m0boy::cpu::m0plus::decode(
                        std::span<const uint8_t>(bytes).subspan(offset), base + uint32_t(offset));
                    std::cout << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                              << base + offset << "  " << instruction.mnemonic << std::dec << "\n";
                    offset += instruction.length;
                    ++count;
                } catch (const std::runtime_error& e) {
                    std::cerr << "Error: " << e.what() << " at file offset " << offset << "\n";
                    return 1;
                }
            }
            return 0;
        }
        if (analyze_only) {
            std::cerr << "Error: M0+ firmware analysis is not available\n";
            return 1;
        }
        const bool aka = target == "aka" && source.id == m0boy::SystemId::meta;
        if (target != "pc" && !aka) return refuse_target(source, *m0boy::parse_target(target), target);
        const fs::path output = output_dir.empty()
            ? fs::path(fs::path(rom_path).stem().string() + "_" + std::string(source.name) + "_" + target)
            : fs::path(output_dir);
        const fs::path runtime = resolve_meta_dir(argv[0], meta_dir).parent_path() / "m0plus/runtime";
        if (!m0boy::m0plus::generate(rom_path, source.id, output, error, runtime, coverage_paths, aka)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << (aka ? "AKA" : "PC") << " firmware project written to " << output.string() << "\n";
        return 0;
    }
    if (disasm_only && source.id == m0boy::SystemId::sg1000 && !analyze_only) {
        if (specific_bank > 0) {
            std::cerr << "Error: SG-1000 unbanked cartridges only have bank 0\n";
            return 1;
        }
        std::string error;
        auto cartridge = m0boy::sg1000::Cartridge::load(rom_path, error);
        if (!cartridge) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        m0boy::cpu::z80::Decoder decoder(*cartridge);
        size_t offset = 0, count = 0;
        while (offset < cartridge->bytes().size() &&
               (!limit_instructions || count < limit_instructions)) {
            const auto instruction = decoder.decode(static_cast<uint16_t>(offset));
            if (instruction.length > cartridge->bytes().size() - offset) {
                std::cerr << "Error: truncated Z80 instruction at file offset " << offset << "\n";
                return 1;
            }
            std::cout << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                      << offset << "  " << instruction.mnemonic << std::dec << "\n";
            offset += instruction.length;
            ++count;
        }
        return 0;
    }
    if (disasm_only && source.id == m0boy::SystemId::nes && !analyze_only) {
        /* the iNES header and the trainer are not code: the PRG banks
         * follow, mapped from $8000 (the vectors close the last one) */
        std::ifstream file(rom_path, std::ios::binary);
        uint8_t header[16];
        if (!file.read(reinterpret_cast<char*>(header), 16)) {
            std::cerr << "Error: truncated iNES header: " << rom_path << "\n";
            return 1;
        }
        file.seekg((header[6] & 0x04) ? 512 : 0, std::ios::cur);
        std::vector<uint8_t> prg(size_t(header[4]) * 16384);
        if (prg.empty() || !file.read(reinterpret_cast<char*>(prg.data()), std::streamsize(prg.size()))) {
            std::cerr << "Error: truncated PRG banks: " << rom_path << "\n";
            return 1;
        }
        class PrgReader : public m0boy::ByteReader {
        public:
            const std::vector<uint8_t>* bytes;
            uint8_t read_banked(m0boy::BankId, uint16_t address) const override {
                const size_t at = address - 0x8000u;
                return at < bytes->size() ? (*bytes)[at] : uint8_t(0xFF);
            }
        } reader;
        reader.bytes = &prg;
        m0boy::cpu::m6502::Decoder decoder(reader);
        size_t offset = 0, count = 0;
        while (offset < prg.size() && (!limit_instructions || count < limit_instructions)) {
            const auto instruction = decoder.decode(uint16_t(0x8000u + offset));
            std::cout << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                      << (0x8000u + offset) << "  " << instruction.mnemonic;
            if (instruction.target) std::cout << " $" << std::setw(4) << *instruction.target;
            std::cout << std::dec << "\n";
            offset += instruction.length;
            ++count;
        }
        return 0;
    }
    if (disasm_only && source.id == m0boy::SystemId::pce && !analyze_only) {
        /* a raw HuCard image seen linearly: the socle's view of the first
         * bank, the vectors at the end of the address space come later */
        std::ifstream file(rom_path, std::ios::binary);
        std::vector<uint8_t> rom(std::min<size_t>(size_t(0x10000),
            std::filesystem::file_size(rom_path) - (std::filesystem::file_size(rom_path) % 0x20000 == 128 ? 128 : 0)));
        if (rom.empty() || !file.read(reinterpret_cast<char*>(rom.data()), std::streamsize(rom.size()))) {
            std::cerr << "Error: cannot read " << rom_path << "\n";
            return 1;
        }
        class RawReader : public m0boy::ByteReader {
        public:
            const std::vector<uint8_t>* bytes;
            uint8_t read_banked(m0boy::BankId, uint16_t address) const override {
                return address < bytes->size() ? (*bytes)[address] : uint8_t(0xFF);
            }
        } reader;
        reader.bytes = &rom;
        m0boy::cpu::huc6280::Decoder decoder(reader);
        size_t offset = 0, count = 0;
        while (offset < rom.size() && (!limit_instructions || count < limit_instructions)) {
            const auto instruction = decoder.decode(uint16_t(offset));
            std::cout << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                      << offset << "  " << instruction.mnemonic;
            if (instruction.target) std::cout << " $" << std::setw(4) << *instruction.target;
            std::cout << std::dec << "\n";
            offset += instruction.length;
            ++count;
        }
        return 0;
    }
    /* --sd-name: an 8.3 base, upper case (the FAT keeps short names so). */
    if (!g_sd_name.empty()) {
        for (char& c : g_sd_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool ok = g_sd_name.size() <= 8;
        for (char c : g_sd_name) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
        if (!ok) {
            std::cerr << "Error: --sd-name must be 1 to 8 letters, digits, '_' or '-'\n";
            return 1;
        }
    }
    if (source.id == m0boy::SystemId::megadrive) {
        if (!source.supports(*m0boy::parse_target(target)))
            return refuse_target(source, *m0boy::parse_target(target), target);
        if (disasm_only || analyze_only || (target != "pc" && target != "aka" && !coverage_paths.empty()) ||
            meta_thumb || g_meta_pack || g_meta_stream != -1 ||
            !g_rom_profiles.empty() || g_meta_resident || emit_android_project ||
            !native_patch_manifest_path.empty() || specific_bank > 0) {
            std::cerr << "Error: unsupported Mega Drive profile option; use --target, --coverage (PC), --megadrive-sweep and --meta-compiler\n";
            return 1;
        }
        m0boy::megadrive::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid Mega Drive output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_megadrive_" + target) : fs::path(output_dir);
        options.targets_dir = resolve_meta_dir(argv[0], meta_dir).parent_path();
        options.compiler = meta_compiler;
        options.coverage = coverage_paths;
        options.sweep = megadrive_sweep;
        std::string error;
        if (!m0boy::megadrive::generate(rom_path, options, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        const bool native = [&] {
            std::ifstream status(options.output / "native-status.txt");
            std::string line;
            return bool(std::getline(status, line)) && (line == "native" || line == "native-c");
        }();
        std::cout << "Generated Mega Drive project for " << target << ": " << options.output
                  << "\n  mode:               "
                  << (target == "pc" ? "covered 68000 opcodes emitted as C, in-tree fallback" :
                      native ? "instruction-precise native profile" : "unsupported")
                  << (native && meta_precise ? " (--meta-precise)" : "") << "\n";
        return 0;
    }
    if (source.id == m0boy::SystemId::pico8) {
        if (disasm_only || analyze_only || meta_thumb || g_meta_pack || !coverage_paths.empty() || meta_precise ||
            emit_android_project || !native_patch_manifest_path.empty() || specific_bank > 0) {
            std::cerr << "Error: unsupported PICO-8 option; a cartridge is compiled to C, use --target and --meta-compiler\n";
            return 1;
        }
        if (!source.supports(*m0boy::parse_target(target))) {
            return refuse_target(source, *m0boy::parse_target(target), target);
        }
        m0boy::pico8::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid PICO-8 output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_pico8_" + target) : fs::path(output_dir);
        options.meta_templates = resolve_meta_dir(argv[0], meta_dir);
        options.compiler = meta_compiler;
        options.sd_name = g_sd_name;
        std::string error;
        if (!m0boy::pico8::generate(rom_path, options, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated PICO-8 project for " << target << ": " << options.output << "\n";
        return 0;
    }
    if (source.id == m0boy::SystemId::sg1000 || source.id == m0boy::SystemId::gamegear ||
        source.id == m0boy::SystemId::mastersystem || source.id == m0boy::SystemId::msx) {
        const bool msx = source.id == m0boy::SystemId::msx;
        const bool mastersystem = source.id == m0boy::SystemId::mastersystem;
        const bool gamegear = source.id == m0boy::SystemId::gamegear || mastersystem;
        const char* system_label = msx ? "MSX" : mastersystem ? "Master System" : gamegear ? "Game Gear" : "SG-1000";
        if (!source.supports(*m0boy::parse_target(target))) {
            return refuse_target(source, *m0boy::parse_target(target), target);
        }
        if (analyze_only || meta_thumb || g_meta_pack ||
            g_meta_stream != -1 || !g_rom_profiles.empty() || g_meta_resident ||
            emit_android_project || !native_patch_manifest_path.empty() ||
            !symbol_file_path.empty() || !annotation_file_path.empty() ||
            specific_bank > 0 || !manual_entry_points.empty() || !trace_file_path.empty()) {
            std::cerr << "Error: unsupported " << system_label << " option; use --coverage, --target, --meta-precise, --meta-compiler, --meta-code-budget and --meta-bytes-per-instruction\n";
            return 1;
        }
        m0boy::sg1000::GenerateOptions options;
        options.gamegear = gamegear;
        options.mastersystem = mastersystem;
        options.msx = msx;
        options.msx_pack = msx_pack && msx;
        if (!msx_mapper_name.empty()) {
            int found = -1;
            for (int m = 0; m < 5; m++) if (msx_mapper_name == m0_msx_mapper_name(static_cast<unsigned>(m))) found = m;
            if (found < 0 || !msx) {
                std::cerr << "Error: --msx-mapper takes plain, ascii8, ascii16, konami or konami-scc, with a MSX cartridge\n";
                return 1;
            }
            options.msx_mapper = found;
        }
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid " << system_label << " output prefix\n";
            return 1;
        }
        options.sd_name = g_sd_name;
        options.output = output_dir.empty() ? fs::path(options.name + (msx ? "_msx_" : mastersystem ? "_sms_" : gamegear ? "_gg_" : "_sg1000_") + target) : fs::path(output_dir);
        options.meta_templates = resolve_meta_dir(argv[0], meta_dir);
        options.coverage = coverage_paths;
        options.code_budget_bytes = g_meta_code_budget;
        if (meta_bpi_set) options.bytes_per_instruction = meta_bpi;
        options.fill_bytes = meta_fill;
        options.rom_plan = rom_plan;
        options.precise = meta_precise;
        options.aggressive_scan = aggressive_scan;
        options.infer_data_zones = infer_data_zones;
        if (g_meta_sync_cycles >= 0) options.sync_cycles = static_cast<unsigned>(g_meta_sync_cycles);
        options.compiler = meta_compiler;
        std::string error;
        m0boy::sg1000::GenerateStats stats;
        if (!m0boy::sg1000::generate(rom_path, options, stats, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated " << system_label << " project for " << target << ": " << options.output << "\n"
                  << "  instructions:       " << stats.compiled << " compiled (" << stats.covered << " played, "
                  << stats.speculative << " reached statically, " << stats.swept << " swept)\n"
                  << "  static sweep:       " << stats.data_zones << " data zone(s), " << stats.data_bytes << " ROM byte(s)\n"
                  << "  regions:            " << stats.regions << ", " << stats.entries << " entries\n"
                  << (stats.cart_ram ? (gamegear ? (target == "desktop" ? "  cartridge RAM:      32 KiB, 16 KiB window at 8000-BFFF\n"
                                                                            : target == "espboy" ? "  cartridge RAM:      8 KiB in RAM, saved in flash\n"
                                                                            : "  cartridge RAM:      32 KiB in the save file on the SD card\n")
                                                 : "  cartridge RAM:      8 KiB at 8000-BFFF\n") : "")
                  << (stats.streamed_pages && !stats.packed ? "  ROM streaming:      " + std::to_string(stats.streamed_pages) + (target == "espboy" ? " pages of 16 KiB read from the flash (rom.bin)" : " pages of 16 KiB read from the SD card")
                                                 + (stats.pool_sectors ? ", " + std::to_string(stats.pool_sectors) + " hot sectors of them kept in flash" : std::string()) + "\n" : std::string())
                  << (stats.pool_sectors ? "  ROM flash pool:     " + std::to_string(stats.pool_sectors) + " sectors, " +
                       std::to_string(stats.packed_sectors) + " LZSS, " + std::to_string(stats.pool_bytes) + " bytes" +
                       (stats.packed ? " (whole ROM in flash)\n" : " (SD fallback raw)\n") : std::string())
                  << (stats.pool_sectors && !rom_plan.empty() ? "  ROM plan:           " + rom_plan.string() + " (simulated card cache)\n" : std::string())
                  << (meta_precise ? "  mode:               precise (sync per instruction)\n" : "");
        return 0;
    }
    /* PC Engine follows the NES interpreter pipeline's plan; its console
     * targets come once the picture's budget says where (see system.cpp). */
    if (source.id == m0boy::SystemId::nes) {
        if (!source.supports(*m0boy::parse_target(target))) {
            return refuse_target(source, *m0boy::parse_target(target), target);
        }
        m0boy::nes::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid NES output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_nes_" + target) : fs::path(output_dir);
        options.meta_templates = resolve_meta_dir(argv[0], meta_dir);
        options.compiler = meta_compiler;
        options.coverage = coverage_paths;
        options.precise = meta_precise;
        options.interpret = nes_interp;
        options.sweep = nes_sweep;
        options.code_budget_bytes = g_meta_code_budget;
        if (meta_bpi_set) options.bytes_per_instruction = meta_bpi;
        std::string error;
        m0boy::nes::GenerateStats stats;
        if (!m0boy::nes::generate(rom_path, options, stats, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated NES project for " << target << ": " << options.output << "\n";
        if (nes_interp)
            std::cout << "  mode:               interpreter only (--nes-interp)\n";
        else
            std::cout << "  instructions:       " << stats.compiled << " compiled (" << stats.covered << " played)\n"
                      << "  regions:            " << stats.regions << ", " << stats.entries << " entries\n"
                      << (nes_sweep ? "  sweep:              the PRG's JSR/JMP targets outside the walk are compiled too\n" : "")
                      << (meta_precise ? "  mode:               precise (budget checked after every instruction)\n" : "");
        return 0;
    }
    if (source.id == m0boy::SystemId::vectrex) {
        if (!source.supports(*m0boy::parse_target(target)))
            return refuse_target(source, *m0boy::parse_target(target), target);
        if (disasm_only || analyze_only || meta_thumb || g_meta_pack || g_meta_stream != -1 ||
            !g_rom_profiles.empty() || g_meta_resident || emit_android_project ||
            !native_patch_manifest_path.empty() || specific_bank > 0) {
            std::cerr << "Error: unsupported Vectrex option; use --target, --bios, --full-boot, --coverage, "
                         "--meta-precise, --nes-interp and --meta-compiler\n";
            return 1;
        }
        m0boy::vectrex::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid Vectrex output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_vectrex_" + target) : fs::path(output_dir);
        options.targets_dir = resolve_meta_dir(argv[0], meta_dir).parent_path();
        options.compiler = meta_compiler;
        options.bios = g_vectrex_bios;
        options.full_boot = g_vectrex_full_boot;
        options.coverage = coverage_paths;
        options.precise = meta_precise;
        options.interpret = nes_interp;
        options.sweep = vectrex_sweep;
        options.code_budget_bytes = g_meta_code_budget;
        if (meta_bpi_set) options.bytes_per_instruction = unsigned(meta_bpi);
        std::string error;
        m0boy::vectrex::GenerateStats stats;
        if (!m0boy::vectrex::generate(rom_path, options, stats, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated Vectrex project for " << target << ": " << options.output << "\n";
        if (nes_interp)
            std::cout << "  mode:               interpreter only (--nes-interp)\n";
        else
            std::cout << "  instructions:       " << stats.compiled << " compiled (" << stats.covered << " played)"
                      << (stats.bios_compiled ? ", system ROM included" : ", cartridge only") << "\n"
                      << "  regions:            " << stats.regions << ", " << stats.entries << " entries\n"
                      << (vectrex_sweep ? "  sweep:              the JSR/JMP extended targets outside the walk are compiled too\n" : "")
                      << (meta_precise ? "  mode:               precise (slice checked after every instruction)\n" : "");
        return 0;
    }
    if (source.id == m0boy::SystemId::pce) {
        if (!source.supports(*m0boy::parse_target(target))) {
            return refuse_target(source, *m0boy::parse_target(target), target);
        }
        if (analyze_only || meta_thumb || g_meta_pack ||
            g_meta_stream != -1 || !g_rom_profiles.empty() || g_meta_resident ||
            emit_android_project || !native_patch_manifest_path.empty() ||
            !symbol_file_path.empty() || !annotation_file_path.empty() ||
            specific_bank > 0 || !manual_entry_points.empty() || !trace_file_path.empty()) {
            std::cerr << "Error: unsupported PC Engine option; use --target, --coverage, --meta-precise, "
                         "--nes-interp, --pce-sweep, --meta-code-budget and --meta-bytes-per-instruction\n";
            return 1;
        }
        m0boy::pce::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid PC Engine output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_pce_" + target) : fs::path(output_dir);
        options.meta_templates = resolve_meta_dir(argv[0], meta_dir);
        options.compiler = meta_compiler;
        options.coverage = coverage_paths;
        options.precise = meta_precise;
        options.interpret = nes_interp;
        options.sweep = pce_sweep;
        options.code_budget_bytes = g_meta_code_budget;
        if (meta_bpi_set) options.bytes_per_instruction = meta_bpi;
        std::string error;
        m0boy::pce::GenerateStats stats;
        if (!m0boy::pce::generate(rom_path, options, stats, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated PC Engine project for " << target << ": " << options.output << "\n";
        if (nes_interp)
            std::cout << "  mode:               interpreter only (--nes-interp)\n";
        else
            std::cout << "  instructions:       " << stats.compiled << " compiled (" << stats.covered << " played)\n"
                      << "  regions:            " << stats.regions << ", " << stats.entries << " entries\n"
                      << (pce_sweep ? "  sweep:              the card's JSR/JMP targets outside the walk are compiled too\n" : "")
                      << (meta_precise ? "  mode:               precise (budget checked after every instruction)\n" : "");
        return 0;
    }
    /* SNES, phase 1: the 65C816's interpreter and recompiler on the
     * desktop harness, after the PCE's plan; the consoles wait on the
     * picture's budget (the PPU's 64 KiB VRAM, the APU's 64 KiB A-RAM). */
    if (source.id == m0boy::SystemId::snes) {
        if (!source.supports(*m0boy::parse_target(target))) {
            return refuse_target(source, *m0boy::parse_target(target), target);
        }
        if (analyze_only || meta_thumb || g_meta_pack ||
            g_meta_stream != -1 || !g_rom_profiles.empty() || g_meta_resident ||
            emit_android_project || !native_patch_manifest_path.empty() ||
            !symbol_file_path.empty() || !annotation_file_path.empty() ||
            specific_bank > 0 || !manual_entry_points.empty() || !trace_file_path.empty()) {
            std::cerr << "Error: unsupported SNES option; use --target, --coverage, --meta-precise, "
                         "--nes-interp, --meta-code-budget and --meta-bytes-per-instruction\n";
            return 1;
        }
        m0boy::snes::GenerateOptions options;
        options.target = *m0boy::parse_target(target);
        options.name = output_prefix.empty() ? sanitize_prefix(fs::path(rom_path).stem().string()) : output_prefix;
        if (options.name.empty() || sanitize_prefix(options.name) != options.name) {
            std::cerr << "Error: invalid SNES output prefix\n";
            return 1;
        }
        options.output = output_dir.empty() ? fs::path(options.name + "_snes_" + target) : fs::path(output_dir);
        options.meta_templates = resolve_meta_dir(argv[0], meta_dir);
        options.compiler = meta_compiler;
        options.coverage = coverage_paths;
        options.precise = meta_precise;
        options.interpret = nes_interp;
        options.code_budget_bytes = g_meta_code_budget;
        if (meta_bpi_set) options.bytes_per_instruction = meta_bpi;
        std::string error;
        m0boy::snes::GenerateStats stats;
        if (!m0boy::snes::generate(rom_path, options, stats, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Generated SNES project for " << target << ": " << options.output << "\n";
        if (nes_interp)
            std::cout << "  mode:               interpreter only (--nes-interp)\n";
        else
            std::cout << "  instructions:       " << stats.compiled << " compiled (" << stats.covered << " played)\n"
                      << "  regions:            " << stats.regions << ", " << stats.entries << " entries\n"
                      << (meta_precise ? "  mode:               precise (budget checked after every instruction)\n" : "");
        return 0;
    }
    if (!source.supports(*m0boy::parse_target(target))) {
        return refuse_target(source, *m0boy::parse_target(target), target);
    }
    if (target != "desktop") { /* every wired console of the Game Boy pipeline */
        return run_meta_target(argv[0], rom_path, output_dir, output_prefix, meta_dir,
                               coverage_paths, meta_precise, meta_static, meta_bpi, emit_comments,
                               *m0boy::parse_target(target), meta_thumb, meta_compiler, meta_sweep);
    }
    if (!coverage_paths.empty() || meta_compiler != "gcc" || meta_thumb || meta_precise || meta_static || meta_sweep || !meta_dir.empty()) {
        std::cerr << "Error: --coverage, --meta-compiler, --meta-thumb, --meta-precise, --meta-static, --meta-sweep and --meta-dir require a console target (meta, pokitto, espboy, pc, odroid, aka)\n";
        return 1;
    }
    if (!emit_android_project && (!android_package.empty() || !android_app_name.empty())) {
        std::cerr << "Error: --android-package and --android-app-name require --android\n";
        return 1;
    }
    if (emit_android_project && (disasm_only || analyze_only)) {
        std::cerr << "Error: --android is only supported when generating code\n";
        return 1;
    }
    if (!native_patch_manifest_path.empty() && (disasm_only || analyze_only)) {
        std::cerr << "Error: --native-patch is only supported when generating code\n";
        return 1;
    }

    if (!output_prefix.empty() && sanitize_prefix(output_prefix) != output_prefix) {
        std::cerr << "Error: --output-prefix must be a non-empty C identifier\n";
        return 1;
    }

    GenerationOptions generation_opts;
    generation_opts.verbose = verbose;
    generation_opts.trace_log = trace_log;
    generation_opts.limit_instructions = limit_instructions;
    generation_opts.codegen_jobs = requested_jobs;
    generation_opts.single_function = single_function;
    generation_opts.emit_comments = emit_comments;
    generation_opts.aggressive_scan = aggressive_scan;
    generation_opts.infer_data_zones = infer_data_zones;
    generation_opts.analyze_all_banks = analyze_all_banks;
    generation_opts.manual_entry_points = manual_entry_points;
    generation_opts.trace_file_path = trace_file_path;
    generation_opts.symbol_file_path = symbol_file_path;
    generation_opts.symbol_analysis_policy = symbol_analysis_policy;
    generation_opts.annotation_file_path = annotation_file_path;
    generation_opts.emit_android_project = emit_android_project;
    generation_opts.android_package = android_package;
    generation_opts.android_app_name = android_app_name;
    if (!disasm_only && !analyze_only) {
        generation_opts.runtime_dir =
            resolve_runtime_dir(argv[0], explicit_runtime_dir);
        if (generation_opts.runtime_dir.empty()) {
            std::cerr << "Error: Could not locate the runtime snapshot. "
                         "Use --runtime-dir or GBRECOMP_RUNTIME_DIR.\n";
            return 1;
        }
    }
    
    print_banner();

    fs::path input_path = rom_path;
    if (fs::exists(input_path) && fs::is_directory(input_path)) {
        if (!progress_json_path.empty() || !output_prefix.empty()) {
            std::cerr << "Error: --progress-json and --output-prefix are single-ROM only\n";
            return 1;
        }
        if (disasm_only || analyze_only) {
            std::cerr << "Error: Directory mode does not support --disasm or --analyze\n";
            return 1;
        }
        if (emit_android_project) {
            std::cerr << "Error: Android project generation is only supported for single-ROM output in v1\n";
            return 1;
        }
        if (!native_patch_manifest_path.empty()) {
            std::cerr << "Error: Native patch manifests are single-ROM only in v1\n";
            return 1;
        }
        if (specific_bank >= 0 || !manual_entry_points.empty()) {
            std::cerr << "Error: Directory mode does not support --bank or --add-entry-point\n";
            return 1;
        }
        if (!trace_file_path.empty() || !symbol_file_path.empty() || !annotation_file_path.empty()) {
            std::cerr << "Error: Directory mode does not support --use-trace, --symbols, or --annotations\n";
            return 1;
        }

        if (output_dir.empty()) {
            output_dir = input_path.filename().string() + "_output";
        }

        fs::path out_path = output_dir;
        if (!fs::exists(out_path)) {
            std::cout << "Creating output directory: " << out_path << "\n";
            fs::create_directories(out_path);
        }

        std::vector<fs::path> rom_paths = collect_rom_paths(input_path);
        if (rom_paths.empty()) {
            std::cerr << "Error: No .gb/.gbc/.sgb files found under " << input_path << "\n";
            return 1;
        }

        std::cout << "Discovered " << rom_paths.size() << " ROMs under " << input_path << "\n";

        size_t batch_jobs = resolve_parallel_job_count(requested_jobs, rom_paths.size());
        if ((verbose || trace_log) && batch_jobs > 1) {
            std::cout << "Verbose/trace logging enabled; using a single batch worker to keep output readable.\n";
            batch_jobs = 1;
        }
        std::cout << "Using " << batch_jobs << " worker"
                  << (batch_jobs == 1 ? "" : "s")
                  << " for multi-ROM generation\n";

        std::set<std::string> used_prefixes;
        std::vector<std::string> module_prefixes;
        module_prefixes.reserve(rom_paths.size());

        for (const fs::path& rom_file : rom_paths) {
            module_prefixes.push_back(make_unique_prefix(rom_file.stem().string(), used_prefixes));
        }

        std::vector<MultiRomModule> modules(rom_paths.size());
        GenerationOptions module_generation_opts = generation_opts;
        if (batch_jobs > 1) {
            module_generation_opts.codegen_jobs = 1;
        }

        if (batch_jobs <= 1) {
            for (size_t i = 0; i < rom_paths.size(); ++i) {
                if (!generate_multi_rom_module(rom_paths[i], module_generation_opts, out_path,
                                               module_prefixes[i], modules[i])) {
                    return 1;
                }
            }
        } else {
            std::atomic<size_t> next_rom_index{0};
            std::atomic<bool> failed{false};
            std::vector<std::thread> workers;
            workers.reserve(batch_jobs);

            auto worker = [&]() {
                for (;;) {
                    const size_t rom_index = next_rom_index.fetch_add(1);
                    if (rom_index >= rom_paths.size() || failed.load()) {
                        return;
                    }
                    if (!generate_multi_rom_module(rom_paths[rom_index], module_generation_opts,
                                                   out_path, module_prefixes[rom_index],
                                                   modules[rom_index])) {
                        failed.store(true);
                        return;
                    }
                }
            };

            for (size_t i = 0; i < batch_jobs; ++i) {
                workers.emplace_back(worker);
            }
            for (std::thread& worker_thread : workers) {
                worker_thread.join();
            }
            if (failed.load()) {
                return 1;
            }
        }

        std::string launcher_name = sanitize_prefix(out_path.filename().string());
        if (launcher_name.empty()) {
            launcher_name = sanitize_prefix(input_path.filename().string());
        }
        if (launcher_name.empty()) {
            launcher_name = "multi_rom_launcher";
        }

        const fs::path launcher_source_path = out_path / "launcher_main.cpp";
        const fs::path launcher_manifest_path = out_path / "launcher_manifest.json";
        const fs::path launcher_cmake_path = out_path / "CMakeLists.txt";

        if (!write_text_file(launcher_source_path, make_launcher_source(launcher_name, modules))) {
            std::cerr << "Error: Failed to write " << launcher_source_path << "\n";
            return 1;
        }
        if (!write_text_file(launcher_manifest_path, make_launcher_manifest(launcher_name, modules))) {
            std::cerr << "Error: Failed to write " << launcher_manifest_path << "\n";
            return 1;
        }
        if (!write_text_file(launcher_cmake_path, make_multi_rom_cmake(launcher_name, out_path, modules))) {
            std::cerr << "Error: Failed to write " << launcher_cmake_path << "\n";
            return 1;
        }
        if (!copy_runtime_snapshot(generation_opts.runtime_dir, out_path)) {
            std::cerr << "Error: Failed to embed runtime snapshot from "
                      << generation_opts.runtime_dir << "\n";
            return 1;
        }

        std::cout << "\nGenerated multi-ROM launcher files:\n";
        std::cout << "  " << launcher_source_path << "\n";
        std::cout << "  " << launcher_manifest_path << "\n";
        std::cout << "  " << launcher_cmake_path << "\n";
        std::cout << "\nGames:\n";
        for (const MultiRomModule& module : modules) {
            std::cout << "  " << module.output_prefix << " -> " << module.display_name << "\n";
        }
        std::cout << "\nBuild instructions:\n";
        std::cout << "  cmake -G Ninja -S " << out_path << " -B " << (out_path / "build") << "\n";
        std::cout << "  ninja -C " << (out_path / "build") << "\n";
        std::cout << "  " << ((out_path / "build") / launcher_name) << "\n";
        return 0;
    }

    ProgressReporter progress;
    if (!progress_json_path.empty() && !progress.open(progress_json_path)) {
        std::cerr << "Error: Failed to open generation progress destination\n";
        return 1;
    }
    
    // Load ROM
    std::cout << "Loading ROM: " << rom_path << "\n";
    
    auto rom_opt = gbrecomp::ROM::load(rom_path);
    if (!rom_opt) {
        progress.failure("rom-validation", "rom-unreadable");
        std::cerr << "Error: Failed to load ROM\n";
        return 1;
    }
    
    auto& rom = *rom_opt;
    
    if (!rom.is_valid()) {
        progress.failure("rom-validation", "rom-invalid");
        std::cerr << "Error: " << rom.error() << "\n";
        return 1;
    }
    progress.stage("rom-validated", 1);
    
    // Print ROM info
    gbrecomp::print_rom_info(rom);

    gbrecomp::SymbolTable symbol_table;
    if (!symbol_file_path.empty()) {
        std::string error;
        if (!symbol_table.load_sym_file(symbol_file_path, &rom, &error)) {
            progress.failure("analysis-inputs", "symbols-invalid");
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        const auto& symbol_stats = symbol_table.load_stats();
        std::cout << "Loaded " << symbol_table.size() << " unique address symbols from "
                  << symbol_file_path << " ("
                  << symbol_stats.address_records << " address records, "
                  << symbol_stats.duplicate_address_records
                  << " duplicate-address aliases, "
                  << symbol_stats.constant_records << " constant records, "
                  << symbol_stats.unique_constants << " unique constants)\n";
        std::cout << "Symbol analysis policy: "
                  << (symbol_analysis_policy ==
                              gbrecomp::SymbolAnalysisPolicy::NAMES_ONLY
                          ? "names-only"
                          : "infer-boundaries")
                  << "\n";
    }
    if (!annotation_file_path.empty()) {
        const size_t previous_annotation_count = symbol_table.annotation_count();
        std::string error;
        if (!symbol_table.load_annotation_file(annotation_file_path, &error)) {
            progress.failure("analysis-inputs", "annotations-invalid");
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Loaded " << (symbol_table.annotation_count() - previous_annotation_count)
                  << " annotations from "
                  << annotation_file_path << "\n";
    }
    
    // Set default output directory
    if (output_dir.empty()) {
        output_dir = fs::path(rom_path).stem().string() + "_output";
    }
    
    // Disassemble only mode
    if (disasm_only) {
        std::cout << "\nDisassembly:\n";
        std::cout << "============\n";
        
        gbrecomp::BankId bank = (specific_bank >= 0)
            ? static_cast<gbrecomp::BankId>(specific_bank)
            : 0;
        auto instructions = gbrecomp::decode_bank(rom, bank);
        
        for (const auto& instr : instructions) {
            std::cout << instr.disassemble() << "\n";
        }
        return 0;
    }
    
    // Analyze control flow
    std::cout << "\nAnalyzing control flow...\n";
    

    gbrecomp::AnalyzerOptions analyze_opts;
    analyze_opts.trace_log = trace_log;
    analyze_opts.verbose = verbose;
    analyze_opts.verbose = verbose;
    analyze_opts.max_instructions = limit_instructions;
    analyze_opts.entry_points = manual_entry_points;
    analyze_opts.analyze_all_banks = analyze_all_banks;
    analyze_opts.aggressive_scan = aggressive_scan;
    analyze_opts.infer_data_zones = infer_data_zones;
    analyze_opts.trace_file_path = trace_file_path;
    analyze_opts.annotations = gbrecomp::build_analysis_annotations(
        symbol_table, symbol_analysis_policy);
    if (!symbol_file_path.empty() || !annotation_file_path.empty()) {
        const size_t explicit_boundaries = symbol_table.annotation_count();
        const size_t inferred_boundaries =
            analyze_opts.annotations.size() >= explicit_boundaries
                ? analyze_opts.annotations.size() - explicit_boundaries
                : 0;
        std::cout << "Analyzer boundary inputs: "
                  << analyze_opts.annotations.size() << " ("
                  << explicit_boundaries << " explicit annotations, "
                  << inferred_boundaries << " inferred from address symbols)\n";
    }

    const std::vector<gbrecomp::AnalyzerOptions::RamOverlay> dma_overlays =
        detect_oam_dma_overlays(rom);
    for (const auto& overlay : dma_overlays) {
            gbrecomp::BankId bank = static_cast<gbrecomp::BankId>(overlay.rom_addr >> 16);
            uint16_t offset = static_cast<uint16_t>(overlay.rom_addr & 0xFFFF);
            size_t rom_idx = (offset < 0x4000)
                ? offset
                : static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);

            std::cout << "Detected OAM DMA routine at ROM 0x" << std::hex << rom_idx
                        << " (Bank " << (int)bank << ":0x" << offset << ", size "
                        << std::dec << overlay.size << "). Mapping to HRAM "
                        << std::hex << std::showbase << overlay.ram_addr
                        << std::noshowbase << std::dec << ".\n";

            analyze_opts.ram_overlays.push_back(overlay);
    }

    auto analysis = gbrecomp::analyze(rom, analyze_opts);
    if (symbol_table.size() > 0) {
        gbrecomp::apply_symbols_to_analysis(symbol_table, analysis);
    }
    progress.stage("analysis-complete", 2);
    
    std::cout << "  Found " << analysis.stats.total_functions << " functions\n";
    std::cout << "  Found " << analysis.stats.total_blocks << " basic blocks\n";
    std::cout << "  " << analysis.label_addresses.size() << " labels needed\n";
    
    if (verbose) {
        gbrecomp::print_analysis_summary(analysis);
    }
    
    if (analyze_only) {
        return 0;
    }
    
    // Build IR
    std::cout << "\nBuilding IR...\n";
    
    gbrecomp::ir::BuilderOptions ir_opts;
    ir_opts.emit_comments = emit_comments;
    
    gbrecomp::ir::IRBuilder builder(ir_opts);
    auto ir_program = builder.build(
        analysis, output_prefix.empty() ? rom.name() : output_prefix);
    progress.stage("ir-complete", 3);
    
    std::cout << "  " << ir_program.blocks.size() << " IR blocks\n";
    std::cout << "  " << ir_program.functions.size() << " IR functions\n";

    gbrecomp::NativePatchPackage native_patch;
    if (!native_patch_manifest_path.empty()) {
        std::string native_patch_error;
        if (!gbrecomp::load_native_patch_manifest(native_patch_manifest_path,
                                                  rom.data(),
                                                  rom.size(),
                                                  native_patch,
                                                  native_patch_error)) {
            progress.failure("native-patch", "native-patch-invalid");
            std::cerr << "Error: Native patch manifest: " << native_patch_error << "\n";
            return 1;
        }
        if (!gbrecomp::validate_native_patch_bindings(native_patch,
                                                      ir_program,
                                                      native_patch_error)) {
            progress.failure("native-patch", "native-patch-unresolved");
            std::cerr << "Error: Native patch manifest: " << native_patch_error << "\n";
            return 1;
        }
        std::cout << "  Native patch " << native_patch.patch_id << ": "
                  << native_patch.bindings.size() << " binding(s), exact ROM "
                  << native_patch.rom_sha256.substr(0, 12) << "...\n";
    }
    
    // Generate code
    std::cout << "\nGenerating C code...\n";
    
    gbrecomp::codegen::GeneratorOptions gen_opts;
    gen_opts.output_prefix = output_prefix.empty()
        ? sanitize_prefix(fs::path(rom_path).stem().string())
        : output_prefix;
    gen_opts.output_dir = output_dir;
    gen_opts.emit_comments = emit_comments;
    gen_opts.single_function_mode = single_function;
    gen_opts.parallel_codegen_jobs = requested_jobs;
    gen_opts.native_patch = std::move(native_patch);
    append_codegen_ram_overlays(rom, analyze_opts.ram_overlays, gen_opts);
    
    auto output = gbrecomp::codegen::generate_output(
        ir_program, rom.data(), rom.size(), gen_opts);
    progress.stage("code-generation-complete", 4);
    
    // Create output directory
    fs::path out_path = output_dir;
    if (!fs::exists(out_path)) {
        std::cout << "Creating output directory: " << out_path << "\n";
        fs::create_directories(out_path);
    }
    
    // Write output files
    if (!gbrecomp::codegen::write_output(output, output_dir)) {
        progress.failure("output-write", "output-write-failed");
        std::cerr << "Error: Failed to write output files\n";
        return 1;
    }
    if (!copy_runtime_snapshot(generation_opts.runtime_dir, out_path)) {
        progress.failure("runtime-snapshot", "runtime-copy-failed");
        std::cerr << "Error: Failed to embed runtime snapshot from "
                  << generation_opts.runtime_dir << "\n";
        return 1;
    }

    std::string resolved_android_package;
    std::string resolved_android_app_name;
    if (emit_android_project) {
        resolved_android_package = android_package.empty()
            ? make_default_android_package(gen_opts.output_prefix)
            : android_package;
        if (!is_valid_java_package(resolved_android_package)) {
            progress.failure("android-project", "android-package-invalid");
            std::cerr << "Error: Invalid Android package name '" << resolved_android_package << "'\n";
            return 1;
        }
        resolved_android_app_name = android_app_name;
        if (resolved_android_app_name.empty()) {
            resolved_android_app_name = !rom.header().title.empty()
                ? rom.header().title
                : gen_opts.output_prefix;
        }
        if (!write_android_project(out_path, gen_opts.output_prefix, output,
                                   resolved_android_package, resolved_android_app_name)) {
            progress.failure("android-project", "android-write-failed");
            std::cerr << "Error: Failed to write Android project scaffold\n";
            return 1;
        }
    }
    progress.stage("output-complete", 5);
    
    std::cout << "\nGenerated files:\n";
    std::cout << "  " << (out_path / output.header_file) << "\n";
    std::cout << "  " << (out_path / output.source_file) << "\n";
    std::cout << "  " << (out_path / output.main_file) << "\n";
    std::cout << "  " << (out_path / output.cmake_file) << "\n";
    if (!output.rom_data_file.empty()) {
        std::cout << "  " << (out_path / output.rom_data_file) << "\n";
    }
    if (emit_android_project) {
        std::cout << "  " << (out_path / "android" / "app" / "build.gradle") << "\n";
        std::cout << "  " << (out_path / "android" / "app" / "jni" / "src" / "CMakeLists.txt") << "\n";
        std::cout << "  " << (out_path / "android" / "app" / "src" / "main" / "AndroidManifest.xml") << "\n";
    }
    
    std::cout << "\nBuild instructions:\n";
    std::cout << "  cmake -G Ninja -S " << out_path << " -B " << (out_path / "build") << "\n";
    std::cout << "  ninja -C " << (out_path / "build") << "\n";
    std::cout << "  " << ((out_path / "build") / gen_opts.output_prefix) << "\n";
    if (emit_android_project) {
        std::cout << "\nAndroid build instructions:\n";
        std::cout << "  SDL2_SOURCE_DIR=/path/to/SDL gradle -p " << (out_path / "android") << " :app:assembleDebug\n";
        std::cout << "  Open " << (out_path / "android") << " in Android Studio if you prefer the IDE flow.\n";
        std::cout << "  Package: " << resolved_android_package << "\n";
        std::cout << "  App name: " << resolved_android_app_name << "\n";
    }
    progress.complete();
    
    return 0;
}
