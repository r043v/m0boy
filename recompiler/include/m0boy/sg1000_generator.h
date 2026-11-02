#pragma once
#include "m0boy/system.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

struct GenerateOptions {
    std::filesystem::path output, meta_templates;
    std::string name;
    std::string sd_name; ///< 8.3 base of NAME.BIN, NAME.GG/.SMS, NAME.SAV; empty: derived from name
    TargetId target;
    std::vector<std::filesystem::path> coverage;
    std::filesystem::path coverage_map; ///< deepeel's physical-bank memory map: every code range seeds the walk
    std::filesystem::path ram_image;   ///< 8 KiB snapshot of C000-DFFF from a played session (compiled in place)
    bool precise = false;          ///< sync before every instruction (differential tests)
    unsigned sync_cycles = 0;      ///< EmitOptions::sync_cycles (40: bounded interrupt latency)
    bool gamegear = false;         ///< Game Gear cartridge (Sega mapper, VDP mode 4)
    bool mastersystem = false;     ///< with gamegear: a Master System cartridge
    bool msx = false;              ///< MSX1 cartridge (simulated BIOS, TMS9918A, AY-3-8910)
    int msx_mapper = -1;           ///< msx: M0_MSX_* to force, -1: detect from the ROM
    bool msx_pack = false;         ///< msx, consoles: LZSS sectors in flash even when the ROM fits raw (more flash for compiled code)
    std::string compiler = "gcc";  ///< firmware compiler: gcc or clang
    size_t code_budget_bytes = 0;  ///< 0: the flash left by the runtime and the ROM
    size_t bytes_per_instruction = 0; ///< flash per compiled instruction; 0: the default estimate
    size_t fill_bytes = 0;         ///< Game Gear: flash left by a measured build, spent on more ROM sectors
    std::filesystem::path rom_plan; ///< Game Gear: SD reads each ROM block saves (tools/profile m0romsim)
    bool aggressive_scan = true;  ///< seed sweep-validated unreached courses
    bool infer_data_zones = true;  ///< static code/data sweep of the ROM
};

struct GenerateStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    size_t swept = 0;          ///< instructions added by the speculative sweep
    size_t data_zones = 0;     ///< inferred data zones (>= 512 bytes)
    size_t data_bytes = 0;     ///< ROM bytes inside those zones
    bool cart_ram = false;
    size_t streamed_pages = 0; ///< Game Gear pages left on the SD card
    size_t pool_sectors = 0;   ///< Game Gear sectors of those pages kept in flash
    bool packed = false;      ///< all ROM data in flash, no ROM file required
    size_t packed_sectors = 0, pool_bytes = 0;
};

bool generate(const std::filesystem::path& rom, const GenerateOptions&, GenerateStats&, std::string& error);

} // namespace m0boy::sg1000
