#include "dvdfs.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;
namespace uc {

static std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
// path::string() throws for names outside the ANSI code page (the game tree contains such names); keep UTF-8 instead.
static std::string utf8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

void DvdFs::walk(const fs::path& dir, const std::string& rel) {
    std::vector<fs::directory_entry> kids;
    for (const auto& k : fs::directory_iterator(dir)) kids.push_back(k);
    std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return lower(utf8(a.path().filename())) < lower(utf8(b.path().filename())); });
    for (const auto& k : kids) {
        std::string name = utf8(k.path().filename());
        std::string r = rel.empty() ? name : rel + "/" + name;
        bool is_dir = k.is_directory();
        e_.push_back({r, k.path(), is_dir, is_dir ? 0u : uint32_t(k.file_size())});
        index_[lower(r)] = int(e_.size()) - 1;
        if (is_dir) walk(k.path(), r);   // depth first, like an FST
    }
}

bool DvdFs::open_root(const fs::path& root) {
    e_.clear(); index_.clear();
    if (!fs::is_directory(root)) return false;
    e_.push_back({"", root, true, 0});
    index_[""] = 0;
    walk(root, "");
    return true;
}

size_t DvdFs::overlay(const fs::path& files_root) {
    if (e_.empty()) return 0;
    std::vector<std::pair<std::string, fs::path>> files;   // relative path (UTF-8, '/'), full path
    std::error_code error;
    for (fs::recursive_directory_iterator it(files_root, error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        const auto type = it->symlink_status(entry_error).type();
        if (type == fs::file_type::symlink || type == fs::file_type::junction) { it.disable_recursion_pending(); continue; }
        if (type != fs::file_type::regular || it->file_size(entry_error) > UINT32_MAX) continue;
        const auto rel = it->path().lexically_relative(files_root).generic_u8string();
        files.emplace_back(std::string(rel.begin(), rel.end()), it->path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
    size_t count = 0;
    for (const auto& [rel, full] : files) {
        std::error_code size_error;
        const auto measured = fs::file_size(full, size_error);
        if (size_error || measured > UINT32_MAX) continue;   // the file changed or vanished since the scan
        const uint32_t size = uint32_t(measured);
        if (const auto at = index_.find(lower(rel)); at != index_.end()) {
            Entry& entry = e_[size_t(at->second)];
            if (entry.dir) continue;
            entry.full = full; entry.size = size; ++count;
            continue;
        }
        for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {   // missing parent folders
            const std::string folder = rel.substr(0, slash);
            if (index_.count(lower(folder))) continue;
            e_.push_back({folder, files_root / fs::path(std::u8string(folder.begin(), folder.end())), true, 0});
            index_[lower(folder)] = int(e_.size()) - 1;
        }
        e_.push_back({rel, full, false, size});
        index_[lower(rel)] = int(e_.size()) - 1;
        ++count;
    }
    return count;
}

int DvdFs::entry_of(std::string path) const {
    for (char& c : path) if (c == '\\') c = '/';
    std::string norm;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part == "..") return -1;                       // never leave the root
        if (!part.empty() && part != ".") norm += (norm.empty() ? "" : "/") + part;
        i = j + 1;
    }
    auto it = index_.find(lower(norm));
    return it == index_.end() ? -1 : it->second;
}

int64_t DvdFs::read(int entry, void* dst, uint32_t len, uint32_t offset) const {
    if (!valid(entry) || e_[size_t(entry)].dir) return -1;
    const Entry& e = e_[size_t(entry)];
    if (len == 0 || offset >= e.size) return 0;
    uint32_t n = std::min<uint32_t>(len, e.size - offset);
    std::ifstream f(e.full, std::ios::binary);
    if (!f) return -1;
    f.seekg(offset);
    f.read(static_cast<char*>(dst), n);
    return int64_t(f.gcount());
}

}  // namespace uc
