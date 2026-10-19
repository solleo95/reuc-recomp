#include "check.h"
#include "scene_lights.h"
int main() {
    {   // channel control decoding as lightControl in smoke.slang does
        const auto lighting = gx::channel_lighting(0x2 | (1 << 2) | (2 << 7) | (3 << 9));
        CHECK(lighting.lit && lighting.mask == 1 && lighting.diffuse == 2 && lighting.attenuation == 3);
        CHECK(!gx::channel_lighting(0x1).lit);
    }
    {   // light kinds
        gx::LightUniform light;
        light.position = {1e6f, 0, 0, 0};
        CHECK(gx::classify_light(light, 0) == gx::LightKind::directional);
        light.position = {1, 2, 3, 0}; light.cosine = {1, 0, 0, 0};
        CHECK(gx::classify_light(light, 3) == gx::LightKind::point);
        light.cosine = {-3, 4, 0, 0};
        CHECK(gx::classify_light(light, 3) == gx::LightKind::spot);
        CHECK(gx::classify_light(light, 1) == gx::LightKind::specular);
    }
    return 0;
}
