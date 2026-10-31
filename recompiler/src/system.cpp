#include "m0boy/system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace m0boy {
namespace {
constexpr std::array cpu_table = {
    CpuDescriptor{CpuId::sm83, "sm83", true},
    CpuDescriptor{CpuId::z80, "z80", true},
    CpuDescriptor{CpuId::lua, "lua", false}, // PICO-8 Lua bytecode (femto8's compiler)
    CpuDescriptor{CpuId::m6502, "6502", true},    // NES (2A03); HuC6280 extends it
    CpuDescriptor{CpuId::huc6280, "huc6280", true},  // PC Engine; extends the 6502 table
    CpuDescriptor{CpuId::m6809, "6809", false},      // Vectrex: the recompiler decodes it
    CpuDescriptor{CpuId::m68k, "m68k", false}, // bounded compiler decoder, no --disasm listing yet
    CpuDescriptor{CpuId::w65c816, "w65c816", false}, // SNES 5A22; every byte is a defined opcode
    CpuDescriptor{CpuId::m0plus, "m0plus", true},
};

/* Wired pipelines: where a project generator exists today.  The runtime's
 * needs (min_ram_bytes) further restrict what the hardware accepts —
 * supports() is both, --list-systems prints the result. */
constexpr std::array console_targets = {
    TargetId::desktop, TargetId::meta, TargetId::pokitto, TargetId::espboy, TargetId::pc,
    TargetId::odroid, TargetId::aka,
};
// The Sega consoles' frontends run on every destination, the ESPboy included.
constexpr std::array sega_targets = console_targets;
// PICO-8: compiled to C for the META, the Pokitto, the ESPboy and the
// Gamebuino AKA (the ESP32 family's shared core), plus the PC.
constexpr std::array pico8_targets = {TargetId::desktop, TargetId::meta, TargetId::pokitto, TargetId::espboy, TargetId::pc, TargetId::aka};
// MSX: the SG-1000 frontends (TMS9918A, Z80); the ESPboy and PC builds are
// not wired up.
constexpr std::array msx_targets = {TargetId::desktop, TargetId::meta, TargetId::pokitto, TargetId::pc};
// NES: the recompiled pipeline on the META, the Pokitto and the desktop
// harness.
// PC Engine: the interpreter on the desktop harness, the PC's SDL2
// window and the Gamebuino AKA (the HuCard whole in the app partition;
// the other consoles wait on the picture's budget — a 768 KiB HuCard
// does not fit a 256 KiB flash either).
constexpr std::array nes_targets = {TargetId::desktop, TargetId::meta, TargetId::pokitto, TargetId::pc};
constexpr std::array pce_targets = {TargetId::desktop, TargetId::pc, TargetId::aka};
// Vectrex: the 6809 recompiled in front of the interpreter on the META and
// the Gamebuino AKA (the ESP32 family's shared core), and the SDL host (pc).
constexpr std::array vectrex_targets = {TargetId::meta, TargetId::pc, TargetId::aka};
constexpr std::array megadrive_targets = {TargetId::desktop, TargetId::pc, TargetId::aka};
constexpr std::array m0plus_targets = {TargetId::pc};
// the META firmware also runs on the AKA, through the standalone SAMD21 runtime
constexpr std::array meta_source_targets = {TargetId::pc, TargetId::aka};
// SNES: the 65C816/PPU/APU runtime on the desktop harness and the PC's
// SDL2 window (the cartridge embedded); the consoles wait on the RAM
// budget — the runtime holds ~434 KiB between WRAM, VRAM, A-RAM, the
// SRAM window and the displayed frame.
constexpr std::array snes_targets = {TargetId::desktop, TargetId::pc};
constexpr std::array<TargetId, 0> no_targets {};

constexpr std::array system_table = {
    SystemDescriptor{SystemId::gameboy, "gameboy", CpuId::sm83, 0x0100,
                     8192, 24576, gameboy::inspect, console_targets},
    SystemDescriptor{SystemId::sg1000, "sg1000", CpuId::z80, 0x0000,
                     1024, 20480, sg1000::inspect, sega_targets},
    SystemDescriptor{SystemId::gamegear, "gg", CpuId::z80, 0x0000,
                     8192, 26624, sg1000::inspect_gamegear, sega_targets},
    SystemDescriptor{SystemId::mastersystem, "sms", CpuId::z80, 0x0000,
                     8192, 26624, sg1000::inspect_mastersystem, sega_targets},
    SystemDescriptor{SystemId::msx, "msx", CpuId::z80, 0x4000,
                     16384, 28672, sg1000::inspect_msx, msx_targets},
    SystemDescriptor{SystemId::pico8, "pico8", CpuId::lua, 0x0000,
                     0x8000, 30720, pico8::inspect, pico8_targets},
    /* NES: 2 KiB WRAM, 2 KiB VRAM, 8 KiB CHR RAM and the line buffers; the
     * ROM (16 KiB to 1 MiB) is the real constraint, as with the Game Boy. */
    SystemDescriptor{SystemId::nes, "nes", CpuId::m6502, 0xfffc,
                     2048, 16384, nes::inspect, nes_targets},
    /* PC Engine: 8 KiB WRAM plus the VDC's 64 KiB VRAM and the frontend
     * buffers — more than every 32 KiB console and than the ESPboy's
     * ~9 KiB of free RAM after a Sega firmware. */
    SystemDescriptor{SystemId::pce, "pce", CpuId::huc6280, 0xfffe,
                     8192, 86016, pce::inspect, pce_targets},
    /* Vectrex: 1 KiB of RAM; the console frontend adds an 8 KiB phosphor */
    SystemDescriptor{SystemId::vectrex, "vectrex", CpuId::m6809, 0xfffe,
                     1024, 12288, vectrex::inspect, vectrex_targets},
    /* First Mega Drive profile: 4 KiB of the 68k's upper work RAM and
     * CRAM/background colour.  The generator checks every reachable opcode
     * and rejects ROMs outside this deliberately small profile. */
    SystemDescriptor{SystemId::megadrive, "megadrive", CpuId::m68k, 0x0004,
                     65536, 8192, megadrive::inspect, megadrive_targets},
    /* SNES: the CPU, 128 KiB of WRAM, the cartridge's 32 KiB SRAM window,
     * the PPU's 64 KiB VRAM, the APU's 64 KiB A-RAM and the displayed
     * frame; the ROM banks live through the LoROM/HiROM windows. */
    SystemDescriptor{SystemId::snes, "snes", CpuId::w65c816, 0x00fffc,
                     131072, 446464, snes::inspect, snes_targets},
    /* Firmware images, not cartridges.  The reset vector is at the image
     * start; META is linked at 0x4000, Pokitto at zero.  Execution/codegen
     * PC projects include the board emulator. */
    SystemDescriptor{SystemId::meta, "meta", CpuId::m0plus, 0x4004,
                     32768, 32768, m0plus::inspect_meta, meta_source_targets},
    SystemDescriptor{SystemId::pokitto, "pokitto", CpuId::m0plus, 0x0004,
                     32768, 32768, m0plus::inspect_pokitto, m0plus_targets},
};

/* Facts of each destination.  meta/pokitto: the whole contiguous SRAM is
 * 32 KiB (the Pokitto adds two 2 KiB banks the audio ring and the SD cache
 * use); espboy: the ESP8266's 80 KiB heap holds firmware and ROM cache;
 * odroid: ESP32-WROVER, 16 MiB flash of which 2 MiB for the app, 4 MiB
 * PSRAM for the ROM; aka: ESP32-S3, 8 MiB flash and octal PSRAM (official CORE v1.4 library). */
constexpr std::array target_table = {
    TargetFacts{TargetId::desktop, "desktop", true, 0, 0, 0, 0, 0, false,
                "host process (harnesses, unlimited)"},
    TargetFacts{TargetId::meta, "meta", false, 32768, 0, 245760, 160, 128, true,
                "Gamebuino META (SAMD21 Cortex-M0+, 256 KiB flash)"},
    TargetFacts{TargetId::pokitto, "pokitto", false, 32768, 0, 233472, 220, 176, true,
                "Pokitto (LPC11U68, loader at 0x39000, +2+2 KiB banks)"},
    TargetFacts{TargetId::espboy, "espboy", false, 81920, 0, 1048576, 128, 128, false,
                "ESPboy (ESP8266, no SD, ROM and saves in 4 MiB flash)"},
    TargetFacts{TargetId::pc, "pc", true, 0, 0, 0, 320, 240, true,
                "PC (SDL2 window, SD card as a file image)"},
    TargetFacts{TargetId::odroid, "odroid", false, 327680, 4194304, 2097152, 320, 240, true,
                "ODROID-GO (ESP32-WROVER, ILI9341, 16 MiB flash, 4 MiB PSRAM)"},
    TargetFacts{TargetId::aka, "aka", false, 327680, 8388608, 0x2EE000, /* the official 3000K app slot */ 320, 240, true,
                "Gamebuino AKA (ESP32-S3, 8 MiB flash, octal PSRAM, i80 LCD, SDMMC)"},
};
}

