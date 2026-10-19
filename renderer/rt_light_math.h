#pragma once
#include "graphics_effects.h"
#include "rt_ao_math.h"
#include <algorithm>
#include <cstdint>
namespace uc {
// Ray-traced lighting (docs/superpowers/plans/2026-10-20-ray-tracing-p6-lighting.md): one diffuse bounce. divisor, rays per pixel, the rays' reach in view units, the share
// of the bounced light that is added.
struct RtLightPreset { uint32_t divisor; uint32_t rays; float reach; float strength; };
inline RtLightPreset rt_light_preset(EffectLevel level) {
    switch (level) {
        case EffectLevel::low: return {4,1,8.f,0.5f};
        case EffectLevel::medium: return {2,1,12.f,0.6f};
        case EffectLevel::high: return {2,1,16.f,0.7f};
        case EffectLevel::ultra: return {1,2,24.f,0.7f};
        default: return {0,0,0.f,0.f};
    }
}
// The denoiser's history of the lighting signal: the bounce is lit from the picture itself, so it follows its surface's motion but not fast changes of the light.
constexpr uint32_t kRtLightHistory=16;
// One channel of the composite: the scene colour plus albedo x irradiance x strength, clamped to 1 (it only adds light).
inline float rt_light_add(float color, float albedo, float irradiance, float strength) { return std::min(color+albedo*std::max(irradiance,0.f)*strength,1.f); }
// How much a ray's hit counts: 1 close to the surface and away from the screen's edge, 0 at the reach and at the border of the screen (edge: 0 at the border, 1 from 8%
// of the screen inward); shaders/rt_reflect.slang does the same.
inline float rt_hit_confidence(float distance, float reach, float edge) { const float d=distance/reach; return std::clamp(edge,0.f,1.f)*std::max(1.f-d*d,0.f); }
}
