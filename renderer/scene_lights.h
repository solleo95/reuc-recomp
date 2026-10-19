#pragma once
#include "gx_materials.h"
#include <cstdint>
namespace gx {
enum class LightKind { directional, point, spot, specular };
// A GX colour/alpha channel control word as lightControl() in smoke.slang reads it.
struct ChannelLighting { bool lit=false; uint8_t mask=0; uint32_t attenuation=0; uint32_t diffuse=0; };
ChannelLighting channel_lighting(uint32_t control);
// attenuation: the channel's attenuation function (0 none, 1 specular, 3 spot/distance).
LightKind classify_light(const LightUniform& light, uint32_t attenuation);
}
