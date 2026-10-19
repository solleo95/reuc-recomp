#pragma once
#include "graphics_effects.h"
#include <array>
#include <cmath>
#include <cstdint>
namespace uc {
// Ray-traced ambient occlusion (docs/superpowers/plans/2026-10-20-ray-tracing-p4-ao.md): cosine-weighted hemisphere rays of a limited length, the share of them
// that hit something lowers the scene colour. divisor: the signal is traced at 1/divisor of the internal resolution per axis (0 = the effect is off); radius: the
// rays' length in view units (GTAO's is 0.5); strength: how much of the colour a fully blocked hemisphere removes.
struct RtAoPreset { uint32_t divisor; uint32_t rays; float radius; float strength; };
inline RtAoPreset rt_ao_preset(EffectLevel level) {
    switch (level) {
        case EffectLevel::low: return {2,1,0.35f,0.7f};
        case EffectLevel::medium: return {2,1,0.5f,0.75f};
        case EffectLevel::high: return {1,2,0.5f,0.8f};
        case EffectLevel::ultra: return {1,4,0.8f,0.8f};
        default: return {0,0,0.f,0.f};
    }
}
inline float rt_ao_factor(float blocked_fraction, float strength) { return 1.f-strength*blocked_fraction; }
// A direction in the hemisphere of the unit `normal`, cosine-weighted (u1, u2 in [0, 1)): shaders/rt_shadow.slang does the same arithmetic.
inline std::array<float,3> rt_ao_direction(const std::array<float,3>& normal, float u1, float u2) {
    const float radius=std::sqrt(u1), angle=6.2831853f*u2;
    const float x=radius*std::cos(angle), y=radius*std::sin(angle), z=std::sqrt(std::fmax(1.f-u1,0.f));
    // an orthonormal basis around the normal (Frisvad, with the sign trick of Duff et al.)
    const float sign=normal[2]>=0.f ? 1.f : -1.f, a=-1.f/(sign+normal[2]), b=normal[0]*normal[1]*a;
    const std::array<float,3> tangent{1.f+sign*normal[0]*normal[0]*a,sign*b,-sign*normal[0]}, bitangent{b,sign+normal[1]*normal[1]*a,-normal[1]};
    std::array<float,3> d{};
    for (size_t i=0;i<3;++i) d[i]=tangent[i]*x+bitangent[i]*y+normal[i]*z;
    const float length=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    for (float& v:d) v/=length;
    return d;
}
}
