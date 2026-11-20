#pragma once
#include "m0boy/system.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::megadrive {
struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;
    std::filesystem::path output;
    std::filesystem::path targets_dir;
    std::string compiler = "gcc";
    std::vector<std::filesystem::path> coverage;
    bool rom_external = false; /* --rom-external (PC): the ROM is not embedded; the executable loads it at run time */
    bool sweep = false; /* --megadrive-sweep: the walk also seeds from the ROM's JSR/JMP abs.l targets */
};
bool generate(const std::filesystem::path &rom, const GenerateOptions &, std::string &error);
}
