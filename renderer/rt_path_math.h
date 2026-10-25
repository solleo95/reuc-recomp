#pragma once
#include "graphics_effects.h"
#include <algorithm>
#include <array>
#include <cstdint>
namespace uc {
// Path tracing of the scene's lighting (docs/superpowers/plans/2026-10-20-path-tracing-spike.md; shaders/rt_path.slang is the GPU side). The game has no light sources in the
// world, so light comes from emitters found in the picture: a draw whose mean colour on screen is bright shines with that colour. A path leaves a
// lit surface along a cosine-weighted direction, hits geometry through the hit table, and either ends in an emitter (adds throughput x emission), bounces with the hit's
// mean albedo as its reflectance, or leaves the scene / runs out of bounces (adds the dome `ambient`). Colours are the game's display-referred ones.
struct RtPathSettings {
    uint32_t divisor=2;      // the signal pixel covers divisor x divisor scene pixels
    uint32_t paths=1;        // paths per signal pixel per frame
    uint32_t bounces=3;      // surface hits a path may follow
    float reach=80.f;        // a ray's length in view units
    float emission=8.f;      // an emitter's radiance relative to the colour the game shows for it
    float max_radiance=2.f; // the most one path may add per channel: a path that reaches an emitter in one lucky ray would show as a firefly
    float ambient=1.2f;      // what a ray that leaves the scene (or a path out of bounces) sees: the dome (warm, kRtPathWarm); in a closed car most paths end here after bounces, hence the strength
    float key=0.8f;          // the synthetic overhead light (rt_shadow_math.h kRtOverheadDirection), a soft short shadow ray (warm as well)
    float key_reach=8.f;     // the overhead light's rays are blocked only within this many view units (as the shadow pass)
    uint32_t light_count=32; // up to this many emitters found on screen are sampled for direct light with shadow rays; 0: only the paths find emitters
    uint32_t ao_rays=2;     // short rays per signal pixel for the contact occlusion
    float ao_radius=3.5f;    // their length in view units (the game's scale: a seat is about 4 tall; rt_ao_math.h High)
    float ao_strength=0.7f;  // how dark a fully blocked surface gets
    float mix=0.65f;        // 1: the path-traced lighting replaces the game's on lit surfaces; 0: nothing changes (the rest keeps the game's mood)
    float threshold=0.55f;   // a surface shines when the mean colour the game shows for it is brighter than this
    bool operator==(const RtPathSettings&) const=default;
};
inline RtPathSettings rt_path_settings() { return {}; }
// The quality levels of the player's "ray tracing" option: high is the defaults above; low and medium trace fewer bounces, occlusion rays and emitters; ultra traces at full
// resolution with a longer path and more occlusion rays. The level also sets the characters' local shadows (graphics_effects.h effective_effects). off counts as high.
inline RtPathSettings rt_path_preset(EffectLevel level) {
    RtPathSettings settings;
    switch (level) {
    case EffectLevel::low: settings.bounces=2; settings.ao_rays=1; settings.light_count=8; break;
    case EffectLevel::medium: settings.bounces=3; settings.ao_rays=1; settings.light_count=16; break;
    case EffectLevel::ultra: settings.divisor=1; settings.bounces=4; settings.ao_rays=3; break;
    default: break;
    }
    return settings;
}
// Rays per scene pixel (the signal is traced at 1/divisor^2 of the pixels): the paths' bounces, the occlusion rays, the emitter's shadow ray and the overhead light's.
inline double rt_path_rays_per_pixel(const RtPathSettings& settings) { return (double(settings.paths)*settings.bounces+settings.ao_rays+(settings.light_count?1.0:0.0)+(settings.key>0.f?1.0:0.0))/(double(settings.divisor)*settings.divisor); }
// The colour of the dome and of the overhead light: the game's lamp-lit warmth (a neutral dome greyed the picture); the shader has the same constant.
constexpr std::array<float,3> kRtPathWarm{1.0f,0.86f,0.66f};
inline float rt_path_luminance(const std::array<float,3>& colour) { return 0.2126f*colour[0]+0.7152f*colour[1]+0.0722f*colour[2]; }
// The mean albedo of a record from the screen accumulation (the sums of 8-bit channels and the pixel count); `fallback` for a record that is not on screen this frame.
inline std::array<float,3> rt_path_mean_albedo(const std::array<uint32_t,3>& sums, uint32_t count, const std::array<float,3>& fallback={0.4f,0.4f,0.4f}) {
    if (!count) return fallback;
    const float scale=1.f/(255.f*float(count));
    return {float(sums[0])*scale,float(sums[1])*scale,float(sums[2])*scale};
}
// Whether a record's surface is a light source: the colour the game shows for it (its mean over the screen) is bright, whether the draw is lit or not (a lamp, a lit panel, a window).
inline bool rt_path_emits(const std::array<float,3>& shown, float threshold) { return rt_path_luminance(shown)>threshold; }
inline std::array<float,3> rt_path_clamp(const std::array<float,3>& radiance, float limit) {
    return {std::clamp(radiance[0],0.f,limit),std::clamp(radiance[1],0.f,limit),std::clamp(radiance[2],0.f,limit)};
}
// Direct light (docs/superpowers/plans/2026-10-20-path-tracing-spike.md): every bright record found on screen is a point-ish area light at the mean of its view positions,
// sampled by the pixels with a shadow ray. The accumulation walks a grid of kRtPathGridStep pixels, so a record's area is its samples x step^2 pixels, each pixel being
// rt_path_pixel_size view units wide at its depth (xScale from the view parameters).
constexpr uint32_t kRtPathMaxLights=32;
constexpr float kRtPathGridStep=4.f, kRtPathPositionScale=16.f;   // the position sums are fixed-point: view units x 16, signed, wrapping in the unsigned atomics
inline float rt_path_pixel_size(float depth, float x_scale, float viewport_width) { return 2.f*depth/(x_scale*viewport_width); }
inline float rt_path_emitter_area(uint32_t samples, float pixel_size) { return float(samples)*kRtPathGridStep*kRtPathGridStep*pixel_size*pixel_size; }
inline float rt_path_decode_position(int32_t sum, uint32_t samples) { return samples ? float(sum)/(kRtPathPositionScale*float(samples)) : 0.f; }
// The estimator's radiance (relative to the albedo) of an emitter of `area` seen at squared distance d2 under cosines at the surface and the light: L A cos cos / (pi d2),
// with the distance floored at min_d2 so that a surface next to the emitter stays finite.
inline float rt_path_nee(float radiance, float area, float cos_surface, float cos_light, float d2, float min_d2) {
    if (cos_surface<=0.f || cos_light<=0.f) return 0.f;
    return radiance*area*cos_surface*cos_light/(3.14159265f*std::max(d2,min_d2));
}
// The contact occlusion factor from the blocked fraction of the short rays: 1 open, 1 - strength fully blocked.
inline float rt_path_ao(float blocked_fraction, float strength) { return 1.f-std::clamp(strength,0.f,1.f)*std::clamp(blocked_fraction,0.f,1.f); }
// The small integers of the shader's push constants share words (the block is full at 128 bytes): divisor | frame << 8, and paths | bounces << 8 | ao rays << 16 | lights << 24.
struct RtPathPacked { uint32_t divisor_frame, counts; };
inline RtPathPacked rt_path_pack(const RtPathSettings& settings, uint32_t frame=0) {
    return {(settings.divisor&255u)|((frame&255u)<<8),(settings.paths&255u)|((settings.bounces&255u)<<8)|((settings.ao_rays&255u)<<16)|((std::min(settings.light_count,kRtPathMaxLights)&255u)<<24)};
}
// The lit surface's colour after the path tracer: the game's own colour (its textures, which the albedo target does not always carry) scaled by the gathered light relative
// to a reference white, within [kRtPathMinScale, kRtPathMaxScale]; `mix` blends the scale toward 1 and the game's fog keeps its colour (fog 1: unchanged).
// Positive gain rolls off toward white, preserving bright texture detail in the already lit SDR colour. The shader uses the same curve.
constexpr float kRtPathReference=0.45f, kRtPathMinScale=0.25f, kRtPathMaxScale=1.8f;
inline std::array<float,3> rt_path_compose(const std::array<float,3>& game, const std::array<float,3>& light, float mix, float fog) {
    const float weight=std::clamp(mix,0.f,1.f)*(1.f-std::clamp(fog,0.f,1.f));
    std::array<float,3> out{};
    for (int i=0;i<3;++i) {
        const float scale=std::clamp(light[i]/kRtPathReference,kRtPathMinScale,kRtPathMaxScale);
        const float gain=1.f+(scale-1.f)*weight;
        out[i]=game[i]*gain/(1.f+game[i]*std::max(gain-1.f,0.f));
    }
    return out;
}
}
