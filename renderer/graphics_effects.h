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
    bool contact_shadows=false;   // a short screen-space ray march toward the scene's dominant directional or spot light (G-buffer, scene targets)
    EffectLevel motion_blur=EffectLevel::off;   // per-object blur along the motion vectors after the upscale (shutter 0.3 / 0.5 / 0.7 / 0.7 with more samples)
    bool ray_tracing=false;   // master switch of the hardware ray tracing effects; each active RT effect replaces its raster counterpart
    bool ray_reconstruction=false;   // DLSS Ray Reconstruction denoises and upscales the ray-traced picture on DLSS frames (it needs the GPU, DLSS and an active RT effect); off: the own denoiser and DLSS Super Resolution
    bool path_tracing=false;   // the scene's lighting is path traced (several bounces, light from emissive surfaces) instead of the game's own; needs the ray tracing master switch
    EffectLevel rt_reflections=EffectLevel::off, rt_lighting=EffectLevel::off, rt_shadows=EffectLevel::off, rt_local_shadows=EffectLevel::off, rt_ambient_occlusion=EffectLevel::off;
    bool operator==(const GraphicsEffects&) const=default;
};
// The level an RT effect runs at: its chosen one while the master switch is on and the device supports ray tracing, else off.
inline EffectLevel rt_level(const GraphicsEffects& effects, EffectLevel level, bool rt_supported) { return effects.ray_tracing && rt_supported ? level : EffectLevel::off; }
inline bool any_rt_active(const GraphicsEffects& effects, bool rt_supported) {
    if(effects.ray_tracing && rt_supported && effects.path_tracing) return true;
    for(const auto level:{effects.rt_reflections,effects.rt_lighting,effects.rt_shadows,effects.rt_local_shadows,effects.rt_ambient_occlusion}) if(rt_level(effects,level,rt_supported)!=EffectLevel::off) return true;
    return false;
}
// The block every pass runs from: RT levels masked by the master switch and device support, and an active RT shadow or occlusion switches its raster
// counterpart off (RT replaces it, never stacks). The renderer keeps the chosen block for the config and derives this one.
inline GraphicsEffects effective_effects(const GraphicsEffects& chosen, bool rt_supported) {
    GraphicsEffects effective=chosen;
    effective.rt_reflections=rt_level(chosen,chosen.rt_reflections,rt_supported); effective.rt_lighting=rt_level(chosen,chosen.rt_lighting,rt_supported);
    effective.rt_shadows=rt_level(chosen,chosen.rt_shadows,rt_supported); effective.rt_local_shadows=rt_level(chosen,chosen.rt_local_shadows,rt_supported);
    effective.rt_ambient_occlusion=rt_level(chosen,chosen.rt_ambient_occlusion,rt_supported);
    effective.path_tracing=chosen.path_tracing && chosen.ray_tracing && rt_supported;
    if(effective.path_tracing) {   // path tracing lights the scene itself: sun shadows, occlusion and diffuse bounce are part of it (the reflections and the local shadows of the characters' rig lights stay their own passes)
        effective.rt_shadows=effective.rt_ambient_occlusion=effective.rt_lighting=EffectLevel::off;
        effective.shadows=EffectLevel::off; effective.contact_shadows=false; effective.ambient_occlusion=EffectLevel::off;
    }
    if(effective.rt_shadows!=EffectLevel::off) { effective.shadows=EffectLevel::off; effective.contact_shadows=false; }
    if(effective.rt_ambient_occlusion!=EffectLevel::off) effective.ambient_occlusion=EffectLevel::off;
    return effective;
}
// The screen-space effects run on the internal scene targets at the scene boundary; this says whether any of the chosen effects needs them
// (the renderer then keeps the scene-target path alive even without an upscaler). Takes the effective block (RT levels already masked).
inline bool needs_scene_targets(const GraphicsEffects& effects) { return effects.ambient_occlusion!=EffectLevel::off || effects.contact_shadows || effects.shadows!=EffectLevel::off || effects.motion_blur!=EffectLevel::off || any_rt_active(effects,true); }
// Bits of the per-draw push constant `effects` (smoke.slang Projection.effects).
constexpr uint32_t kEffectPerPixelLighting=1;
constexpr uint32_t kEffectRtMaterial=2;     // write the albedo and material-record targets (any RT effect active); the draw's record index rides in the push constant
constexpr uint32_t kEffectRtModelValid=4;   // the draw's record has kRtModelValid (rt_material.h): the G-buffer flags carry kGBufferModelValid
inline bool valid_anisotropy(uint32_t value) { return value==1 || value==2 || value==4 || value==8 || value==16; }
// The level a device runs: 1 without samplerAnisotropy, never above its maxSamplerAnisotropy.
inline uint32_t clamp_anisotropy(uint32_t requested, bool supported, float device_max) {
    if (!supported || device_max<1.f) return 1;
    return std::min(requested,uint32_t(device_max));
}
}
