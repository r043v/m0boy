#pragma once
/* --rom-external: PC projects that do not embed the cartridge (targets/common/runtime/m0_rom_ext.h loads it). */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace m0boy {
inline bool g_rom_external = false;
inline uint32_t rom_crc32(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}
/* the id header of the image the runtime must find (the bytes the generator would have embedded) */
inline std::string rom_id_header(const std::string &prefix, const uint8_t *d, size_t n, const std::string &name) {
    char b[320];
    std::snprintf(b, sizeof b, "#pragma once\n#define %s_ROM_EXPECT_SIZE %zuu\n#define %s_ROM_EXPECT_CRC32 0x%08xu\n#define %s_ROM_NAME \"%s\"\n",
                  prefix.c_str(), n, prefix.c_str(), rom_crc32(d, n), prefix.c_str(), name.c_str());
    return b;
}
}
