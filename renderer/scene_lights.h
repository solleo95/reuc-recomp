#pragma once
#include "gx_materials.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace gx {
enum class LightKind { directional, point, spot, specular };
// A GX colour/alpha channel control word as lightControl() in smoke.slang reads it.
struct ChannelLighting { bool lit=false; uint8_t mask=0; uint32_t attenuation=0; uint32_t diffuse=0; bool ambient_vertex=false; };   // ambient_vertex: the ambient comes from the vertex colour, not the register
ChannelLighting channel_lighting(uint32_t control);
// attenuation: the channel's attenuation function (0 none, 1 specular, 3 spot/distance).
LightKind classify_light(const LightUniform& light, uint32_t attenuation);

// A GX light as the scene's draws set it, with the number of scene draws it lights. position is view space; for a directional light it is the far
// point it sits at (the direction to the light is its normalisation). color is in GX units (0..255).
struct CollectedLight { LightKind kind; std::array<float,3> position, direction, color; uint32_t draws; };
// The distinct lights of a frame's scene draws (the colour channels only; a light counts once per draw). Filled while the scene is recorded.
class SceneLightCollector {
public:
    void add(const DrawUniform& uniforms);
    void clear() { lights_.clear(); }
    const std::vector<CollectedLight>& lights() const { return lights_; }
private:
    std::vector<CollectedLight> lights_;
};
// The one directional or spot light the screen-space shadows use: the most lit draws times the brightest colour channel. `previous` (last frame's
// pick) keeps the job unless another light scores at least a quarter more, so the shadow does not jump between similar lights.
std::optional<CollectedLight> select_dominant(std::span<const CollectedLight> lights, const std::optional<CollectedLight>& previous);
}
