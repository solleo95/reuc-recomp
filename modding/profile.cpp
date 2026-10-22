#include "profile.h"
#include "mod_paths.h"
#include <algorithm>

namespace fs = std::filesystem;

namespace uc::mods {

SaveChoice resolve_save_root(const fs::path& exe_dir, bool explicit_root_given, const std::vector<const ModEntry*>& active) {
    SaveChoice choice;
    if (explicit_root_given) return choice;
    const fs::path vanilla = exe_dir / "saves", modded = exe_dir / "saves-modded", partial = exe_dir / "saves-modded.partial";
    const bool gameplay = std::any_of(active.begin(), active.end(), [](const ModEntry* e) { return e->manifest.touches_gameplay; });
    choice.root = vanilla;
    if (!gameplay) return choice;

    std::error_code error;
    if (fs::is_directory(modded, error)) { choice.root = modded; return choice; }
    // Copy into a side folder and rename it into place, so a crash halfway never leaves a half-filled saves-modded that looks finished.
    fs::remove_all(partial, error);
    error.clear();
    fs::create_directories(partial, error);
    if (!error && fs::is_directory(vanilla)) fs::copy(vanilla, partial, fs::copy_options::recursive, error);
    if (!error && fs::exists(modded)) error = std::make_error_code(std::errc::file_exists);   // something that is not a folder is in the way
    if (!error) fs::rename(partial, modded, error);
    if (error) {
        std::error_code ignored;
        fs::remove_all(partial, ignored);
        choice.gameplay_mods_allowed = false;
        choice.warning = "cannot prepare " + utf8(modded) + " (" + error.message() + "): mods that change gameplay are not started, the normal saves stay untouched";
        return choice;
    }
    choice.root = modded;
    return choice;
}

}  // namespace uc::mods
