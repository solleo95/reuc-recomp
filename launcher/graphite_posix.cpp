#include "graphite.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <stdexcept>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace uc::launcher {
// Linux: the PNG is decoded with stb_image (the Windows launcher uses WIC) and uploaded as an RGBA texture.
SDL_Texture* load_banner(SDL_Renderer* renderer, const std::filesystem::path& path) {
    if (!renderer) throw std::runtime_error("Banner requires an SDL renderer");
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) throw std::runtime_error("Cannot read " + path.string() + ": " + stbi_failure_reason());
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    const bool uploaded = texture && SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
    stbi_image_free(pixels);
    if (!uploaded) {
        const std::string reason = SDL_GetError();
        SDL_DestroyTexture(texture);
        throw std::runtime_error("Cannot create banner texture: " + reason);
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}
}
