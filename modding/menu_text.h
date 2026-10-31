#pragma once
#include <string_view>

namespace uc::mods {

// The mod manager's own strings (English and Russian; other languages show English), the same in the in-game menu and in the launchers.
// Keys are those of ModMenuUi ("allow_native", "none", "enabled", ...); nullptr for an unknown key.
const char* manager_text(std::string_view language, std::string_view key);

}  // namespace uc::mods
