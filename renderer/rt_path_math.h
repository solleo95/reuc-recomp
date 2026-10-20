#pragma once
#include "rt_material.h"
#include <algorithm>
#include <array>
#include <cstdint>
namespace uc {
// Path tracing of the scene's lighting (docs/superpowers/plans/2026-10-20-path-tracing-spike.md; shaders/rt_path.slang is the GPU side). The game has no light sources in the
// world, so light comes from emitters found in the picture: an unlit draw (rt_material.h kRtEmissive) whose mean albedo is bright shines with that colour. A path leaves a
// lit surface along a cosine-weighted direction, hits geometry through the hit table, and either ends in an emitter (adds throughput x emission), bounces with the hit's
// mean albedo as its reflectance, or leaves the scene / runs out of bounces (adds the dome `ambient`). Colours are the game's display-referred ones.
struct RtPathSettings {
    uint32_t divisor=2;      // the signal pixel covers divisor x divisor scene pixels
    uint32_t paths=1;        // paths per signal pixel per frame
    uint32_t bounces=3;      // surface hits a path may follow
    float reach=80.f;        // a ray's length in view units
    float emission=6.f;      // an emitter's radiance relative to its albedo
    float ambient=0.12f;     // what a ray that leaves the scene (or a path out of bounces) sees: the dome
    float key=0.35f;         // the synthetic overhead light (rt_shadow_math.h kRtOverheadDirection), a short shadow ray
    float key_reach=8.f;     // the overhead light's rays are blocked only within this many view units (as the shadow pass)
    float mix=1.f;           // 1: the path-traced lighting replaces the game's on lit surfaces; 0: nothing changes
    float threshold=0.45f;   // an unlit surface shines when its mean albedo is brighter than this
};
inline RtPathSettings rt_path_settings() { return {}; }
inline float rt_path_luminance(const std::array<float,3>& colour) { return 0.2126f*colour[0]+0.7152f*colour[1]+0.0722f*colour[2]; }
// The mean albedo of a record from the screen accumulation (the sums of 8-bit channels and the pixel count); `fallback` for a record that is not on screen this frame.
inline std::array<float,3> rt_path_mean_albedo(const std::array<uint32_t,3>& sums, uint32_t count, const std::array<float,3>& fallback={0.4f,0.4f,0.4f}) {
    if (!count) return fallback;
    const float scale=1.f/(255.f*float(count));
    return {float(sums[0])*scale,float(sums[1])*scale,float(sums[2])*scale};
}
// Whether a record's surface is a light source: an unlit draw with a bright mean albedo.
inline bool rt_path_emits(uint32_t record_flags, const std::array<float,3>& albedo, float threshold) { return (record_flags&kRtEmissive) && rt_path_luminance(albedo)>threshold; }
// The lit surface's colour after the path tracer: its albedo times the gathered light, blended with the game's own colour by `mix`, and left to the game's fog (fog 1: unchanged).
inline std::array<float,3> rt_path_compose(const std::array<float,3>& game, const std::array<float,3>& albedo, const std::array<float,3>& light, float mix, float fog) {
    const float weight=std::clamp(mix,0.f,1.f)*(1.f-std::clamp(fog,0.f,1.f));
    std::array<float,3> out{};
    for (int i=0;i<3;++i) out[i]=game[i]+(std::min(albedo[i]*std::max(light[i],0.f),1.f)-game[i])*weight;
    return out;
}
}
