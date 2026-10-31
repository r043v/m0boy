/* 65C816 -> C for the SNES runtime (targets/snes/runtime/snes_gen.h). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::snes {

/* One line of a coverage file the harness writes (--coverage): a 24-bit
 * address (K:PC), its executions, the contexts it ran under — K, DBR, the
 * direct page and the M/X/E flags packed, K lowest, ten hex digits — and
 * whether a dynamic transfer (RTS, RTL, RTI, a jump through a pointer, an
 * interrupt) landed on it.  Bank and flag changes give one address several
 * contexts; the recompiler keys its instances by both. */
struct CoverageEntry {
    uint32_t pc = 0; /* K:PC */
    uint32_t hits = 0;
    bool dynamic = false;
    std::vector<uint64_t> contexts;
};

struct EmitOptions {
    uint32_t rom_bytes = 0x8000;
    bool hirom = false;          /* the cartridge's map */
    bool exhirom = false;        /* the ExHiROM map: banks $C0-$FF at the file's start */
    bool superfx = false;        /* a SuperFX attached: $6000-$7FFF is its register window */
    bool dsp1 = false;           /* a DSP-1 attached: its window overlaps the ROM banks */
    bool precise = false;        /* the slice ends after the same instruction as the interpreter's */
    size_t max_instructions = 0; /* 0: every reachable instruction */
    unsigned check_cycles = 48;  /* fast mode: the most cycles between two budget checks */
};

struct EmitStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

std::string emit_code(const std::vector<uint8_t>& rom, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::snes
