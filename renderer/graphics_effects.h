#pragma once
#include <algorithm>
#include <cstdint>
namespace uc {
// The graphics options that change the picture beyond what the game draws (config graphics.*, launcher, in-game menu). Every effect has its own
// field; each later feature adds one. All off with anisotropy 1 is the picture of the original renderer.
struct GraphicsEffects {
    uint32_t anisotropy=16;   // 1, 2, 4, 8 or 16: anisotropic filtering of mipmapped scene textures (clamped to the device)
    bool operator==(const GraphicsEffects&) const=default;
};
inline bool valid_anisotropy(uint32_t value) { return value==1 || value==2 || value==4 || value==8 || value==16; }
// The level a device runs: 1 without samplerAnisotropy, never above its maxSamplerAnisotropy.
inline uint32_t clamp_anisotropy(uint32_t requested, bool supported, float device_max) {
    if (!supported || device_max<1.f) return 1;
    return std::min(requested,uint32_t(device_max));
}
}
