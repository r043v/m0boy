#include "m0boy/system.h"
#include <fstream>
#include <vector>

namespace m0boy::megadrive {
std::optional<CartridgeInfo> inspect(const std::filesystem::path &path, std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot read Mega Drive ROM: " + path.string(); return std::nullopt; }
    std::vector<unsigned char> rom{std::istreambuf_iterator<char>(file), {}};
    if (rom.size() < 0x200 || rom.size() > 0x400000 ||
        std::string_view(reinterpret_cast<const char *>(rom.data() + 0x100), 4) != "SEGA") {
        error = "Mega Drive ROM needs a SEGA header at $100 (512 bytes to 4 MiB)";
        return std::nullopt;
    }
    auto be32 = [&](size_t at) { return (uint32_t(rom[at]) << 24) | (uint32_t(rom[at+1]) << 16) |
                                     (uint32_t(rom[at+2]) << 8) | rom[at+3]; };
    /* Bus 68000 sur 24 bits : un SP $FFFFFC00 (Street Fighter II) est la RAM $FFFC00. */
    const uint32_t sp = be32(0) & 0xffffffu, pc = be32(4) & 0xffffffu;
    /* Sonic 3 ships a stack pointer of 0 and installs its own stack first
     * thing; the CPU reset just takes it, so the runtime does too. */
    if ((sp != 0 && (sp < 0xff0000u || sp > 0x1000000u || (sp & 1))) || pc < 0x1e0 ||
        pc >= rom.size() || (pc & 1)) {
        error = "Mega Drive ROM needs a ROM reset PC and a work-RAM stack";
        return std::nullopt;
    }
    return CartridgeInfo{SystemId::megadrive, path.stem().string(), rom.size(), 0,
                         "flat cartridge"};
}
}
