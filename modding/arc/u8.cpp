#include "arc/u8.h"
#include <algorithm>
#include <cctype>

namespace uc::mods {
namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint64_t align32(uint64_t v) { return (v + 0x1F) & ~uint64_t(0x1F); }

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

std::optional<U8Archive> U8Archive::parse(std::span<const uint8_t> file, std::string& error) {
    const auto refuse = [&](const char* why) -> std::optional<U8Archive> { error = why; return std::nullopt; };
    if (file.size() < 0x20) return refuse("too short for a U8 archive");
    if (file.size() > kMaxArchiveBytes) return refuse("larger than the 256 MiB limit");
    if (be32(file.data()) != 0x55AA382D) return refuse("not a U8 archive (bad magic)");
    const uint64_t size = file.size();
    const uint32_t root_off = be32(&file[4]), tables = be32(&file[8]), data_off = be32(&file[12]);
    if (root_off < 0x20 || uint64_t(root_off) + tables > size || tables < 12) return refuse("the node and name tables lie outside the file");
    if (data_off < uint64_t(root_off) + tables || data_off > size) return refuse("the data area lies inside the tables or outside the file");
    if (file[root_off] != 1) return refuse("the first node is not a folder");
    const uint32_t count = be32(&file[root_off + 8]);
    if (count == 0 || count > kMaxNodes) return refuse("the node count is zero or above the 100000 limit");
    if (uint64_t(count) * 12 > tables) return refuse("the node table is longer than the tables");

    U8Archive archive;
    archive.original_.assign(file.begin(), file.end());
    archive.root_off_ = root_off;
    archive.data_off_ = data_off;
    const size_t pool_size = tables - size_t(count) * 12;
    const uint8_t* pool = &file[root_off + size_t(count) * 12];
    archive.pool_.assign(pool, pool + pool_size);
    archive.original_nodes_ = count;
    archive.original_pool_ = pool_size;
    archive.nodes_.resize(count);

    std::vector<uint32_t> open{0};   // folders whose range contains the current node
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* at = &file[root_off + size_t(i) * 12];
        Node& node = archive.nodes_[i];
        const uint8_t flag = at[0];
        node.name_off = (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | at[3];
        const uint32_t a = be32(at + 4), b = be32(at + 8);
        if (flag > 1) return refuse("a node has an unknown type");
        if (node.name_off >= pool_size) return refuse("a name lies outside the string pool");
        const auto end_of_name = std::find(archive.pool_.begin() + node.name_off, archive.pool_.end(), uint8_t(0));
        if (end_of_name == archive.pool_.end()) return refuse("a name is not terminated inside the string pool");
        if (i > 0) {
            const std::string name = archive.name_of(node);
            if (name.empty() || name.size() > 255 || name == "." || name == ".." || name.find('/') != std::string::npos) return refuse("a node has an unusable name");
            while (open.size() > 1 && i >= archive.nodes_[open.back()].end) open.pop_back();
            if (i >= archive.nodes_[open.back()].end) return refuse("the folder ranges do not nest");
        }
        node.dir = flag == 1;
        if (node.dir) {
            node.parent = i == 0 ? 0 : a;
            node.end = i == 0 ? count : b;
            if (i == 0) {
                if (b != count) return refuse("the root folder does not cover all nodes");
                continue;
            }
            if (a != open.back()) return refuse("a folder names the wrong parent");
            if (b <= i || b > archive.nodes_[open.back()].end) return refuse("a folder range leaves its parent or ends before it starts");
            open.push_back(i);
            if (open.size() > kMaxDepth) return refuse("folders are nested deeper than 64 levels");
        } else {
            if (a < data_off || uint64_t(a) + b > size) return refuse("a member lies outside the file");
            if (b > kMaxMemberBytes) return refuse("a member is larger than 64 MiB");
            node.order = a;
            node.data.assign(file.begin() + a, file.begin() + a + b);
        }
    }
    archive.next_order_ = uint64_t(size) + 1;
    return archive;
}

std::string U8Archive::name_of(const Node& node) const {
    return std::string(reinterpret_cast<const char*>(&pool_[node.name_off]));
}

int U8Archive::find_child(uint32_t dir, std::string_view name) const {
    for (uint32_t i = dir + 1; i < nodes_[dir].end; i = nodes_[i].dir ? nodes_[i].end : i + 1)
        if (same_name(name_of(nodes_[i]), name)) return int(i);
    return -1;
}

bool U8Archive::split_path(std::string_view path, std::vector<std::string_view>& parts, std::string& error) {
    parts.clear();
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.size() > 255 || part.find_first_of(std::string_view("\\\0", 2)) != std::string_view::npos) {
            error = "'" + std::string(path) + "' is not a usable path inside an archive";
            return false;
        }
        parts.push_back(part);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (parts.size() > kMaxDepth) { error = "'" + std::string(path) + "' is nested deeper than 64 levels"; return false; }
    return true;
}

