#include "gx_textures.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define UC_EXPORT __declspec(dllexport)
#else
#define UC_EXPORT __attribute__((visibility("default")))
#endif

// Single-level C boundary: Python owns every input/output buffer.
extern "C" UC_EXPORT int uc_decode_texture(uint32_t width, uint32_t height, uint32_t format,
    const std::byte* data, size_t size, uint32_t palette_format, const std::byte* palette,
    size_t palette_size, uint8_t* output, size_t output_size, char* error, size_t error_size) noexcept {
    try {
        if (!data || !output || format>255 || palette_format>2 || palette_size>32768 ||
            (palette_size && !palette) || (palette_size&1))
            throw std::invalid_argument("invalid texture decode buffers/format");
        gx::TextureDesc desc{width,height,static_cast<gx::TextureFormat>(format),1};
        if (gx::encoded_texture_size(desc)!=size || size_t(width)*height*4!=output_size)
            throw std::invalid_argument("texture decode buffer size mismatch");
        const auto pixels=gx::decode_texture(desc,{data,size},
            {static_cast<gx::PaletteFormat>(palette_format),{palette,palette_size}});
        std::copy(pixels.levels[0].rgba.begin(),pixels.levels[0].rgba.end(),output);
        return 0;
    } catch (const std::exception& caught) {
        if (error && error_size) std::snprintf(error,error_size,"%s",caught.what());
        return 1;
    } catch (...) {
        if (error && error_size) std::snprintf(error,error_size,"unknown texture decode failure");
        return 1;
    }
}
