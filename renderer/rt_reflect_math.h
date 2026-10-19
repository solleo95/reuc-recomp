#pragma once
#include "graphics_effects.h"
#include "rt_material.h"
#include "view_reconstruct.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace uc {
// Ray-traced reflections (docs/superpowers/plans/2026-10-20-ray-tracing-p5-reflections.md). divisor: the signal is traced at 1/divisor of the internal resolution per axis
// (0 = off); rays per pixel; cone: half angle of the roughness cone in radians (0 = a mirror); reach: the rays' length in view units.
struct RtReflectPreset { uint32_t divisor; uint32_t rays; float cone; float reach; };
inline RtReflectPreset rt_reflect_preset(EffectLevel level) {
    switch (level) {
        case EffectLevel::low: return {4,1,0.f,30.f};
        case EffectLevel::medium: return {2,1,0.f,45.f};
        case EffectLevel::high: return {2,1,0.05f,60.f};
        case EffectLevel::ultra: return {1,2,0.08f,80.f};
        default: return {0,0,0.f,0.f};
    }
}
// How reflective a draw is: the game's own gloss signal, the specular material colour of channel 1 (0 matte, 0.02 to 0.1 the common wood and metal, 1 the seat and
// wall class), half its square root so that faint gloss still shows; only when channel 1 is lit with the specular function.
inline float rt_reflectivity(const std::array<float,3>& specular_material, bool specular_lit) {
    if (!specular_lit) return 0.f;
    const float luminance=std::clamp(0.299f*specular_material[0]+0.587f*specular_material[1]+0.114f*specular_material[2],0.f,1.f);
    return 0.5f*std::sqrt(luminance);
}
// The reflectivity travels in bits 12 to 19 of the record flags (rt_material.h: the attenuation is in 8-9, the diffuse mode in 10-11); kRtReflective marks it non-zero.
constexpr uint32_t kRtReflectivityShift=12;
inline uint32_t rt_reflect_pack(float reflectivity, uint32_t flags) {
    const uint32_t value=uint32_t(std::lround(std::clamp(reflectivity,0.f,1.f)*255.f));
    return (flags&~(255u<<kRtReflectivityShift)&~kRtReflective)|(value<<kRtReflectivityShift)|(value ? kRtReflective : 0u);
}
inline float rt_reflect_unpack(uint32_t flags) { return float((flags>>kRtReflectivityShift)&255u)/255.f; }
// The composite's weight is fresnel x reflectivity x this: the game's gloss values are small (S12) and its scenes dark, so the plain product would show nothing.
constexpr float kRtReflectStrength=4.f;
constexpr float kRtFresnelF0=0.04f;
inline float rt_fresnel(float n_dot_v) { const float m=1.f-std::clamp(n_dot_v,0.f,1.f), m2=m*m; return kRtFresnelF0+(1.f-kRtFresnelF0)*m2*m2*m; }
inline std::array<float,3> rt_reflect(const std::array<float,3>& incident, const std::array<float,3>& normal) {
    const float d=incident[0]*normal[0]+incident[1]*normal[1]+incident[2]*normal[2];
    return {incident[0]-2.f*d*normal[0],incident[1]-2.f*d*normal[1],incident[2]-2.f*d*normal[2]};
}
// The pixel (in target pixels, fractional) and the stored depth (0..1 of the viewport's range) of a view-space position: the inverse of reconstruct_view.
struct RtProjected { bool valid; std::array<float,2> pixel; float depth; };
inline RtProjected rt_project(const ViewParams& view, const std::array<float,6>& viewport, const std::array<float,3>& p) {
    if (!(p[2]<-1e-4f)) return {false,{},0.f};
    const float ndc_x=view.x_scale*p[0]/-p[2]-view.x_offset, ndc_y=view.y_scale*p[1]/-p[2]-view.y_offset;
    const float depth01=(view.depth_a*p[2]+view.depth_b)/-p[2];   // clip.z / clip.w
    return {true,{viewport[0]+(ndc_x*0.5f+0.5f)*viewport[2],viewport[1]+(ndc_y*0.5f+0.5f)*viewport[3]},viewport[4]+depth01*(viewport[5]-viewport[4])};
}
// The frame shows the hit when the depth buffer's view distance at the hit's pixel is within 3% of the hit's own.
inline bool rt_hit_visible(float frame_distance, float hit_distance) { return std::fabs(frame_distance-hit_distance)<=0.03f*hit_distance; }
}
