#include "gx_textures.h"
#include "../runtime/gx_write_watch.h"
#include <algorithm>
#include <array>

namespace gx {
namespace {
using Pixel = std::array<uint8_t, 4>;
struct Tile { uint32_t width, height; size_t bytes; };
Tile tile_for(TextureFormat format) {
    switch (format) {
    case TextureFormat::i4: case TextureFormat::c4: case TextureFormat::cmpr: return {8,8,32};
    case TextureFormat::i8: case TextureFormat::c8: case TextureFormat::ia4: return {8,4,32};
    case TextureFormat::ia8: case TextureFormat::c14x2: case TextureFormat::rgb565: case TextureFormat::rgb5a3: return {4,4,32};
    case TextureFormat::rgba8: return {4,4,64};
    }
    throw TextureError(TextureErrorCode::unsupported, "unsupported GX texture format");
}
uint8_t expand5(unsigned v) { return uint8_t((v << 3) | (v >> 2)); }
uint8_t expand6(unsigned v) { return uint8_t((v << 2) | (v >> 4)); }
Pixel rgb565(uint16_t word) {
    return {expand5(word >> 11), expand6((word >> 5) & 63), expand5(word & 31), 255};
}
Pixel rgb5a3(uint16_t word) {
    if (word & 0x8000) return {expand5((word >> 10) & 31), expand5((word >> 5) & 31), expand5(word & 31),255};
    const unsigned a = word >> 12;
    return {uint8_t(((word >> 8) & 15) * 17), uint8_t(((word >> 4) & 15) * 17), uint8_t((word & 15) * 17), uint8_t((a << 5) | (a << 2) | (a >> 1))};
}
uint8_t byte(std::span<const std::byte> data, size_t offset) { return std::to_integer<uint8_t>(data[offset]); }
uint16_t word(std::span<const std::byte> data, size_t offset) { return uint16_t((unsigned(byte(data,offset)) << 8) | byte(data,offset+1)); }
Pixel texel(TextureFormat format, std::span<const std::byte> tile, uint32_t x, uint32_t y, PaletteView palette) {
    switch (format) {
    case TextureFormat::c4: case TextureFormat::c8: case TextureFormat::c14x2: {
        size_t index;
        if (format==TextureFormat::c4) index=(byte(tile,(y*8+x)/2)>>((x&1) ? 0 : 4))&15;
        else if (format==TextureFormat::c8) index=byte(tile,y*8+x);
        else index=word(tile,(y*4+x)*2)&0x3FFF;
        if (index>=palette.bytes.size()/2) throw TextureError(TextureErrorCode::truncated,"GX palette index exceeds loaded palette");
        const auto value=word(palette.bytes,index*2);
        switch (palette.format) {
        case PaletteFormat::ia8: { const auto i=uint8_t(value); return {i,i,i,uint8_t(value>>8)}; }
        case PaletteFormat::rgb565: return rgb565(value);
        case PaletteFormat::rgb5a3: return rgb5a3(value);
        }
        throw TextureError(TextureErrorCode::unsupported,"unsupported GX palette format");
    }
    case TextureFormat::i4: {
        const size_t index = y * 8 + x;
        const uint8_t v = uint8_t(((byte(tile,index/2) >> ((index & 1) ? 0 : 4)) & 15) * 17);
        return {v,v,v,v};
    }
    case TextureFormat::i8: {
        const uint8_t v = byte(tile,y*8+x);
        return {v,v,v,v};
    }
    case TextureFormat::ia4: {
        const uint8_t v = byte(tile,y*8+x), i = uint8_t((v & 15) * 17);
        return {i,i,i,uint8_t((v >> 4) * 17)};
    }
    case TextureFormat::ia8: {
        const size_t offset = (y*4+x)*2;
        const uint8_t i = byte(tile,offset+1);
        return {i,i,i,byte(tile,offset)};
    }
    case TextureFormat::rgb565: return rgb565(word(tile,(y*4+x)*2));
    case TextureFormat::rgb5a3: return rgb5a3(word(tile,(y*4+x)*2));
    case TextureFormat::rgba8: {
        const size_t offset = (y*4+x)*2;
        return {byte(tile,offset+1),byte(tile,offset+32),byte(tile,offset+33),byte(tile,offset)};
    }
    case TextureFormat::cmpr: {
        const size_t block = ((y/4)*2 + x/4)*8;
        const uint16_t a = word(tile,block), b = word(tile,block+2);
        const unsigned select = (byte(tile,block+4+y%4) >> (6-2*(x%4))) & 3;
        const Pixel first = rgb565(a), second = rgb565(b);
        if (select == 0) return first;
        if (select == 1) return second;
        Pixel out{0,0,0,255};
        for (size_t channel = 0; channel < 3; ++channel) {
            if (a > b) {
                const unsigned weight = select == 2 ? 5 : 3;
                out[channel] = uint8_t((first[channel]*weight + second[channel]*(8-weight))/8);
            } else out[channel] = uint8_t((unsigned(first[channel]) + second[channel])/2);
        }
        if (a <= b && select == 3) out[3] = 0;
        return out;
    }
    }
    throw TextureError(TextureErrorCode::unsupported, "unsupported GX texture format");
}
size_t level_size(uint32_t width, uint32_t height, Tile tile) {
    return size_t((width + tile.width - 1)/tile.width) * ((height + tile.height - 1)/tile.height) * tile.bytes;
}
} // namespace

size_t encoded_texture_size(const TextureDesc& desc) {
    // GX texture extent fields contain ten bits (dimension minus one).
    if (!desc.width || !desc.height || desc.width > 1024 || desc.height > 1024 || !desc.levels)
        throw TextureError(TextureErrorCode::descriptor, "invalid GX texture extent or mip count");
    const Tile tile = tile_for(desc.format);
    unsigned max_levels = 1;
    for (uint32_t dimension = std::max(desc.width,desc.height); dimension > 1; dimension >>= 1) ++max_levels;
    if (desc.levels > max_levels)
        throw TextureError(TextureErrorCode::descriptor, "GX mip chain extends past 1x1");
    uint32_t width = desc.width, height = desc.height;
    size_t required = 0;
    for (unsigned level = 0; level < desc.levels; ++level) {
        required += level_size(width,height,tile);
        width = std::max(1u,width/2); height = std::max(1u,height/2);
    }
    return required;
}
TexturePixels decode_texture(const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette) {
    const size_t required=encoded_texture_size(desc);
    if (pixels.size() < required)
        throw TextureError(TextureErrorCode::truncated, "truncated GX texture mip chain");
    const Tile tile=tile_for(desc.format);
    TexturePixels result;
    result.levels.reserve(desc.levels);
    uint32_t width = desc.width, height = desc.height;
    size_t offset = 0;
    for (unsigned level = 0; level < desc.levels; ++level) {
        TextureLevel output{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
        for (uint32_t top = 0; top < height; top += tile.height)
            for (uint32_t left = 0; left < width; left += tile.width) {
                const auto encoded = pixels.subspan(offset,tile.bytes);
                offset += tile.bytes;
                for (uint32_t y = 0; y < std::min(tile.height,height-top); ++y)
                    for (uint32_t x = 0; x < std::min(tile.width,width-left); ++x) {
                        const Pixel value = texel(desc.format,encoded,x,y,palette);
                        const size_t target = (size_t(top+y)*width+left+x)*4;
                        std::copy(value.begin(),value.end(),output.rgba.begin()+target);
                    }
            }
        result.levels.push_back(std::move(output));
        width = std::max(1u,width/2); height = std::max(1u,height/2);
    }
    return result;
}
std::shared_ptr<const TexturePixels> TextureCache::snapshot(uint32_t address, const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette) {
    const auto required = encoded_texture_size(desc);
    if (pixels.size() < required) throw TextureError(TextureErrorCode::truncated, "truncated GX texture mip chain");
    pixels = pixels.first(required);
    address &= 0x3FFFFFFF;
    if (watch_exhausted()) { watch_reset(); clear(); }
    const auto candidates = by_address_.equal_range(address);
    for (auto candidate = candidates.first; candidate != candidates.second; ++candidate) {
        const auto it = candidate->second;
        if (it->address == address && it->desc.width == desc.width && it->desc.height == desc.height &&
            it->desc.format == desc.format && it->desc.levels == desc.levels && it->format == palette.format &&
            same_bytes(it->palette, palette.bytes)) {
            // The pixel bytes are compared only when a guest store touched their pages since the last comparison (gx_write_watch.h), and anyway every
            // 256th use as a net for native writers that miss the barrier.
            if (!written_since(address, required, it->stamp) && it->skips < 256) ++it->skips;
            else if (same_bytes(it->encoded, pixels)) { it->stamp = watch_stamp(); it->skips = 0; }
            else continue;
            auto result = it->pixels;
            entries_.splice(entries_.begin(), entries_, it);
            return result;
        }
    }
    auto result = std::make_shared<const TexturePixels>(decode_texture(desc, pixels, palette));
    ++decodes_;
    size_t bytes = pixels.size() + palette.bytes.size();
    for (const auto& level : result->levels) bytes += level.rgba.size();
    if (bytes <= budget_) {
        while (!entries_.empty() && (resident_ > budget_ - bytes || entries_.size() >= 4096)) {
            auto last = entries_.end(); --last;
            const auto indexed = by_address_.equal_range(last->address);
            for (auto it = indexed.first; it != indexed.second; ++it) {
                if (it->second == last) { by_address_.erase(it); break; }
            }
            resident_ -= entries_.back().bytes; entries_.pop_back();
        }
        entries_.push_front({address, desc, palette.format, {pixels.begin(), pixels.end()}, {palette.bytes.begin(), palette.bytes.end()}, result, bytes, watch_stamp(), 0});
        try {
            by_address_.emplace(address, entries_.begin());
        } catch (...) {
            entries_.pop_front();
            throw;
        }
        resident_ += bytes;
    }
    return result;
}
} // namespace gx
