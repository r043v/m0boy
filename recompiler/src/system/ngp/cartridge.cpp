/* Neo Geo Pocket cartridge inspection.  The header is 0x30 bytes: the
 * licence string (28 bytes), the 24-bit entry point (0x1C), the catalog
 * word (0x20), the sub-catalog (0x22), the colour mode (0x23: 0x00 mono,
 * 0x10 colour) and the 12-byte name (0x24).  The banking is the flash
 * die's own: the first 2 MiB answer at 0x200000, a 4 MiB cart's upper half
 * answers at 0x800000. */
#include "m0boy/ngp_generator.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace m0boy::ngp {
namespace {

bool header_ok(const std::string& data, size_t ho) {
    if (data.size() < ho + 0x30) return false;
    const uint32_t pc = (uint32_t)data[ho + 0x1C] | ((uint32_t)data[ho + 0x1D] << 8) | ((uint32_t)data[ho + 0x1E] << 16);
    const uint8_t mode = data[ho + 0x23];
    return pc >= 0x200000 && pc < 0xA00000 && !(mode & ~0x10u);
}

} // namespace

/* The flash image: some dumps carry a prefix (a loader's padding), the header
 * then sits at 0x20100; it is stripped here so the image maps flat from
 * 0x200000, for the header reads of inspect() and for generation after. */
std::string load_flash(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot open " + path.string();
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string data = buffer.str();
    if (data.size() < 0x30) {
        error = path.string() + ": too small for a Neo Geo Pocket header ("
                + std::to_string(data.size()) + " bytes)";
        return {};
    }
    if (data.size() > 4 * 1024 * 1024 + 0x20100) {
        error = path.string() + ": larger than the 4 MiB flash the cartridges carry";
        return {};
    }
    if (!header_ok(data, 0) && header_ok(data, 0x20100)) data.erase(0, 0x20100);
    if (data.size() > 4 * 1024 * 1024) {
        error = path.string() + ": larger than the 4 MiB flash the cartridges carry";
        return {};
    }
    return data;
}

std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    const std::string data = load_flash(path, error);
    if (data.empty()) return std::nullopt;
    const auto* rom = reinterpret_cast<const uint8_t*>(data.data());
    /* the entry point lives in the cartridge windows (0x200000 up) */
    const uint32_t start_pc = rom[0x1C] | (rom[0x1D] << 8) | ((uint32_t)rom[0x1E] << 16);
    if (start_pc < 0x200000 || start_pc >= 0xA00000) {
        std::ostringstream out;
        out << path.string() << ": entry point $" << std::hex << start_pc
            << " is outside the cartridge windows ($200000-$9FFFFF)";
        error = out.str();
        return std::nullopt;
    }
    if (start_pc - 0x200000 >= data.size()) {
        error = path.string() + ": entry point beyond the image (a chip this header does not describe?)";
        return std::nullopt;
    }
    const uint8_t mode = rom[0x23];
    if (mode & ~0x10u) {
        std::ostringstream out;
        out << path.string() << ": unknown colour mode byte $" << std::hex << (unsigned)mode;
        error = out.str();
        return std::nullopt;
    }
    /* the licence field names SNK on every commercial cart; homebrew may
     * differ, so it is a note, not a gate */
    char licence[29] = {};
    memcpy(licence, rom, 28);
    for (char& c : licence) if (!std::isprint((unsigned char)c)) c = ' ';

    /* the name: 12 bytes, ASCII on most western releases, UTF-16 on
     * Japanese ones — keep the printable run either way */
    char name[13] = {};
    for (unsigned i = 0; i < 12; i++) {
        const uint8_t c = rom[0x24 + i];
        name[i] = std::isprint(c) ? (char)c : ' ';
    }
    /* trim */
    std::string title(name);
    while (!title.empty() && title.back() == ' ') title.pop_back();

    CartridgeInfo info;
    info.system = SystemId::ngp;
    info.title = title.empty() ? path.stem().string() : title;
    info.rom_bytes = data.size();
    info.cartridge_ram_bytes = 0; /* saves live in the cartridge's flash */
    info.mapper = "flash2M";
    info.colour = (mode & 0x10u) != 0;
    return info;
}

} // namespace m0boy::ngp
