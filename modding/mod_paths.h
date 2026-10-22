#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

// root / relative (UTF-8, '/' separators), or nullopt when the result could leave root: "..", absolute, drive, UNC, stream colon,
// or a junction/symlink inside root that points outside it. The target need not exist.
std::optional<std::filesystem::path> confine(const std::filesystem::path& root, std::string_view relative);

// The path as UTF-8 text for messages. path::string() throws for names the system code page cannot hold.
inline std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Whole file as bytes; nullopt when it cannot be opened. Works for non-ASCII paths.
std::optional<std::string> read_file(const std::filesystem::path& path);

struct DiscoveredMod {
    std::filesystem::path root;
    std::string manifest_text;   // empty when mod.json is unreadable
    std::string folder_name;     // the folder, or the .zip file name
    std::string error;           // why a zip could not be extracted or mod.json is missing from it
};

// Direct child folders of mods_dir that hold a mod.json entry, plus *.zip files (extracted under mods_dir/.cache), sorted by name;
// folders starting with '.' are skipped. A missing mods_dir gives an empty list.
std::vector<DiscoveredMod> find_mod_folders(const std::filesystem::path& mods_dir);

}  // namespace uc::mods
