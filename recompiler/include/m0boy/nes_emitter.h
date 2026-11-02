/* 6502 -> C for the NES runtime (targets/nes/runtime/nes_gen.h). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::nes {

/* One line of a coverage file the harness writes (--coverage): a PRG
 * address, the bank context it ran under ("ADDR:BB", the switchable bank
 * mapped at the fetch; no colon, the fixed window — all of NROM), its
 * executions, and whether a dynamic transfer (RTS, RTI, JMP (ind), an
 * interrupt) landed on it. */
struct CoverageEntry {
    uint16_t pc = 0;
    uint32_t hits = 0;
    bool dynamic = false;
    uint8_t bank = 0xFF;
};

struct EmitOptions {
    uint32_t prg_size = 32768;   /* 16 or 32 KiB mirrored from $8000 (NROM),
                                  * or the banked size: $8000-$BFFF switches,
                                  * $C000-$FFFF the last 16 KiB (UxROM) */
    bool precise = false;        /* the slice ends after the same instruction as the interpreter's */
    size_t max_instructions = 0; /* 0: every reachable instruction */
    bool sweep = false;          /* --nes-sweep: the walk also seeds from the PRG's JSR/JMP targets */
    unsigned check_cycles = 48;  /* fast mode: the most cycles between two budget checks */
};

struct EmitStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

std::string emit_code(const std::vector<uint8_t>& prg, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::nes
