#include "scene_lights.h"
#include <algorithm>
#include <cmath>
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
namespace {
bool close(float a, float b) { return std::abs(a-b)<=1e-4f*std::max({1.f,std::abs(a),std::abs(b)}); }
bool close(const std::array<float,3>& a, const std::array<float,3>& b) { return close(a[0],b[0]) && close(a[1],b[1]) && close(a[2],b[2]); }
bool same_light(const CollectedLight& a, const CollectedLight& b) { return a.kind==b.kind && close(a.position,b.position) && close(a.direction,b.direction) && close(a.color,b.color); }
float score(const CollectedLight& light) { return float(light.draws)*std::max({light.color[0],light.color[1],light.color[2]}); }
bool shadow_caster(const CollectedLight& light) { return light.kind==LightKind::directional || light.kind==LightKind::spot; }
}
void SceneLightCollector::add(const DrawUniform& uniforms) {
    std::array<size_t,16> counted; size_t counted_size=0;   // lights of this draw already counted (both colour channels may use the same one; no allocation per draw)
    for (size_t channel=0;channel<2;++channel) {
        const auto lighting=channel_lighting(uniforms.channels[channel]);
        if (!lighting.lit) continue;
        for (size_t light=0;light<8;++light) {
            if (!(lighting.mask&(1u<<light))) continue;
            const auto& source=uniforms.lights[light];
            CollectedLight found{classify_light(source,lighting.attenuation),{source.position[0],source.position[1],source.position[2]},
                {source.direction[0],source.direction[1],source.direction[2]},{source.color[0],source.color[1],source.color[2]},0};
            size_t at=0;
            while (at<lights_.size() && !same_light(lights_[at],found)) ++at;
            if (at==lights_.size()) lights_.push_back(found);
            if (std::find(counted.begin(),counted.begin()+counted_size,at)==counted.begin()+counted_size) { counted[counted_size++]=at; ++lights_[at].draws; }
        }
    }
}
std::optional<CollectedLight> select_dominant(std::span<const CollectedLight> lights, const std::optional<CollectedLight>& previous) {
    const CollectedLight* best=nullptr; const CollectedLight* held=nullptr;
    for (const auto& light:lights) {
        if (!shadow_caster(light)) continue;
        if (!best || score(light)>score(*best)) best=&light;
        if (previous && same_light(light,*previous)) held=&light;
    }
    if (!best) return std::nullopt;
    if (held && score(*best)<1.25f*score(*held)) return *held;
    return *best;
}
}
