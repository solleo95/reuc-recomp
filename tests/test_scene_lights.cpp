#include "check.h"
#include "scene_lights.h"
#include <vector>
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
    {   // collecting: a light shared by two draws is one light lit by two draws; point lights are kept but never dominant
        gx::DrawUniform draw;
        draw.channels[0]=1|2|(1<<2)|(2<<7)|(3<<9);
        draw.lights[0].position={1,2,3,0}; draw.lights[0].cosine={-3,4,0,0}; draw.lights[0].color={200,200,200,255};   // a spot light
        gx::SceneLightCollector collector;
        collector.add(draw); collector.add(draw);
        CHECK(collector.lights().size()==1 && collector.lights()[0].draws==2 && collector.lights()[0].kind==gx::LightKind::spot);
        draw.lights[0].position={1e6f,0,0,0};   // another draw: a directional light
        collector.add(draw);
        CHECK(collector.lights().size()==2 && collector.lights()[1].kind==gx::LightKind::directional && collector.lights()[1].draws==1);
        gx::DrawUniform unlit; collector.add(unlit);   // channels without lighting add nothing
        CHECK(collector.lights().size()==2);
        collector.clear(); CHECK(collector.lights().empty());
    }
    {   // the dominant light: only directional and spot lights, with hysteresis
        const auto make=[](gx::LightKind kind,float x,uint32_t draws) { return gx::CollectedLight{kind,{x,0,0},{0,0,1},{1,0,0},draws}; };
        CHECK(!gx::select_dominant({},std::nullopt));
        const std::vector<gx::CollectedLight> points{make(gx::LightKind::point,1,50)};
        CHECK(!gx::select_dominant(points,std::nullopt));
        const auto a=make(gx::LightKind::directional,1,10);
        std::vector<gx::CollectedLight> lights{a,make(gx::LightKind::spot,2,12),make(gx::LightKind::point,3,99)};
        CHECK(gx::select_dominant(lights,a)->position[0]==1);   // 12 does not beat 10 by a quarter: the previous light stays
        CHECK(gx::select_dominant(lights,std::nullopt)->position[0]==2);   // without history the strongest wins
        lights[1].draws=13;
        CHECK(gx::select_dominant(lights,a)->position[0]==2);   // 13 > 12.5: the new light takes over
        CHECK(gx::select_dominant(lights,make(gx::LightKind::directional,9,10))->position[0]==2);   // a previous light that is gone does not hold anything
    }
    return 0;
}
