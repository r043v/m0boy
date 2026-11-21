#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "m0boy/system.h"
#include "m0boy/target_info.h"

namespace m0boy::ngp {

std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
/* the flash image, the loader's padding prefix stripped when the header sits at 0x20100 */
std::string load_flash(const std::filesystem::path&, std::string& error);

/* Project generation, after the PCE generator's plan: the cartridge image
 * embedded as C data, the interpreter runtime, the TLCS-900H translated
 * code (ngp_gen.c, unless interpret), the desktop harness and the PC's
 * SDL2 window. */
struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;                     /* project prefix, file-safe */
    std::filesystem::path output;         /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: consoles/, common/) */
    bool interpret = false;               /* --interp: no translated code, the interpreter only */
    size_t code_budget_bytes = 0;         /* --meta-code-budget: flash for ngp_gen.c (0: no cap) */
};

struct GenerateStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o,
              GenerateStats& stats, std::string& error);

} // namespace m0boy::ngp
