#pragma once
#include "graphics_effects.h"
#include "scene_lights.h"
#include "view_reconstruct.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
namespace uc {
// A shadow map of one directional light, fitted in view space (the palettes and the GX lights are already there).
// view_to_light maps a view-space point to the map: x and y in -1..1 over the box, z in 0..1 along the light's travel direction.
struct ShadowFit {
    std::array<std::array<float,4>,4> view_to_light;
    float extent;       // half size of the square box, in view units
    float depth_range;  // length of the box along the light, in view units
    float texel_world;  // size of one shadow texel, in view units
};
struct ShadowPreset { uint32_t resolution; uint32_t filter_taps; uint32_t blocker_taps; bool soft; };
inline ShadowPreset shadow_preset(EffectLevel level) {
    switch(level) {
    case EffectLevel::low: return {1024,4,0,false};
    case EffectLevel::medium: return {2048,9,0,false};
    case EffectLevel::high: return {2048,16,16,true};
    case EffectLevel::ultra: return {4096,24,24,true};
    case EffectLevel::off: break;
    }
    return {0,0,0,false};
}
// The box covers the camera frustum from the camera to `distance` along -z and reaches back toward the light by `distance` so casters behind the slice still
// land in the map. Its centre snaps to whole texels: the map does not shimmer when the camera moves.
inline std::optional<ShadowFit> fit_shadow(const gx::Projection& projection, const gx::CollectedLight& light, float distance, uint32_t resolution) {
    const float length=std::sqrt(light.position[0]*light.position[0]+light.position[1]*light.position[1]+light.position[2]*light.position[2]);
    if (length<1e-6f || resolution==0 || distance<=0.f) return std::nullopt;
    const std::array<float,3> to_light{light.position[0]/length,light.position[1]/length,light.position[2]/length};
    const std::array<float,3> forward{-to_light[0],-to_light[1],-to_light[2]};   // the direction the light travels
    const std::array<float,3> helper=std::abs(forward[1])<0.99f ? std::array<float,3>{0,1,0} : std::array<float,3>{1,0,0};
    std::array<float,3> right{helper[1]*forward[2]-helper[2]*forward[1],helper[2]*forward[0]-helper[0]*forward[2],helper[0]*forward[1]-helper[1]*forward[0]};
    const float right_length=std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    for(auto& v:right) v/=right_length;
    const std::array<float,3> up{forward[1]*right[2]-forward[2]*right[1],forward[2]*right[0]-forward[0]*right[2],forward[0]*right[1]-forward[1]*right[0]};
    const auto params=view_params(projection);
    float min_x=1e30f,max_x=-1e30f,min_y=1e30f,max_y=-1e30f,min_z=1e30f,max_z=-1e30f;
    const auto dot=[](const std::array<float,3>& a,const std::array<float,3>& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    for(const float z:{-0.1f,-distance}) for(const float ndc_x:{-1.f,1.f}) for(const float ndc_y:{-1.f,1.f}) {
        const std::array<float,3> corner{(ndc_x+params.x_offset)*-z/params.x_scale,(ndc_y+params.y_offset)*-z/params.y_scale,z};
        min_x=std::min(min_x,dot(corner,right)); max_x=std::max(max_x,dot(corner,right));
        min_y=std::min(min_y,dot(corner,up)); max_y=std::max(max_y,dot(corner,up));
        min_z=std::min(min_z,dot(corner,forward)); max_z=std::max(max_z,dot(corner,forward));
    }
    const float extent=std::max(std::max(max_x-min_x,max_y-min_y)*0.5f,1e-3f);
    const float texel=2.f*extent/float(resolution);
    const float centre_x=std::round((min_x+max_x)*0.5f/texel)*texel, centre_y=std::round((min_y+max_y)*0.5f/texel)*texel;
    const float z_min=min_z-distance, range=max_z-z_min;
    ShadowFit fit{};
    for(size_t i=0;i<3;++i) {
        fit.view_to_light[0][i]=right[i]/extent; fit.view_to_light[1][i]=up[i]/extent; fit.view_to_light[2][i]=forward[i]/range;
    }
    fit.view_to_light[0][3]=-centre_x/extent; fit.view_to_light[1][3]=-centre_y/extent; fit.view_to_light[2][3]=-z_min/range; fit.view_to_light[3]={0,0,0,1};
    fit.extent=extent; fit.depth_range=range; fit.texel_world=texel;
    return fit;
}
// Whether the shadow-casting light moved enough that last frame's map no longer matches (same kind, same direction within 1e-3 = unchanged).
inline bool light_changed(const std::optional<gx::CollectedLight>& a, const std::optional<gx::CollectedLight>& b) {
    if (a.has_value()!=b.has_value()) return true;
    if (!a) return false;
    if (a->kind!=b->kind) return true;
    const auto unit=[](const std::array<float,3>& v) { const float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); return std::array<float,3>{v[0]/std::max(l,1e-12f),v[1]/std::max(l,1e-12f),v[2]/std::max(l,1e-12f)}; };
    const auto da=unit(a->position), db=unit(b->position);
    return std::abs(da[0]-db[0])>1e-3f || std::abs(da[1]-db[1])>1e-3f || std::abs(da[2]-db[2])>1e-3f;
}
}
