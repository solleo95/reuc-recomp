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
// What a light of a record is to the shadow pass (docs/superpowers/specs/2026-10-19-rt-level-shadows-design.md): a lamp (point or spot: a physical light of the
// level, traced against the whole scene under rt_local_shadows), a rig (a directional fill light that follows a character, traced against characters only under
// rt_shadows) or specular (the specular function of a channel: its share is never removed). `attenuation` is the channel's function from the record flags.
enum class RtLightClass { lamp, rig, specular };
inline RtLightClass rt_light_class(const gx::LightUniform& light, uint32_t attenuation) {
    if (attenuation==1) return RtLightClass::specular;
    const float x=light.position[0], y=light.position[1], z=light.position[2];
    return x*x+y*y+z*z>1e10f ? RtLightClass::rig : RtLightClass::lamp;   // a position beyond 1e5 units stands for a direction (scene_lights.cpp classify_light)
}
// A lamp's shadow ray ends this many view units before the lamp: the lamp sits inside its own fixture (a shade, a chandelier, a sconce on the wall) which
// would block every ray to it. View units are the game's (one unit is about 0.4 m: the cars' lamps hang 5 units above the eye). Tuned on the first scene.
constexpr float kRtLampClearance=1.0f;
inline float rt_lamp_tmax(float distance) { return std::fmax(distance-kRtLampClearance,0.f); }   // 0: the surface is inside the clearance, no ray
// The game's point lights are camera-relative: in the first scene's first seconds (the camera travels down the car) every lamp keeps its view-space z (-42.7, +70.7,
// -72.1, +177.7: the chandelier's light stays 42.7 units ahead while the chandelier model comes closer) and only wobbles in x and y with the camera's sway.
// Rays toward them make every shadow they cast swim with the camera, so a point light is not traced (the pass traces lamps only when a test asks for it).
// Level shadows come from a synthetic key light instead: a direction fixed in view space, well off the camera's axis (right, above and a little toward the
// camera: a light near the axis sees what the camera sees and casts no visible shadow), so a camera translation does not move its shadows (a rotation does). It lowers the lit colour by kRtOverheadStrength where it is
// blocked (a softened shadow: the game's own shading is baked into the colour).
constexpr std::array<float,3> kRtOverheadDirection{0.6189f,0.7220f,0.3094f};   // (0.6, 0.7, 0.3) normalised: toward the light
constexpr float kRtOverheadStrength=0.65f;
// The light sits inside the room, not above its roof: a ray longer than this (view units) is not blocked, or the ceiling would shadow everything (a seat top is 7 units
// below the car's roof). What blocks within the reach: luggage racks, seat backs, tables, characters, the chandelier.
constexpr float kRtOverheadReach=8.0f;   // 5.8 units of height at the light's elevation, short of the roof
constexpr float kRtOverheadConeRadians=0.06f; // half angle of its soft-shadow cone
// The key light stands still in the world, so its view-space direction follows the camera's rotation: `view_to_prev` is MotionTracker::view_to_prev_view() (the
// previous view from the current one, rows of a 3x4); a direction d_prev of the previous frame is d_cur = R^T d_prev with R its rotation. A turn of more than ~10 degrees in
// one frame is a camera cut (or a wrong estimate): back to the default direction.
inline std::array<float,3> rt_key_direction_after(const std::array<float,3>& previous, const std::array<std::array<float,4>,3>& view_to_prev) {
    std::array<float,3> now{};
    for (size_t i=0;i<3;++i) now[i]=view_to_prev[0][i]*previous[0]+view_to_prev[1][i]*previous[1]+view_to_prev[2][i]*previous[2];
    const float length=std::sqrt(now[0]*now[0]+now[1]*now[1]+now[2]*now[2]);
    if (!(length>0.5f)) return kRtOverheadDirection;
    for (float& v:now) v/=length;
    const float cosine=now[0]*previous[0]+now[1]*previous[1]+now[2]*previous[2];
    return cosine<0.985f ? kRtOverheadDirection : now;
}
// A surface faces the overhead light when the cosine between its normal and the direction is above ~0.05, fully from ~0.3.
inline float rt_overhead_facing(float n_dot_l) { const float t=std::fmin(std::fmax((n_dot_l-0.05f)/0.25f,0.f),1.f); return t*t*(3.f-2.f*t); }
inline float rt_overhead_factor(float blocked_fraction, float facing) { return 1.f-kRtOverheadStrength*blocked_fraction*facing; }
constexpr float kRtSunConeRadians=0.02f;      // half angle of the sun's soft-shadow cone
constexpr float kRtLightRadiusFraction=0.03f; // a point light's soft-shadow disc radius as a fraction of the distance to it
// The factor that removes the shadowed lights' contribution from a pixel's light: GX clamps ambient + lights at 1 before it multiplies the material
// (smoke.slang lightControl), so the lit colour scales by min(total − removed, 1) / min(total, 1), total = ambient + Σ c_i, removed = Σ c_i·(1 − v_i).
// ambient and contributions are in the same unit (gx_light_contribution gives 0..1 per light, like the record's ambient colour); a negative contribution counts as none.
inline float shadow_factor(float ambient_luminance, std::span<const float> contributions, std::span<const float> visibilities) {
    float total=std::fmax(ambient_luminance,0.f), removed=0.f;
    for (size_t i=0;i<contributions.size();++i) {
        const float c=std::fmax(contributions[i],0.f);
        total+=c;
        if (i<visibilities.size()) removed+=c*(1.f-visibilities[i]);
    }
    if (total<=0.f) return 1.f;
    return std::fmin(total-removed,1.f)/std::fmin(total,1.f);
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
    return std::fmax(strength,0.f)*(0.299f*light.color[0]+0.587f*light.color[1]+0.114f*light.color[2])/255.f;   // signed diffuse (mode 1) can go negative: that is no light to remove
}
}
