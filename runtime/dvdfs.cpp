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
    std::unique_lock lock(m_);
    e_.clear(); index_.clear(); undo_.clear(); hidden_.clear();
    if (!fs::is_directory(root)) return false;
    e_.push_back({"", root, true, 0});
    index_[""] = 0;
    walk(root, "");
    return true;
}

size_t DvdFs::add_entry(Entry entry) {
    const std::string key = lower(entry.rel);
    size_t slot;
    const auto hidden = hidden_.find(key);
    if (hidden != hidden_.end() && e_[hidden->second].dir == entry.dir) {   // the same path (and kind) was added before and taken back: same number again
        slot = hidden->second;
        hidden_.erase(hidden);
        e_[slot] = std::move(entry);
    } else {   // (a path that comes back as another kind gets a new number: a held number never changes meaning)
        e_.push_back(std::move(entry));
        slot = e_.size() - 1;
    }
    index_[key] = int(slot);
    undo_.push_back({slot, true, {}});
    return slot;
}

void DvdFs::clear_locked() {
    for (size_t i = undo_.size(); i-- > 0;) {
        Undo& undo = undo_[i];
        if (undo.added) {
            Entry& entry = e_[undo.slot];
            const std::string key = lower(entry.rel);
            index_.erase(key);
            hidden_[key] = undo.slot;
            entry.hidden = true; entry.size = 0; entry.full.clear();   // the slot stays: a number the game holds must never point at another file
        } else {
            e_[undo.slot] = std::move(undo.original);
        }
    }
    undo_.clear();
}

void DvdFs::clear_overlays() {
    std::unique_lock lock(m_);
    clear_locked();
}

// The regular files below files_root (no symlinks or junctions): relative path (UTF-8, '/'), full path, size. Takes no lock: the scan can be slow.
std::vector<DvdFs::Found> DvdFs::scan(const fs::path& files_root) {
    std::vector<Found> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(files_root, error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        const auto type = it->symlink_status(entry_error).type();
        if (type == fs::file_type::symlink || type == fs::file_type::junction) { it.disable_recursion_pending(); continue; }
        if (type != fs::file_type::regular) continue;
        const auto measured = it->file_size(entry_error);
        if (entry_error || measured > UINT32_MAX) continue;
        const auto rel = it->path().lexically_relative(files_root).generic_u8string();
        files.push_back({std::string(rel.begin(), rel.end()), it->path(), uint32_t(measured)});
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return lower(a.rel) < lower(b.rel); });
    return files;
}

size_t DvdFs::apply_locked(const fs::path& files_root, const std::vector<Found>& files) {
    size_t count = 0;
    for (const auto& [rel, full, size] : files) {
        if (const auto at = index_.find(lower(rel)); at != index_.end()) {
            Entry& entry = e_[size_t(at->second)];
            if (entry.dir) continue;
            undo_.push_back({size_t(at->second), false, entry});
            entry.full = full; entry.size = size; ++count;
            continue;
        }
        for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {   // missing parent folders
            const std::string folder = rel.substr(0, slash);
            if (index_.count(lower(folder))) continue;
            add_entry({folder, files_root / fs::path(std::u8string(folder.begin(), folder.end())), true, 0});
        }
        add_entry({rel, full, false, size});
        ++count;
    }
    return count;
}

size_t DvdFs::overlay(const fs::path& files_root) {
    const auto files = scan(files_root);
    std::unique_lock lock(m_);
    if (e_.empty()) return 0;
    return apply_locked(files_root, files);
}

size_t DvdFs::set_overlays(const std::vector<fs::path>& files_roots) {
    std::vector<std::vector<Found>> scanned;
    for (const auto& root : files_roots) scanned.push_back(scan(root));
    std::unique_lock lock(m_);   // one swap: a reader sees the old overlays or the new ones, never the disc's own files in between
    clear_locked();
    if (e_.empty()) return 0;
    size_t count = 0;
    for (size_t i = 0; i < files_roots.size(); ++i) count += apply_locked(files_roots[i], scanned[i]);
    return count;
}

int DvdFs::entry_of(std::string path) const {
    std::shared_lock lock(m_);
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

std::optional<DvdFs::Snapshot> DvdFs::snapshot(int entry) const {
    std::shared_lock lock(m_);
    if (!valid(entry) || e_[size_t(entry)].dir) return std::nullopt;
    return Snapshot{entry, e_[size_t(entry)].full, e_[size_t(entry)].size};
}

int64_t DvdFs::read(const Snapshot& file, void* dst, uint32_t len, uint32_t offset) const {
    if (len == 0 || offset >= file.size) return 0;
    const uint32_t n = std::min<uint32_t>(len, file.size - offset);
    std::ifstream f(file.full, std::ios::binary);
    if (!f) return -1;
    f.seekg(offset);
    f.read(static_cast<char*>(dst), n);
    return int64_t(f.gcount());
}

int64_t DvdFs::read(int entry, void* dst, uint32_t len, uint32_t offset) const {
    std::shared_lock lock(m_);   // held across the file read: a swap of the overlays waits for the read in flight, so a read sees one whole version
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
