#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "m0boy/system.h"
#include "m0boy/target_info.h"

namespace m0boy::ngp {

std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);

/* Project generation, after the PCE generator's plan: the cartridge image
 * embedded as C data, the interpreter runtime, the desktop harness and the
 * PC's SDL2 window.  The TLCS-900H -> C emitter follows later; until then
 * every project interprets. */
struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;                     /* project prefix, file-safe */
    std::filesystem::path output;         /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: consoles/, common/) */
};

struct GenerateStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o,
              GenerateStats& stats, std::string& error);

} // namespace m0boy::ngp
