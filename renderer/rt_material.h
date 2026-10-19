#pragma once
#include "gx_materials.h"
#include "scene_lights.h"
#include <array>
#include <cstdint>
namespace uc {
// The material record of a depth-writing scene draw while an RT effect is active (docs/superpowers/specs/2026-10-19-ray-tracing-design.md, "How it
// renders" 1, 5, 6). The scene pass writes its index per pixel (scene_material_, ~0u = none); RT passes read the record and the draw's 8 GX lights at
// lights_offset in the frame's light array (view space, as DrawState.lights). std430.
// material_color/ambient_color always come from the channel-0 registers (uniforms.materials[0]/ambient[0]), even when MATSRC/AMBSRC selects the vertex
// colour; the albedo target carries the per-pixel vertex colour. Logic-op draws write neither target: their pixels keep the record/albedo underneath.
constexpr uint32_t kRtModelValid=1, kRtReflective=2, kRtEmissive=4;
constexpr uint32_t kRtNoRecord=~0u;
// The record's flags also carry the channel-0 attenuation function (bits 8-9) and diffuse mode (bits 10-11) the RT shadow pass needs to evaluate the draw's
// lights as lightControl() does.
constexpr uint32_t kRtAttenuationShift=8, kRtDiffuseShift=10;
struct RtMaterialRecord { std::array<float,4> material_color, ambient_color; uint32_t light_mask; uint32_t lights_offset; uint32_t flags; float roughness; };
static_assert(sizeof(RtMaterialRecord)==48);
inline uint32_t rt_record_flags(const gx::ChannelLighting& channel0, bool model_valid) {
    return (model_valid ? kRtModelValid : 0u)|((channel0.attenuation&3u)<<kRtAttenuationShift)|((channel0.diffuse&3u)<<kRtDiffuseShift);
}
// Whether the RT estimate can model the draw's colour: channel 0 lit, and TEV stage 0 producing the rasterised colour of channel 0 times a texture or
// konst (or one), with every later stage passing that result through unchanged. Fog comes after the TEV and does not matter. No stage at all is the
// same form (tev.slang evaluateTev: rasterised colour x texture 0). Stage 0's rasteriser swap table must be the identity.
inline bool light_model_valid(const gx::TevMaterial& material, const gx::ChannelLighting& channel0) {
    if (!channel0.lit || material.stages>16) return false;
    if (material.stages==0) return true;
    if (material.swaps[material.stage[0].alpha&3]!=0xE4) return false;   // a swizzled rasterised colour is not the lit colour
    constexpr uint32_t kZero=15, kRasterised=10;
    const auto plain=[](uint32_t env) { return ((env>>16)&0x37u)==0; };   // no bias, add, scale 1 (bits 16-17 bias, 18 subtract, 20-21 scale)
    const uint32_t env=material.stage[0].color, a=(env>>12)&15, b=(env>>8)&15, c=(env>>4)&15, d=env&15;
    const auto factor=[](uint32_t selector) { return selector==8 || selector==12 || selector==14; };   // texture, one, konst
    if (!plain(env) || a!=kZero || d!=kZero || ((material.stage[0].order>>7)&7)!=0) return false;
    if (!((b==kRasterised && factor(c)) || (c==kRasterised && factor(b)))) return false;
    uint32_t current=(env>>22)&3;   // the register holding the lit result
    for (uint32_t i=1;i<material.stages;++i) {
        const uint32_t next=material.stage[i].color;
        if (!plain(next) || ((next>>4)&0xFFFu)!=0xFFFu || (next&15)!=current*2) return false;   // a = b = c = zero, d = that register
        current=(next>>22)&3;
    }
    return true;
}
}
