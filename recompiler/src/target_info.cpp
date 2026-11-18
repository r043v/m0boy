#include "m0boy/target_info.h"

#include <array>

/* Set by recompiler/CMakeLists.txt from the VERSION files (targets/meta/VERSION,
 * targets/consoles/<console>/VERSION) so a firmware records which m0recomp
 * produced it; compile.sh and tools/meta-compiler read it back. */
#ifndef GBRECOMP_META_VERSION
#define GBRECOMP_META_VERSION "unknown"
#endif
#ifndef GBRECOMP_POKITTO_VERSION
#define GBRECOMP_POKITTO_VERSION "unknown"
#endif
#ifndef GBRECOMP_ESPBOY_VERSION
#define GBRECOMP_ESPBOY_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_PC_VERSION
#define GBRECOMP_PC_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_ODROID_VERSION
#define GBRECOMP_ODROID_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_AKA_VERSION
#define GBRECOMP_AKA_VERSION "0.1.0"
#endif

namespace m0boy::consoles {
namespace {
constexpr std::array table = {
    TargetInfo{TargetId::desktop, "", "GBL_TARGET_DESKTOP", "GBRECOMP_VERSION", "meta/VERSION",
               Storage::host_file},
    TargetInfo{TargetId::meta, "meta", "GBL_TARGET_META", "GBRECOMP_META_VERSION", "meta/VERSION",
               Storage::sd_card},
    TargetInfo{TargetId::pokitto, "pokitto", "GBL_TARGET_POKITTO", "GBRECOMP_POKITTO_VERSION",
               "consoles/pokitto/VERSION", Storage::sd_card},
    TargetInfo{TargetId::espboy, "espboy", "GBL_TARGET_ESPBOY", "GBRECOMP_ESPBOY_VERSION",
               "consoles/espboy/VERSION", Storage::flash_beside_app},
    TargetInfo{TargetId::pc, "pc", "GBL_TARGET_PC", "GBRECOMP_PC_VERSION", "consoles/pc/VERSION",
               Storage::host_file},
    TargetInfo{TargetId::odroid, "odroid", "GBL_TARGET_ODROID", "GBRECOMP_ODROID_VERSION",
               "consoles/odroid/VERSION", Storage::sd_card},
    TargetInfo{TargetId::aka, "aka", "GBL_TARGET_AKA", "GBRECOMP_AKA_VERSION",
               "consoles/aka/VERSION", Storage::sd_card},
};
}

const TargetInfo* find(TargetId id) {
    for (const auto& info : table) if (info.id == id) return &info;
    return nullptr;
}

std::string_view version(TargetId id) {
    switch (id) {
        case TargetId::desktop: return "unknown";
        case TargetId::meta: return GBRECOMP_META_VERSION;
        case TargetId::pokitto: return GBRECOMP_POKITTO_VERSION;
        case TargetId::espboy: return GBRECOMP_ESPBOY_VERSION;
        case TargetId::pc: return GBRECOMP_PC_VERSION;
        case TargetId::odroid: return GBRECOMP_ODROID_VERSION;
        case TargetId::aka: return GBRECOMP_AKA_VERSION;
    }
    return "unknown";
}
} // namespace m0boy::consoles
