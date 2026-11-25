#include "m0boy/system.h"

#include <array>
#include <fstream>

namespace m0boy::gp32 {

std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    constexpr size_t info_size = 1108;
    constexpr size_t payload_offset = 12 + 32 + 32 + 16 + 1024;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, payload_offset + 8> header{};
    if (ec || !file.read(reinterpret_cast<char*>(header.data()), header.size())) {
        error = "Cannot read GP32 FXE header";
        return std::nullopt;
    }

    const auto le32 = [&](size_t offset) {
        return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
               uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24;
    };
    const auto payload_size = le32(payload_offset);
    const auto key_size = le32(payload_offset + 4);
    if (le32(0) != 0x20657866 || le32(8) != info_size || payload_size < 32 ||
        key_size == 0 || uint64_t(payload_size) + key_size > size - header.size()) {
        error = "Invalid GP32 FXE header or truncated payload";
        return std::nullopt;
    }

    std::string title;
    for (size_t i = 12; i < 44 && header[i]; ++i)
        title += header[i] >= 32 && header[i] != 127 ? char(header[i]) : '?';
    if (title.empty())
        title = path.stem().string();

    return CartridgeInfo{SystemId::gp32, title, static_cast<size_t>(size), 0, "FXE"};
}

} // namespace m0boy::gp32