bool U8Archive::walk(const std::vector<std::string_view>& parts, size_t& found, uint32_t& folder, int& last, std::string& error) const {
    found = 0;
    folder = 0;
    last = -1;
    for (size_t i = 0; i < parts.size(); ++i) {
        const int at = find_child(folder, parts[i]);
        if (at < 0) return true;
        const bool final_part = i + 1 == parts.size();
        if (!final_part && !nodes_[size_t(at)].dir) { error = "'" + std::string(parts[i]) + "' is a file, not a folder"; return false; }
        ++found;
        if (final_part) last = at;
        else folder = uint32_t(at);
    }
    return true;
}

uint32_t U8Archive::insert(uint32_t parent, std::string_view name, bool dir, std::vector<uint8_t> data) {
    const uint32_t p = nodes_[parent].end;
    std::vector<bool> ancestor(nodes_.size(), false);
    for (uint32_t c = parent;; c = nodes_[c].parent) {
        ancestor[c] = true;
        if (c == 0) break;
    }
    for (uint32_t j = 0; j < nodes_.size(); ++j) {
        Node& node = nodes_[j];
        if (!node.dir) continue;
        if (j != 0 && node.parent >= p) ++node.parent;
        if (ancestor[j] || node.end > p) ++node.end;
    }
    Node fresh;
    fresh.dir = dir;
    fresh.name_off = uint32_t(pool_.size());
    pool_.insert(pool_.end(), name.begin(), name.end());
    pool_.push_back(0);
    fresh.parent = parent;
    if (dir) fresh.end = p + 1;
    else { fresh.order = next_order_++; fresh.data = std::move(data); }
    nodes_.insert(nodes_.begin() + p, std::move(fresh));
    dirty_ = true;
    return p;
}

bool U8Archive::replace(std::string_view path, std::vector<uint8_t> data, std::string& error) {
    std::vector<std::string_view> parts;
    if (!split_path(path, parts, error)) return false;
    size_t found;
    uint32_t folder;
    int last;
    if (!walk(parts, found, folder, last, error)) return false;
    if (last < 0) { error = "the archive has no member '" + std::string(path) + "'"; return false; }
    if (nodes_[size_t(last)].dir) { error = "'" + std::string(path) + "' is a folder in the archive"; return false; }
    if (data.size() > kMaxMemberBytes) { error = "the replacement for '" + std::string(path) + "' is larger than 64 MiB"; return false; }
    nodes_[size_t(last)].data = std::move(data);
    dirty_ = true;
    return true;
}

