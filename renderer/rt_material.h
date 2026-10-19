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
// Whether the RT estimate can model the draw's colour: channel 0 lit with its ambient from the register, and the TEV's final colour a sum of terms that are each
// the rasterised colour of channel 0 times a light-independent factor (texture, konst, one, half) or a previous such result, so that scaling the light
// scales the colour. Evaluated stage by stage over {zero, factor, proportional to the lit colour, unknown}; a stage with a bias, a subtraction, an
// interpolation other than a = zero or c = zero, the rasterised colour twice or a light-independent term added to a proportional one makes the result unknown. The
// colour scale is free (it multiplies the lit colour too). Fog comes after the TEV and does not matter. No stage at all is the same form (tev.slang
// evaluateTev: rasterised colour x texture 0). A stage that reads the rasterised colour needs the identity swap table and colour channel 0.
// A stage may also read the rasteriser colour of channel 1 when channel 1 is lit with the specular function only (the game's second channel: a highlight that follows
// the same lights); it counts as proportional like channel 0, so the factor of channel 0's diffuse lights dims the highlight with the diffuse part (an approximation).
inline bool light_model_valid(const gx::TevMaterial& material, const gx::ChannelLighting& channel0, const gx::ChannelLighting& channel1={}) {
    if (!channel0.lit || channel0.ambient_vertex || material.stages>16) return false;
    if (material.stages==0) return true;
    enum Kind { zero, factor, lit, unknown };
    const auto product=[](Kind x, Kind y) { return x==zero || y==zero ? zero : x==unknown || y==unknown || (x==lit && y==lit) ? unknown : x==lit || y==lit ? lit : factor; };
    // A light-independent term next to a lit one (a second texture times a konst, an emissive texture) stays "lit": the shadow factor dims it with the lit part,
    // an approximation that over-darkens such a term inside a shadow (the game's seats, floors and walls carry one).
    const auto sum=[](Kind x, Kind y) { return x==zero ? y : y==zero ? x : x==unknown || y==unknown ? unknown : x==y ? x : lit; };
    Kind registers[4]={unknown,unknown,unknown,unknown};
    uint32_t output=0;
    for (uint32_t i=0;i<material.stages;++i) {
        const auto& stage=material.stage[i];
        const uint32_t env=stage.color, selectors[4]={(env>>12)&15,(env>>8)&15,(env>>4)&15,env&15};
        const auto kind=[&](uint32_t selector) -> Kind {
            if (selector<8) return selector%2==0 ? registers[selector/2] : unknown;   // 0, 2, 4, 6: the colour registers; the odd ones are alphas
            return selector==10 ? lit : selector==8 || selector==12 || selector==13 || selector==14 ? factor : selector==15 ? zero : unknown;   // 8 texture, 10 rasterised, 12 one, 13 half, 14 konst, 15 zero
        };
        bool reads_rasterised=false;
        for (const uint32_t selector:selectors) reads_rasterised|=selector==10;
        const uint32_t channel=(stage.order>>7)&7;
        if (reads_rasterised && (material.swaps[stage.alpha&3]!=0xE4 || (channel!=0 && !(channel==1 && channel1.lit && channel1.attenuation==1)))) return false;   // a swizzled colour, or a channel that is neither channel 0 nor a specular channel 1, is not the lit colour
        const bool plain=((env>>16)&7u)==0;   // no bias (bits 16-17), no subtraction (bit 18); the scale (20-21) is free
        // d + lerp(a, b, c) = d + a x (1 − c) + b x c: with c = zero it is d + a, with a = zero it is d + b x c; another interpolation is not modelled
        const Kind term=selectors[2]==15 ? kind(selectors[0]) : selectors[0]==15 ? product(kind(selectors[1]),kind(selectors[2])) : unknown;
        output=(env>>22)&3;
        registers[output]=plain ? sum(kind(selectors[3]),term) : unknown;
    }
    return registers[output]==lit;
}
}
