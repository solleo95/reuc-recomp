#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include <memory>
#include <list>
#include <unordered_map>

namespace gx {
enum class TextureFormat : uint8_t { i4 = 0, i8 = 1, ia4 = 2, ia8 = 3, rgb565 = 4, rgb5a3 = 5, rgba8 = 6, c4 = 8, c8 = 9, c14x2 = 10, cmpr = 14 };
enum class PaletteFormat : uint8_t { ia8, rgb565, rgb5a3 };
struct PaletteView { PaletteFormat format = PaletteFormat::ia8; std::span<const std::byte> bytes; };
enum class TextureErrorCode { descriptor, unsupported, truncated };
struct TextureError : std::runtime_error {
    TextureError(TextureErrorCode code, const char* message) : std::runtime_error(message), code(code) {}
    TextureErrorCode code;
};
struct TextureDesc {
    uint32_t width = 0, height = 0;
    TextureFormat format = TextureFormat::rgba8;
    uint8_t levels = 1;
};
struct TextureLevel {
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};
enum class TextureChannels:uint8_t { rgba,alpha_intensity,rgb565 };
struct TexturePixels { std::vector<TextureLevel> levels; bool gpu_only=false; TextureChannels channels=TextureChannels::rgba; };

// Decodes texture data into owned RGBA8 pixels; guest addresses never escape.
TexturePixels decode_texture(const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette = {});
size_t encoded_texture_size(const TextureDesc& desc);
class TextureCache {
public:
    explicit TextureCache(size_t budget = 64 * 1024 * 1024) : budget_(budget) {}
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCache(TextureCache&&) = delete;
    TextureCache& operator=(TextureCache&&) = delete;
    std::shared_ptr<const TexturePixels> snapshot(uint32_t address, const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette = {});
    size_t resident_bytes() const { return resident_; }
    uint64_t decodes() const { return decodes_; }
    void clear() { by_address_.clear(); entries_.clear(); resident_ = 0; decodes_ = 0; }
private:
    struct Entry {
        uint32_t address;
        TextureDesc desc;
        PaletteFormat format;
        std::vector<std::byte> encoded, palette;
        std::shared_ptr<const TexturePixels> pixels;
        size_t bytes;
    };
    std::list<Entry> entries_;
    std::unordered_multimap<uint32_t, std::list<Entry>::iterator> by_address_;
    size_t budget_, resident_ = 0;
    uint64_t decodes_ = 0;
};
} // namespace gx