std::span<const CpuDescriptor> cpus() { return cpu_table; }
std::span<const SystemDescriptor> systems() { return system_table; }
std::span<const TargetFacts> targets() { return target_table; }

const CpuDescriptor& cpu_descriptor(CpuId id) {
    for (const auto& cpu : cpus()) if (cpu.id == id) return cpu;
    throw std::invalid_argument("Unknown CPU id");
}

const SystemDescriptor& system_descriptor(SystemId id) {
    for (const auto& system : systems()) if (system.id == id) return system;
    throw std::invalid_argument("Unknown source system id");
}

const TargetFacts& target_descriptor(TargetId id) {
    for (const auto& target : targets()) if (target.id == id) return target;
    throw std::invalid_argument("Unknown target id");
}

std::optional<CpuId> parse_cpu(std::string_view name) {
    for (const auto& cpu : cpus()) if (cpu.name == name) return cpu.id;
    return std::nullopt;
}

std::optional<SystemId> parse_system(std::string_view name) {
    for (const auto& system : systems()) if (system.name == name) return system.id;
    return std::nullopt;
}

std::optional<TargetId> parse_target(std::string_view name) {
    for (const auto& target : targets()) if (target.name == name) return target.id;
    return std::nullopt;
}

bool SystemDescriptor::wired(TargetId target) const {
    return std::find(codegen_targets.begin(), codegen_targets.end(), target)
           != codegen_targets.end();
}

