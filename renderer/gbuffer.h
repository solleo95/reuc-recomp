#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace uc {
// The normal G-buffer: one RGBA16 unorm attachment next to colour, depth and motion (docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md).
//   r,g = octahedral view-space normal, b = GX fog factor (0 = clear, 1 = fully fogged), a = flags below. A texel of zeros is "no surface".
// shaders/gbuffer.slang writes the same encoding.
constexpr uint16_t kGBufferHasNormal=1, kGBufferLit=2, kGBufferTessellated=4, kGBufferReflective=8;   // the middle two are reserved for later effects
constexpr uint16_t kGBufferCutout=16;   // an alpha-tested draw (grates, nets, foliage): not a solid surface, neither shaded nor an occluder
struct GBufferTexel { std::array<float,3> normal; float fog; uint16_t flags; };
inline float gbuffer_sign(float value) { return value>=0.f ? 1.f : -1.f; }
inline uint16_t gbuffer_unorm(float value) { return uint16_t(std::lround(std::fmin(std::fmax(value,0.f),1.f)*65535.f)); }
// A zero normal ("no normal") encodes as zeros; otherwise `normal` must be unit length.
inline std::array<uint16_t,4> encode_gbuffer(std::array<float,3> normal, float fog, uint16_t flags) {
    uint16_t x=0, y=0;
    const float sum=std::fabs(normal[0])+std::fabs(normal[1])+std::fabs(normal[2]);
    if (sum>0.f) {
        float px=normal[0]/sum, py=normal[1]/sum;
        if (normal[2]<0.f) { const float ox=(1.f-std::fabs(py))*gbuffer_sign(px), oy=(1.f-std::fabs(px))*gbuffer_sign(py); px=ox; py=oy; }
        x=gbuffer_unorm(px*0.5f+0.5f); y=gbuffer_unorm(py*0.5f+0.5f);
    }
    return {x,y,gbuffer_unorm(fog),flags};
}
inline GBufferTexel decode_gbuffer(std::array<uint16_t,4> texel) {
    GBufferTexel result{{0,0,0},texel[2]/65535.f,texel[3]};
    if (!(texel[3]&kGBufferHasNormal)) return result;   // no normal was written: the octahedral channels carry nothing
    const float px=texel[0]/65535.f*2.f-1.f, py=texel[1]/65535.f*2.f-1.f;
    float x=px, y=py, z=1.f-std::fabs(px)-std::fabs(py);
    if (z<0.f) { x=(1.f-std::fabs(py))*gbuffer_sign(px); y=(1.f-std::fabs(px))*gbuffer_sign(py); }
    const float length=std::sqrt(x*x+y*y+z*z);
    result.normal={x/length,y/length,z/length};
    return result;
}
}
