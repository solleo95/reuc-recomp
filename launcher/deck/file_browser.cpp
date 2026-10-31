#include "file_browser.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
namespace uc::deck {
namespace fs = std::filesystem;
namespace {
bool before(const std::string& a, const std::string& b) {   // case-insensitive for ASCII; other bytes (UTF-8) in byte order
    const auto n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}
std::string utf8(const fs::path& path) { auto text = path.u8string(); return {text.begin(), text.end()}; }
bool directory(const fs::path& path) { std::error_code ec; return fs::is_directory(path, ec); }
}
bool is_image_file(const fs::path& path) {
    auto extension = utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return extension == ".iso" || extension == ".wbfs" || extension == ".ciso" || extension == ".wdf" || extension == ".wia";
}
FileBrowser::FileBrowser(fs::path start, PickMode mode, std::vector<fs::path> shortcuts) : shortcuts_(std::move(shortcuts)), mode_(mode) {
    start = fs::absolute(start).lexically_normal();
    while (!directory(start) && start.has_parent_path() && start.parent_path() != start) start = start.parent_path();
    if (!list(start)) list(start.root_path());   // an unreadable start: the top of the filesystem
}
bool FileBrowser::list(const fs::path& folder) {
    std::error_code ec;
    fs::directory_iterator it(folder, ec);
    if (ec) { error_ = utf8(folder) + ": " + ec.message(); return false; }
    if (!directory(folder)) { error_ = utf8(folder) + ": not a folder"; return false; }
    std::vector<BrowserEntry> folders, files;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) { error_ = utf8(folder) + ": " + ec.message(); return false; }
        const auto name = utf8(it->path().filename());
        if (name.empty() || name.front() == '.') continue;
        std::error_code status_error;
        if (it->is_directory(status_error)) folders.push_back({name, true});
        else if (mode_ == PickMode::image && it->is_regular_file(status_error) && is_image_file(it->path())) files.push_back({name, false});
    }
    const auto order = [](const BrowserEntry& a, const BrowserEntry& b) { return before(a.name, b.name); };
    std::sort(folders.begin(), folders.end(), order);
    std::sort(files.begin(), files.end(), order);
    entries_ = std::move(folders);
    entries_.insert(entries_.end(), files.begin(), files.end());
    current_ = folder;
    error_.clear();
    return true;
}
bool FileBrowser::enter(size_t index) {
    if (index >= entries_.size() || !entries_[index].directory) return false;
    return list(current_ / fs::u8path(entries_[index].name));
}
bool FileBrowser::up() {
    const auto parent = current_.parent_path();
    if (parent.empty() || parent == current_) return false;
    return list(parent);
}
bool FileBrowser::go(const fs::path& folder) { return list(fs::absolute(folder).lexically_normal()); }
std::optional<fs::path> FileBrowser::choice(size_t index) const {
    if (mode_ == PickMode::folder) return index == npos ? std::optional<fs::path>(current_) : std::nullopt;
    if (index >= entries_.size() || entries_[index].directory) return std::nullopt;
    return current_ / fs::u8path(entries_[index].name);
}
std::vector<fs::path> default_shortcuts(const fs::path& home, const fs::path& media_root, const std::optional<fs::path>& last_folder) {
    std::vector<fs::path> result;
    std::set<fs::path> seen;
    const auto add = [&](const fs::path& path) { if (directory(path) && seen.insert(path).second) result.push_back(path); };
    if (last_folder) add(*last_folder);
    add(home);
    std::vector<fs::path> mounts;
    std::error_code ec;
    for (fs::directory_iterator outer(media_root, ec), end; !ec && outer != end; outer.increment(ec)) {
        if (!outer->is_directory(ec) || outer->path().filename().string().front() == '.') continue;
        mounts.push_back(outer->path());
        std::error_code inner_error;
        for (fs::directory_iterator inner(outer->path(), inner_error); !inner_error && inner != end; inner.increment(inner_error))
            if (inner->is_directory(inner_error) && inner->path().filename().string().front() != '.') mounts.push_back(inner->path());
    }
    std::sort(mounts.begin(), mounts.end());
    for (const auto& mount : mounts) add(mount);
    return result;
}
std::optional<fs::path> load_last_folder(const fs::path& state_file) {
    std::ifstream in(state_file, std::ios::binary);
    if (!in) return std::nullopt;
    const auto json = nlohmann::json::parse(in, nullptr, false);
    if (!json.is_object()) return std::nullopt;
    const auto found = json.find("last_folder");
    if (found == json.end() || !found->is_string()) return std::nullopt;
    return fs::u8path(found->get<std::string>());
}
bool store_last_folder(const fs::path& state_file, const fs::path& folder) {
    nlohmann::json json = nlohmann::json::object();
    {
        std::ifstream in(state_file, std::ios::binary);
        if (in) {
            auto existing = nlohmann::json::parse(in, nullptr, false);
            if (existing.is_object()) json = std::move(existing);
        }
    }
    json["last_folder"] = utf8(folder);
    std::error_code ec;
    fs::create_directories(state_file.parent_path(), ec);
    const fs::path temporary = state_file.string() + ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << json.dump(2) << "\n";
        if (!out.good()) { fs::remove(temporary, ec); return false; }
    }
    fs::rename(temporary, state_file, ec);
    if (ec) { fs::remove(temporary, ec); return false; }
    return true;
}
}
