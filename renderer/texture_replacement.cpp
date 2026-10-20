#include "texture_replacement.h"
#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <fstream>

namespace gx {
namespace {
constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull, P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
uint64_t read64(const std::byte* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
uint32_t read32(const std::byte* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t round64(uint64_t acc, uint64_t input) { return std::rotl(acc + input * P2, 31) * P1; }
uint64_t merge(uint64_t acc, uint64_t value) { return (acc ^ round64(0, value)) * P1 + P4; }
// Palette index range used by a base level, as the extractor hashes it (c4: nibbles, c8: bytes, c14x2: big-endian 14-bit words).
std::pair<uint32_t, uint32_t> index_range(TextureFormat format, std::span<const std::byte> base) {
    uint32_t low = ~0u, high = 0;
    auto note = [&](uint32_t v) { low = std::min(low, v); high = std::max(high, v); };
    if (format == TextureFormat::c4) for (auto b : base) { note(uint8_t(b) >> 4); note(uint8_t(b) & 15); }
    else if (format == TextureFormat::c8) for (auto b : base) note(uint8_t(b));
    else for (size_t i = 0; i + 1 < base.size(); i += 2) note(((uint32_t(uint8_t(base[i])) << 8) | uint8_t(base[i + 1])) & 0x3FFF);
    return {low, high};
}
std::string hex16(uint64_t v) { char text[17]; std::snprintf(text, sizeof text, "%016" PRIx64, v); return text; }
size_t base_size(const TextureDesc& desc) { TextureDesc one = desc; one.levels = 1; return encoded_texture_size(one); }
uint32_t le32(const std::vector<std::byte>& b, size_t at) { return read32(b.data() + at); }
} // namespace

uint64_t xxh64(std::span<const std::byte> data) {
    const std::byte* p = data.data(); const std::byte* const end = p + data.size();
    uint64_t h;
    if (data.size() >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        for (; p <= end - 32; p += 32) { v1 = round64(v1, read64(p)); v2 = round64(v2, read64(p + 8)); v3 = round64(v3, read64(p + 16)); v4 = round64(v4, read64(p + 24)); }
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else h = P5;
    h += data.size();
    for (; p + 8 <= end; p += 8) h = std::rotl(h ^ round64(0, read64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = std::rotl(h ^ (uint64_t(read32(p)) * P1), 23) * P2 + P3; p += 4; }
    for (; p < end; ++p) h = std::rotl(h ^ (uint64_t(uint8_t(*p)) * P5), 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3;
    return h ^ (h >> 32);
}

std::string texture_key(const TextureDesc& desc, std::span<const std::byte> base, PaletteView palette, bool wildcard) {
    std::string key = std::to_string(desc.width) + "x" + std::to_string(desc.height) + "_" + hex16(xxh64(base));
    const auto format = desc.format;
    if (format == TextureFormat::c4 || format == TextureFormat::c8 || format == TextureFormat::c14x2) {
        if (wildcard) key += "_$";
        else {
            const auto [low, high] = index_range(format, base);
            if (base.empty() || (size_t(high) + 1) * 2 > palette.bytes.size()) return {};   // the dump named such textures with "$"
            key += "_" + hex16(xxh64(palette.bytes.subspan(size_t(low) * 2, (size_t(high) + 1 - low) * 2)));
        }
    }
    return key + "_" + std::to_string(unsigned(format));
}

size_t TextureReplacements::load_index(const std::filesystem::path& dir) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(dir, error), last; !error && it != last; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (it->path().extension() != ".dds" || name.compare(0, 5, "tex1_") != 0) continue;
        std::string key = name.substr(5, name.size() - 9);
        // "_m" marks a mipmapped original; the hash covers the base level only, so both spellings map to the same replacement.
        if (const auto at = key.find("_m_"); at != std::string::npos && key.find('x') < at) key.erase(at, 2);
        std::error_code size_error;
        files_.emplace(std::move(key), Found{it->path(), size_t(it->file_size(size_error))});
    }
    return files_.size();
}

std::optional<TextureReplacements::Found> TextureReplacements::find(const TextureDesc& desc, std::span<const std::byte> encoded, PaletteView palette) const {
    if (files_.empty() || encoded.size() < base_size(desc)) return {};
    const auto base = encoded.first(base_size(desc));
    for (const bool wildcard : {false, true}) {
        const auto key = texture_key(desc, base, palette, wildcard);
        if (key.empty()) continue;
        if (const auto found = files_.find(key); found != files_.end()) return found->second;
    }
    return {};
}

TextureReplacements& texture_replacements() { static TextureReplacements instance; return instance; }

TexturePixels load_dds(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes;
    if (file) { file.seekg(0, std::ios::end); bytes.resize(size_t(file.tellg())); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())); }
    if (!file || bytes.size() < 148) throw TextureError(TextureErrorCode::truncated, "unreadable or truncated DDS replacement");
    constexpr uint32_t kDX10 = 0x30315844, kBc7 = 98;
    if (le32(bytes, 0) != 0x20534444 || le32(bytes, 4) != 124 || le32(bytes, 84) != kDX10 || le32(bytes, 128) != kBc7 || le32(bytes, 132) != 3 || le32(bytes, 140) != 1)
        throw TextureError(TextureErrorCode::unsupported, "DDS replacement is not a BC7 2D texture");
    uint32_t height = le32(bytes, 12), width = le32(bytes, 16), mips = std::max(1u, le32(bytes, 28));
    if (!width || !height || width > 16384 || height > 16384 || mips > 15 || mips > std::bit_width(std::max(width, height)))
        throw TextureError(TextureErrorCode::descriptor, "invalid DDS replacement extent or mip count");
    TexturePixels result; result.bc7 = true;
    size_t at = 148;
    for (uint32_t level = 0; level < mips; ++level) {
        const size_t size = bc7_level_size(width, height);
        if (bytes.size() - at < size) throw TextureError(TextureErrorCode::truncated, "truncated DDS replacement mip chain");
        TextureLevel mip{width, height, std::vector<uint8_t>(size)};
        std::memcpy(mip.rgba.data(), bytes.data() + at, size);
        result.levels.push_back(std::move(mip));
        at += size; width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    return result;
}
} // namespace gx
