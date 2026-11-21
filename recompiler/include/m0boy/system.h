#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace m0boy {

enum class CpuId { sm83, z80, lua, m6502, huc6280, m6809, m68k, w65c816, tlcs900h, m0plus };
enum class SystemId { gameboy, sg1000, gamegear, mastersystem, pico8, msx, nes, pce, vectrex, megadrive, snes, ngp, meta, pokitto };
enum class TargetId { desktop, meta, pokitto, espboy, pc, odroid, aka };

struct CpuDescriptor {
    CpuId id;
    std::string_view name;
    bool decoder_available;
};

/* The destination console as the generator sees it: the hardware the runtime
 * has to fit in.  host targets (desktop, pc) run in a process and are always
 * feasible; ram_bytes counts the contiguous fast SRAM the firmware's .bss,
 * .data and stack live in (PSRAM is slower and only serves the ROM cache and
 * large buffers, so a system's requirement never draws on it); flash_app_bytes
 * is the room for the firmware plus everything it embeds. */
struct TargetFacts {
    TargetId id;
    std::string_view name;
    bool host;
    uint32_t ram_bytes;
    uint32_t psram_bytes;
    uint32_t flash_app_bytes;
    uint16_t screen_cols;
    uint16_t screen_rows;
    bool has_sd;
    std::string_view description;
};

// Metadata shared across cartridge formats; console-specific headers remain
// in their frontend. SG-1000 images do not pass through the Nintendo parser.
struct CartridgeInfo {
    SystemId system;
    std::string title;
    size_t rom_bytes;
    size_t cartridge_ram_bytes;
    std::string mapper;
    bool colour = false; /* Neo Geo Pocket Color: the header's mode byte 0x10 */
};

using InspectCartridge = std::optional<CartridgeInfo> (*)(
    const std::filesystem::path&, std::string& error);

struct SystemDescriptor {
    SystemId id;
    std::string_view name;
    CpuId cpu;
    uint16_t reset_address;
    size_t work_ram_bytes;
    /* The guest state plus the working buffers this system's runtime needs on
     * target hardware (guest RAM, video RAM, line buffers) — the number the
     * destination console must hold in ram_bytes, unlike work_ram_bytes which
     * describes the emulated console for --info. */
    uint32_t min_ram_bytes;
    InspectCartridge inspect;
    std::span<const TargetId> codegen_targets;
    // A native project generator exists for that destination.
    bool wired(TargetId target) const;
    // wired AND the destination hardware can run the runtime (see targets()).
    bool supports(TargetId target) const;
};

std::span<const CpuDescriptor> cpus();
std::span<const SystemDescriptor> systems();
std::span<const TargetFacts> targets();
const CpuDescriptor& cpu_descriptor(CpuId id);
const SystemDescriptor& system_descriptor(SystemId id);
const TargetFacts& target_descriptor(TargetId id);
std::optional<CpuId> parse_cpu(std::string_view name);
std::optional<SystemId> parse_system(std::string_view name);
std::optional<TargetId> parse_target(std::string_view name);
// Hardware feasibility alone, wiring aside: does the runtime fit?
bool target_fits(const SystemDescriptor& system, const TargetFacts& target);
// Empty when the pair is feasible, else a one-line explanation with numbers.
std::string target_misfit_reason(const SystemDescriptor& system, const TargetFacts& target);
// Recognised extensions only. Raw .bin files require an explicit system.
std::optional<SystemId> detect_system(const std::filesystem::path& path);

namespace gameboy {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace pico8 {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace sg1000 {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_gamegear(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_mastersystem(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_msx(const std::filesystem::path&, std::string&);
}
namespace nes {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace pce {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace vectrex {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace megadrive {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace snes {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace ngp {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace m0plus {
std::optional<CartridgeInfo> inspect_meta(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_pokitto(const std::filesystem::path&, std::string&);
}

} // namespace m0boy