bool target_fits(const SystemDescriptor& system, const TargetFacts& target) {
    return target.host || system.min_ram_bytes <= target.ram_bytes;
}

std::string target_misfit_reason(const SystemDescriptor& system, const TargetFacts& target) {
    if (target_fits(system, target)) return {};
    std::ostringstream out;
    out << system.name << " needs " << system.min_ram_bytes / 1024 << " KiB of RAM on target; "
        << target.name << " (" << target.description << ") holds " << target.ram_bytes / 1024
        << " KiB";
    if (target.psram_bytes) out << " and " << target.psram_bytes / 1024 << " KiB of slower PSRAM";
    return out.str();
}

bool SystemDescriptor::supports(TargetId target) const {
    const auto& facts = target_descriptor(target);
    return wired(target) && target_fits(*this, facts);
}

std::optional<SystemId> detect_system(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == ".sg") return SystemId::sg1000;
    if (ext == ".gg") return SystemId::gamegear;
    if (ext == ".sms") return SystemId::mastersystem;
    if (ext == ".p8") return SystemId::pico8;
    if (ext == ".mx1" || ext == ".msx") return SystemId::msx;
    if (ext == ".nes") return SystemId::nes;
    if (ext == ".pce") return SystemId::pce;
    if (ext == ".vec") return SystemId::vectrex;
    if (ext == ".md" || ext == ".gen") return SystemId::megadrive;
    if (ext == ".sfc" || ext == ".smc" || ext == ".swc") return SystemId::snes;
    if (ext == ".rom") { // a MSX ROM starts with "AB"
        std::ifstream file(path, std::ios::binary);
        char header[2] = {};
        if (file.read(header, 2) && header[0] == 'A' && header[1] == 'B') return SystemId::msx;
    }
    if (ext == ".png") {
        auto stem = path.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::tolower(c); });
        if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".p8") == 0) return SystemId::pico8;
    }
    if (ext == ".gb" || ext == ".gbc" || ext == ".sgb") return SystemId::gameboy;
    return std::nullopt;
}
} // namespace m0boy
