#pragma once
#include "gx_vertices.h"
#include <array>
#include <bit>
#include <cstdint>
namespace gx {
// The GPU image of an EFB copy is the guest texture times the render scale (width*efb/640, height*efb/480), but GX indirect texturing counts in texels of the
// guest texture: the shader needs the scale and the maps that are copies. They travel in the unused words 5..7 of DrawUniform::fog (the uniform block is copied
// to the GPU as it is): scale x, scale y, and a bit per texture map (bit 0 = draw.texture, bit n = texture_maps[n-1]). Zero when no map is a copy.
constexpr size_t kCopyScaleFogWord=5;
inline std::array<uint32_t,3> copy_scale_words(const DrawPacket& draw,float scale_x,float scale_y) {
    uint32_t mask=draw.texture && draw.texture->gpu_only ? 1u : 0u;
    for(size_t i=0;i<draw.texture_maps.size();++i) if(draw.texture_maps[i] && draw.texture_maps[i]->gpu_only) mask|=2u<<i;
    if(!mask) return {};
    return {std::bit_cast<uint32_t>(scale_x),std::bit_cast<uint32_t>(scale_y),mask};
}
}
