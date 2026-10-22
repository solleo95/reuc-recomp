#pragma once
#include "gx_textures.h"
#include <atomic>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace gx {
uint64_t xxh64(std::span<const std::byte> data);
// Dolphin-style name of a texture without the "tex1_" prefix, the "_m" mip flag and the extension: "<w>x<h>_<hash>[_<palette hash>|_$]_<format>".
// `base` is the encoded base level; `wildcard` swaps the palette hash for "$" (an unknown runtime palette in the dump). Empty palette hash for non-palette formats.
std::string texture_key(const TextureDesc& desc, std::span<const std::byte> base, PaletteView palette, bool wildcard);
// Replacement textures: `<dir>/tex1_*.dds` (BC7, full mip chain, written by uc_texture_convert), indexed once by file name.
class TextureReplacements {
public:
    struct Found { std::filesystem::path path; size_t bytes; };
    // Returns how many files are indexed in total. A key that is already indexed keeps its file unless replace_existing (mod folders
    // loaded after the base folder, in load order). Safe against find() from other threads (mods are switched on and off while the game runs).
    size_t load_index(const std::filesystem::path& dir, bool replace_existing = false);
    void clear();
    // Changes whenever the table does: a texture cache that built textures from an older generation must drop them (a replacement may be gone or new).
    uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }
    bool empty() const { std::shared_lock lock(m_); return files_.empty(); }
    std::optional<Found> find(const TextureDesc& desc, std::span<const std::byte> encoded, PaletteView palette) const;
private:
    mutable std::shared_mutex m_;
    std::atomic<uint64_t> generation_{0};
    std::unordered_map<std::string, Found> files_;
};
TextureReplacements& texture_replacements();
// Reads a BC7 DDS (DX10 header, 2D, one layer) into owned levels; throws TextureError on anything else.
TexturePixels load_dds(const std::filesystem::path& path);
// Size of one BC7 mip level.
inline size_t bc7_level_size(uint32_t width, uint32_t height) { return size_t((width + 3) / 4) * ((height + 3) / 4) * 16; }
} // namespace gx
