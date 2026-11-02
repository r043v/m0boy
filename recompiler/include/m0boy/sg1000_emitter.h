#pragma once
#include "m0boy/sg1000.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

/// Code location: ROM page << 16 | Z80 address.  Unbanked boards (SG-1000)
/// and the fixed areas of banked ones use page 0, so a location is then
/// the address itself.
using Loc = uint32_t;

/// A played location with the executions the profile recorded: the emitter
/// seeds its worklist and picks mapper defaults hottest first.
struct HotLoc {
    Loc loc;
    uint32_t hits; ///< execution count, summed over the coverage sessions
};

struct EmitOptions {
    bool precise = false;       ///< sync before every instruction (differential tests)
    /// Without precise: also sync before an instruction once this many
    /// T-states have run since the last sync on the straight path (0: loop
    /// heads only), which bounds how late an interrupt is taken.
    unsigned sync_cycles = 0;
    bool cart_ram = false;      ///< 8 KiB of cartridge RAM at 8000-BFFF
    /// Sega mapper (Game Gear): 0000-03FF fixed, three 16 KiB slots paged
    /// by FFFD-FFFF, 8 KiB of RAM at C000-FFFF.
    bool banked = false;
    bool fixed_kib = true; ///< banked: 0000-03FF always page 0 (Sega); false: Codemasters
    unsigned bank_shift = 14; ///< 16 KiB pages; Korean 150-in-1 uses six 8 KiB slots (13)
    /// Without precise: a sync right after each store that may reach a mapper
    /// register (any write but the stack's), so that the code that follows is
    /// looked up under the mapping it wrote.  Korean 150-in-1: a register
    /// write moves every window, the running code included.
    bool sync_stores = false;
    bool mastersystem = false; ///< ports 3E/3F instead of the Game Gear 00-06 registers
    /// MSX: the ROM image handed to emit_code is addressed like the Z80 bus (bytes below
    /// rom_start are never code); ports 98/99 VDP, A0-A2 PSG, A8-AB PPI; RAM is the top
    /// msx_ram bytes of the address space; the BIOS (0000-3FFF) runs in the interpreter.
    bool msx = false;
    unsigned rom_start = 0;    ///< msx: address of the first ROM byte
    unsigned msx_ram = 0x2000; ///< msx: bytes of RAM at the top of the address space
    unsigned msx_init = 0;     ///< msx: INIT routine, a code entry
    unsigned msx_mapper = 0;   ///< msx: bank mapper (m0_msx_layout.h, 0: unbanked); with `banked`, 8 KiB units (bank_shift 13)
    std::string io_out = "sg1000_out", io_in = "sg1000_in";
    size_t max_instructions = 6000; ///< flash budget, in instructions
    size_t max_region_instructions = 200;
    std::vector<Loc> entries; ///< dynamic jump targets seen by profiling
    bool aggressive_scan = true;  ///< seed sweep-validated unreached courses (GB aggressive scan)
    bool infer_data_zones = true; ///< static code/data sweep of the ROM before seeding
    /// 8 KiB snapshot of C000-DFFF from a played session: code the game wrote
    /// in RAM compiles at its own addresses (locations stay the plain pc).
    /// A later session that rewrites that code would diverge — check the
    /// host's run_hash against --interp.
    std::vector<uint8_t> ram;
};

struct EmitStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    size_t native = 0, generic = 0;
    size_t idle = 0;       ///< JR $ / JP $ loops spent at once (SG_IDLE)
    size_t swept = 0;      ///< instructions added by the speculative sweep
    size_t data_zones = 0; ///< inferred data zones (>= 512 bytes)
    size_t data_bytes = 0; ///< ROM bytes inside those zones
    std::vector<uint32_t> locations; ///< every compiled instruction (Loc), ascending
};

/// Z80 -> C for the Sega Z80 runtimes (sg_gen.h): regions of native code
/// for the played (hot first) and statically reachable instructions.
std::string emit_code(std::span<const uint8_t> rom, const std::vector<HotLoc>& hot_first,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::sg1000
