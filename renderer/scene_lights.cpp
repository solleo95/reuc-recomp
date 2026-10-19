#include "scene_lights.h"
namespace gx {
ChannelLighting channel_lighting(uint32_t control) {
    ChannelLighting result;
    result.lit = (control & 2) != 0;
    result.mask = uint8_t(((control >> 2) & 15) | ((control >> 7) & 240));
    result.diffuse = (control >> 7) & 3;
    result.attenuation = (control >> 9) & 3;
    return result;
}
LightKind classify_light(const LightUniform& light, uint32_t attenuation) {
    if (attenuation == 1) return LightKind::specular;
    const float x = light.position[0], y = light.position[1], z = light.position[2];
    if (x * x + y * y + z * z > 1e10f) return LightKind::directional;   // a position beyond 1e5 units stands for a direction
    if (attenuation == 3 && (light.cosine[1] != 0 || light.cosine[2] != 0)) return LightKind::spot;
    return LightKind::point;
}
}
