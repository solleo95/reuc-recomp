#pragma once
#include "registry.h"
#include <filesystem>
#include <string>
#include <vector>

namespace uc::mods {

struct SaveChoice {
    std::filesystem::path root;       // the folder the game should use; empty: the caller's own explicit root stays
    bool gameplay_mods_allowed = true;  // false: the modded saves could not be prepared, so mods that change gameplay must not run
    std::string warning;              // for the log, empty when nothing went wrong
};

// Which save folder a run uses (docs/modding/saves.md). While any active mod has touches_gameplay the game uses <exe>/saves-modded, created once as a
// copy of <exe>/saves (never refreshed afterwards); otherwise <exe>/saves. The vanilla folder is never written. If the copy cannot be made the vanilla
// root is returned with gameplay_mods_allowed = false and a warning. An explicit root (the test harness) is never redirected.
SaveChoice resolve_save_root(const std::filesystem::path& exe_dir, bool explicit_root_given, const std::vector<const ModEntry*>& active);

}  // namespace uc::mods