bool U8Archive::add_or_replace(std::string_view path, std::vector<uint8_t> data, std::string& error) {
    std::vector<std::string_view> parts;
    if (!split_path(path, parts, error)) return false;
    size_t found;
    uint32_t folder;
    int last;
    if (!walk(parts, found, folder, last, error)) return false;
    if (data.size() > kMaxMemberBytes) { error = "'" + std::string(path) + "' is larger than 64 MiB"; return false; }
    if (last >= 0) {
        if (nodes_[size_t(last)].dir) { error = "'" + std::string(path) + "' is a folder in the archive"; return false; }
        nodes_[size_t(last)].data = std::move(data);
        dirty_ = true;
        return true;
    }
    const size_t missing = parts.size() - found;
    size_t names = 0;
    for (size_t i = found; i < parts.size(); ++i) names += parts[i].size() + 1;
    if (nodes_.size() + missing > kMaxNodes || pool_.size() + names > 0xFFFFFF) { error = "the archive has no room for '" + std::string(path) + "' (too many nodes or names)"; return false; }
    for (size_t i = found; i + 1 < parts.size(); ++i) folder = insert(folder, parts[i], true, {});
    insert(folder, parts.back(), false, std::move(data));
    return true;
}

bool U8Archive::contains(std::string_view path) const {
    std::vector<std::string_view> parts;
    std::string error;
    if (!split_path(path, parts, error)) return false;
    size_t found;
    uint32_t folder;
    int last;
    return walk(parts, found, folder, last, error) && last >= 0 && !nodes_[size_t(last)].dir;
}

std::vector<std::string> U8Archive::files() const {
    std::vector<std::string> out;
    std::vector<std::pair<uint32_t, std::string>> open;   // folder end, path prefix
    for (uint32_t i = 1; i < nodes_.size(); ++i) {
        while (!open.empty() && i >= open.back().first) open.pop_back();
        const std::string path = (open.empty() ? std::string() : open.back().second + "/") + name_of(nodes_[i]);
        if (nodes_[i].dir) open.emplace_back(nodes_[i].end, path);
        else out.push_back(path);
    }
    return out;
}

std::vector<uint8_t> U8Archive::write() const {
    if (!dirty_) return original_;
    const uint32_t count = uint32_t(nodes_.size());
    const uint64_t tables = uint64_t(count) * 12 + pool_.size();
    const bool tables_same = count == original_nodes_ && pool_.size() == original_pool_;
    const uint64_t data_off = tables_same ? data_off_ : align32(uint64_t(root_off_) + tables);
    std::vector<uint32_t> members;
    for (uint32_t i = 0; i < count; ++i) if (!nodes_[i].dir) members.push_back(i);
    std::stable_sort(members.begin(), members.end(), [&](uint32_t a, uint32_t b) { return nodes_[a].order < nodes_[b].order; });
    std::vector<uint64_t> offset(count, 0);
    uint64_t cursor = data_off;
    for (uint32_t i : members) {
        offset[i] = cursor;
        cursor += align32(nodes_[i].data.size());
    }
    std::vector<uint8_t> out(size_t(std::max<uint64_t>(cursor, data_off)), 0);
    std::copy(original_.begin(), original_.begin() + root_off_, out.begin());   // the header, with whatever its reserved bytes hold
    put32(&out[8], uint32_t(tables));
    put32(&out[12], uint32_t(data_off));
    for (uint32_t i = 0; i < count; ++i) {
        const Node& node = nodes_[i];
        uint8_t* at = &out[root_off_ + size_t(i) * 12];
        at[0] = node.dir ? 1 : 0;
        at[1] = uint8_t(node.name_off >> 16); at[2] = uint8_t(node.name_off >> 8); at[3] = uint8_t(node.name_off);
        if (node.dir) {
            put32(at + 4, i == 0 ? 0 : node.parent);
            put32(at + 8, node.end);
        } else {
            put32(at + 4, uint32_t(offset[i]));
            put32(at + 8, uint32_t(node.data.size()));
            std::copy(node.data.begin(), node.data.end(), out.begin() + std::ptrdiff_t(offset[i]));
        }
    }
    std::copy(pool_.begin(), pool_.end(), out.begin() + std::ptrdiff_t(root_off_ + size_t(count) * 12));
    return out;
}

}  // namespace uc::mods
