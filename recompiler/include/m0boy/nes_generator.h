/* NES project generation: the interpreter runtime, the cartridge embedded
 * as data and, for the META, the console drivers shared with the other
 * frontends. */
#pragma once

#include "m0boy/target_info.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::nes {

struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;             /* project prefix, file-safe */
    std::filesystem::path output; /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: consoles/, common/) */
    std::string compiler;         /* the firmware compiler's path (compile.sh) */
    std::vector<std::filesystem::path> coverage; /* the harness's --coverage files */
    bool precise = false;         /* --meta-precise: the interpreter's slices, exactly */
    bool interpret = false;       /* no recompiled code (--nes-interp) */
    bool sweep = false;           /* --nes-sweep: the walk also seeds from the PRG's JSR/JMP targets */
    size_t code_budget_bytes = 0; /* 0: the target's default */
    unsigned bytes_per_instruction = 0; /* 0: the default estimate */
    unsigned frame_hz = 0;        /* --frame-hz: the build's display rate
                                   * (0: the frontend's default, 60); the
                                   * option menu switches 50/60 at run time */
};

struct GenerateStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o, GenerateStats& stats,
              std::string& error);

} // namespace m0boy::nes
