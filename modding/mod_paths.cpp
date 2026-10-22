#include "mod_paths.h"
#include "manifest.h"
#include "mod_zip.h"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace uc::mods {

std::optional<fs::path> confine(const fs::path& root, std::string_view relative) {
    if (!is_safe_relative_path(relative)) return std::nullopt;
    const std::u8string text(reinterpret_cast<const char8_t*>(relative.data()), relative.size());
    const fs::path joined = root / fs::path(text);
    std::error_code error;
    const auto base = fs::weakly_canonical(root, error);
    if (error) return std::nullopt;
    const auto resolved = fs::weakly_canonical(joined, error);   // resolves junctions and symlinks of the existing part
    if (error) return std::nullopt;
    const auto [mismatch, unused] = std::mismatch(base.begin(), base.end(), resolved.begin(), resolved.end());
    (void)unused;
    if (mismatch != base.end()) return std::nullopt;
    return joined;
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return std::nullopt;
    return bytes;
}

std::vector<DiscoveredMod> find_mod_folders(const fs::path& mods_dir) {
    std::vector<DiscoveredMod> found;
    std::error_code error;
    for (fs::directory_iterator it(mods_dir, error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        const auto name = it->path().filename().u8string();
        if (name.empty() || name.front() == u8'.') continue;
        DiscoveredMod mod;
        mod.folder_name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        if (it->is_directory(entry_error)) {
            mod.root = it->path();
            if (!fs::exists(mod.root / "mod.json", entry_error)) continue;
        } else if (it->path().extension() == ".zip") {
            if (!extract_zip_mod(it->path(), mods_dir / ".cache", mod.root, mod.error)) { found.push_back(std::move(mod)); continue; }
            if (!fs::exists(mod.root / "mod.json", entry_error)) { mod.error = "the zip has no mod.json"; found.push_back(std::move(mod)); continue; }
        } else continue;
        mod.manifest_text = read_file(mod.root / "mod.json").value_or(std::string());
        if (mod.manifest_text.empty() && mod.error.empty()) mod.error = "mod.json is unreadable";
        found.push_back(std::move(mod));
    }
    std::sort(found.begin(), found.end(), [](const DiscoveredMod& a, const DiscoveredMod& b) { return a.folder_name < b.folder_name; });
    return found;
}

}  // namespace uc::mods
