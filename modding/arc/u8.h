#pragma once
// Wii U8 archives (the game's .arc files): read, change members, write. An archive that was not changed is written back byte for byte; a changed one keeps
// its node order, its names and the order of its members (docs/spikes/M6-arc-members.md).
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

class U8Archive {
public:
    static constexpr size_t kMaxArchiveBytes = 256u << 20, kMaxMemberBytes = 64u << 20, kMaxNodes = 100000, kMaxDepth = 64;

    // Validates everything it reads: sizes, node ranges and nesting, names inside the string pool, members inside the file.
    static std::optional<U8Archive> parse(std::span<const uint8_t> file, std::string& error);

    std::vector<uint8_t> write() const;

    // `path` is the path inside the archive ('/' separated, ASCII case-insensitive, no empty, "." or ".." parts). replace() needs an existing file.
    bool replace(std::string_view path, std::vector<uint8_t> data, std::string& error);
    // Creates missing folders and the file; true also when it replaced an existing file. Appended nodes go to the end of their folder.
    bool add_or_replace(std::string_view path, std::vector<uint8_t> data, std::string& error);
    bool contains(std::string_view path) const;   // a file of that path exists
    std::vector<std::string> files() const;       // every file, in node order

private:
    struct Node {
        bool dir = false;
        uint32_t name_off = 0;
        uint32_t parent = 0;          // folders: index of the enclosing folder
        uint32_t end = 0;             // folders: index one past the last node below it
        uint64_t order = 0;           // files: position of the member in the data area (original offset, or after all of them for new ones)
        std::vector<uint8_t> data;    // files
    };
    std::string name_of(const Node& node) const;
    // The archive after a change that adds `new_nodes` nodes and `new_names` pool bytes and changes the (aligned) member bytes by `data_delta` must stay
    // within kMaxArchiveBytes; checked before anything is changed, so a huge result is never built.
    bool fits(size_t new_nodes, size_t new_names, int64_t data_delta, std::string& error) const;
    int find_child(uint32_t dir, std::string_view name) const;   // -1: none
    uint32_t insert(uint32_t parent, std::string_view name, bool dir, std::vector<uint8_t> data);   // at the end of the parent's range; returns the new index
    static bool split_path(std::string_view path, std::vector<std::string_view>& parts, std::string& error);
    // Follows `parts` from the root as far as the archive has them: how many parts exist (`found`), the folder reached, and the final node when all exist.
    bool walk(const std::vector<std::string_view>& parts, size_t& found, uint32_t& folder, int& last, std::string& error) const;

    std::vector<Node> nodes_;       // node 0 is the root folder
    std::vector<uint8_t> pool_;     // the string pool: NUL-terminated names
    std::vector<uint8_t> original_; // the file as parsed
    uint32_t root_off_ = 0x20, data_off_ = 0;
    uint64_t next_order_ = 0;
    uint64_t data_bytes_ = 0;       // the members' sizes, each rounded up to 0x20
    bool dirty_ = false;
    size_t original_nodes_ = 0, original_pool_ = 0;
};

}  // namespace uc::mods
