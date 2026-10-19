#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
namespace uc {
// Quality presets of the effects that have them (config graphics.*, launcher, menu).
enum class EffectLevel { off, low, medium, high, ultra };
inline std::string_view effect_level_name(EffectLevel level) {
    switch(level) { case EffectLevel::off: return "off"; case EffectLevel::low: return "low"; case EffectLevel::medium: return "medium"; case EffectLevel::high: return "high"; case EffectLevel::ultra: return "ultra"; }
    return {};
}
inline std::optional<EffectLevel> effect_level_from_name(std::string_view name) {
    for(const auto level:{EffectLevel::off,EffectLevel::low,EffectLevel::medium,EffectLevel::high,EffectLevel::ultra}) if(name==effect_level_name(level)) return level;
    return std::nullopt;
}
// The graphics options that change the picture beyond what the game draws (config graphics.*, launcher, in-game menu). Every effect has its own
// field; each later feature adds one. All off with anisotropy 1 is the picture of the original renderer.
struct GraphicsEffects {
    uint32_t anisotropy=16;   // 1, 2, 4, 8 or 16: anisotropic filtering of mipmapped scene textures (clamped to the device)
    bool per_pixel_lighting=false;   // the GX lighting channels are evaluated per pixel instead of per vertex (needs the motion shader variants)
    EffectLevel ambient_occlusion=EffectLevel::off;   // GTAO on the scene targets (needs the G-buffer and the scene-target path)
    EffectLevel shadows=EffectLevel::off;   // a shadow map of the scene's dominant directional light, drawn inside the scene packets (docs/superpowers/plans/2026-10-19-graphics-enhancements-p4-6.md)
    EffectLevel screen_space_shadows=EffectLevel::off;   // a screen-space ray march toward the scene's dominant directional or spot light: low is a contact ray, higher levels reach further (G-buffer, scene targets)
    EffectLevel motion_blur=EffectLevel::off;   // per-object blur along the motion vectors after the upscale (shutter 0.3 / 0.5 / 0.7 / 0.7 with more samples)
    bool operator==(const GraphicsEffects&) const=default;
};
// The screen-space effects run on the internal scene targets at the scene boundary; this says whether any of the chosen effects needs them
// (the renderer then keeps the scene-target path alive even without an upscaler). Later effects add their conditions here.
inline bool needs_scene_targets(const GraphicsEffects& effects) { return effects.ambient_occlusion!=EffectLevel::off || effects.screen_space_shadows!=EffectLevel::off || effects.shadows!=EffectLevel::off || effects.motion_blur!=EffectLevel::off; }
// Bits of the per-draw push constant `effects` (smoke.slang Projection.effects).
constexpr uint32_t kEffectPerPixelLighting=1;
inline bool valid_anisotropy(uint32_t value) { return value==1 || value==2 || value==4 || value==8 || value==16; }
// The level a device runs: 1 without samplerAnisotropy, never above its maxSamplerAnisotropy.
inline uint32_t clamp_anisotropy(uint32_t requested, bool supported, float device_max) {
    if (!supported || device_max<1.f) return 1;
    return std::min(requested,uint32_t(device_max));
}
}
