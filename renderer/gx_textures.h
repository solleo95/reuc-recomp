#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace gx {
enum class TextureFormat : uint8_t { i4 = 0, i8 = 1, ia4 = 2, ia8 = 3, rgb565 = 4, rgb5a3 = 5, rgba8 = 6, cmpr = 14 };
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
struct TexturePixels { std::vector<TextureLevel> levels; };

// Decodes texture data into owned RGBA8 pixels; guest addresses never escape.
TexturePixels decode_texture(const TextureDesc& desc, std::span<const std::byte> pixels);
size_t encoded_texture_size(const TextureDesc& desc);
} // namespace gx
