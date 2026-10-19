#pragma once
#include "graphics_effects.h"
#include "gx_materials.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
namespace uc {
// Presets of the ray-traced shadows (docs/superpowers/plans/2026-10-19-ray-tracing-p2.md, Task 7; resolution divisors are tuned in Task 11). divisor: the
// signal is traced at 1/divisor of the internal resolution per axis (0 = the effect is off).
struct RtShadowPreset { uint32_t divisor; uint32_t rays; bool soft; };      // directional lights: rays per pixel, a cone of kRtSunConeRadians when soft
struct RtLocalPreset { uint32_t divisor; uint32_t lights; bool soft; };     // point and spot lights: the strongest `lights` per pixel, a disc of kRtLightRadiusFraction when soft
inline RtShadowPreset rt_shadow_preset(EffectLevel level) {
    switch (level) {
        case EffectLevel::low: return {4,1,false};
        case EffectLevel::medium: return {2,1,false};
        case EffectLevel::high: return {2,1,true};
        case EffectLevel::ultra: return {1,2,true};
        default: return {0,0,false};
    }
}
inline RtLocalPreset rt_local_preset(EffectLevel level) {
    switch (level) {
        case EffectLevel::low: return {4,1,false};
        case EffectLevel::medium: return {2,2,false};
        case EffectLevel::high: return {2,4,true};
        case EffectLevel::ultra: return {1,8,true};
        default: return {0,0,false};
    }
}
constexpr float kRtSunConeRadians=0.02f;      // half angle of the sun's soft-shadow cone
constexpr float kRtLightRadiusFraction=0.03f; // a point light's soft-shadow disc radius as a fraction of the distance to it
// The factor that removes the shadowed lights' share from a pixel's light: 1 − Σ share_i·(1 − v_i), share_i = c_i / (ambient + Σ c).
// ambient and contributions are in the same unit (gx_light_contribution gives 0..1 per light, like the record's ambient colour).
inline float shadow_factor(float ambient_luminance, std::span<const float> contributions, std::span<const float> visibilities) {
    float total=ambient_luminance, removed=0.f;
    for (const float c:contributions) total+=c;
    if (total<=0.f) return 1.f;
    for (size_t i=0;i<contributions.size() && i<visibilities.size();++i) removed+=contributions[i]*(1.f-visibilities[i]);
    return 1.f-removed/total;
}
// One GX light's diffuse contribution to a surface point in luminance (0..1) as lightControl() in shaders/smoke.slang computes it for a channel with
// the given attenuation function (0 none, 1 specular, 3 distance/spot) and diffuse mode (2 = clamped N.L). The specular function is not a diffuse light.
inline float gx_light_contribution(const gx::LightUniform& light, uint32_t attenuation, uint32_t diffuse, std::array<float,3> position, std::array<float,3> normal) {
    if (attenuation==1) return 0.f;
    const float delta[3]={light.position[0]-position[0],light.position[1]-position[1],light.position[2]-position[2]};
    const float length_squared=delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2], distance=std::sqrt(length_squared);
    const float direction[3]={distance>0.f ? delta[0]/distance : normal[0],distance>0.f ? delta[1]/distance : normal[1],distance>0.f ? delta[2]/distance : normal[2]};
    float strength=1.f;
    if (attenuation==3) {
        const float angle=std::fmax(0.f,direction[0]*light.direction[0]+direction[1]*light.direction[1]+direction[2]*light.direction[2]);
        const float denominator=light.distance[0]+light.distance[1]*distance+light.distance[2]*length_squared;
        const float numerator=light.cosine[0]+light.cosine[1]*angle+light.cosine[2]*angle*angle;
        strength=denominator>0.f ? std::fmax(0.f,numerator)/denominator : 0.f;
    }
    if (diffuse!=0) {
        const float cosine=direction[0]*normal[0]+direction[1]*normal[1]+direction[2]*normal[2];
        strength*=diffuse==2 ? std::fmax(0.f,cosine) : cosine;
    }
    return strength*(0.299f*light.color[0]+0.587f*light.color[1]+0.114f*light.color[2])/255.f;
}
}
