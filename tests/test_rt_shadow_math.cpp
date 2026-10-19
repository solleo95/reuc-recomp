#include "check.h"
#include "rt_shadow_math.h"
#include <cmath>
namespace {
bool near(float a, float b, float eps=1e-4f) { return std::fabs(a-b)<=eps; }
}
int main() {
    using uc::EffectLevel;
    // Presets: resolution divisor, rays, soft edges (docs/superpowers/plans/2026-10-19-ray-tracing-p2.md, Task 7)
    CHECK(uc::rt_shadow_preset(EffectLevel::off).divisor==0 && uc::rt_shadow_preset(EffectLevel::off).rays==0);
    const auto low=uc::rt_shadow_preset(EffectLevel::low), ultra=uc::rt_shadow_preset(EffectLevel::ultra);
    CHECK(low.divisor==4 && low.rays==1 && !low.soft);
    CHECK(uc::rt_shadow_preset(EffectLevel::medium).divisor==2 && !uc::rt_shadow_preset(EffectLevel::medium).soft);
    CHECK(uc::rt_shadow_preset(EffectLevel::high).soft && uc::rt_shadow_preset(EffectLevel::high).divisor==2);
    CHECK(ultra.divisor==1 && ultra.rays==2 && ultra.soft);
    CHECK(uc::rt_local_preset(EffectLevel::off).divisor==0 && uc::rt_local_preset(EffectLevel::off).lights==0);
    CHECK(uc::rt_local_preset(EffectLevel::low).lights==1 && uc::rt_local_preset(EffectLevel::medium).lights==2);
    CHECK(uc::rt_local_preset(EffectLevel::high).lights==4 && uc::rt_local_preset(EffectLevel::ultra).lights==8 && uc::rt_local_preset(EffectLevel::ultra).soft);

    // The share of a pixel's light a shadowed light removes
    const float one[]={0.8f}, zero_vis[]={0.f}, full_vis[]={1.f};
    CHECK(near(uc::shadow_factor(0.2f,one,zero_vis),0.2f));
    CHECK(near(uc::shadow_factor(0.2f,one,full_vis),1.f));
    const float two[]={0.4f,0.4f}, half[]={0.f,1.f};
    CHECK(near(uc::shadow_factor(0.2f,two,half),0.6f));
    CHECK(uc::shadow_factor(0.f,{},{})==1.f);   // no light at all: nothing to remove
    // GX clamps ambient + lights at 1 before it multiplies the material (smoke.slang lightControl): the shadow removes a light's share of the clamped sum
    const float bright[]={0.8f};
    CHECK(near(uc::shadow_factor(0.5f,bright,zero_vis),0.5f));   // (1.3 - 0.8) clamps to 0.5, 1.3 clamps to 1
    const float half_light[]={0.5f};
    CHECK(near(uc::shadow_factor(1.2f,half_light,zero_vis),1.f));   // the ambient alone already saturates the surface: the shadow changes nothing
    CHECK(uc::shadow_factor(0.5f,bright,zero_vis)>=0.f && uc::shadow_factor(0.5f,bright,zero_vis)<=1.f);

    // GX contribution (renderer/shaders/smoke.slang lightControl): attenuation 0 = none, 3 = distance/spot; diffuse 2 = clamped N.L
    gx::LightUniform directional;
    directional.color={255.f,255.f,255.f,0.f}; directional.position={0.f,0.f,1e6f,0.f};
    const std::array<float,3> origin{0,0,0}, up{0,0,1}, tilted{std::sqrt(0.75f),0.f,0.5f};   // N.L = 0.5
    CHECK(near(uc::gx_light_contribution(directional,0,2,origin,up),1.f,1e-3f));
    CHECK(near(uc::gx_light_contribution(directional,0,2,origin,tilted),0.5f,1e-3f));
    const std::array<float,3> away{0,0,-1};
    CHECK(uc::gx_light_contribution(directional,0,2,origin,away)==0.f);   // diffuse 2 clamps at zero
    CHECK(uc::gx_light_contribution(directional,0,1,origin,away)==0.f);   // signed diffuse (mode 1) goes negative behind the surface: the share is clamped at zero
    gx::LightUniform point;
    point.color={255.f,255.f,255.f,0.f}; point.position={0.f,0.f,2.f,0.f}; point.cosine={1.f,0.f,0.f,0.f}; point.distance={1.f,0.f,1.f,0.f};
    CHECK(near(uc::gx_light_contribution(point,3,2,origin,up),0.2f));   // 1 / (1 + d^2) at d = 2
    CHECK(uc::gx_light_contribution(point,1,2,origin,up)==0.f);   // the specular function is not a diffuse light
    gx::LightUniform dim=directional; dim.color={0.f,255.f,0.f,0.f};
    CHECK(near(uc::gx_light_contribution(dim,0,2,origin,up),0.587f,1e-3f));   // luminance weights
    return 0;
}
