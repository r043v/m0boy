#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "m0boy/system.h"

namespace m0boy::ngp {

std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);

} // namespace m0boy::ngp
