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
    // A lamp's ray stops short of the lamp so that its own fixture (shade, chandelier) does not block it
    CHECK(near(uc::rt_lamp_tmax(10.f),10.f-uc::kRtLampClearance));
    CHECK(uc::rt_lamp_tmax(uc::kRtLampClearance*0.5f)==0.f && uc::rt_lamp_tmax(uc::kRtLampClearance)==0.f);   // a surface inside the clearance is not shadowed by its lamp
    CHECK(uc::kRtLampClearance>0.f);
    // Light classes of the shadow pass (spec: a lamp is a physical point/spot light of the level, a rig a character's directional fill light, a specular light is never traced)
    gx::LightUniform lamp; lamp.position={0.f,5.f,70.f,0.f}; lamp.color={185.f,155.f,60.f,0.f};   // a car lamp of the first scene
    gx::LightUniform rig; rig.position={-5.2e9f,6.9e9f,4.9e9f,0.f}; rig.color={29.f,29.f,29.f,0.f};   // a character rig light
    gx::LightUniform spot; spot.position={1.f,2.f,3.f,0.f}; spot.cosine={1.f,0.5f,0.f,0.f};
    CHECK(uc::rt_light_class(lamp,3)==uc::RtLightClass::lamp);
    CHECK(uc::rt_light_class(rig,0)==uc::RtLightClass::rig);
    CHECK(uc::rt_light_class(spot,3)==uc::RtLightClass::lamp);
    CHECK(uc::rt_light_class(lamp,1)==uc::RtLightClass::specular && uc::rt_light_class(rig,1)==uc::RtLightClass::specular);   // the specular function wins over the position
    // The light of the first scene's seats is a point light at the eye (view position (0.1, 2.8, 0.1)): its shadows move with the camera, so it is not traced.
    // The synthetic overhead light (a direction fixed in view space, constant under camera translation) casts the level's shadows instead.
    gx::LightUniform eye; eye.position={0.1f,2.8f,0.1f,0.f};
    CHECK(uc::rt_is_eye_light(eye) && !uc::rt_is_eye_light(lamp) && !uc::rt_is_eye_light(rig));
    CHECK(uc::kRtEyeLightRadius>2.9f && uc::kRtEyeLightRadius<5.f);   // covers the headlamp (2.8 units from the eye), not the ceiling lamps (5+ units)
    const auto& toward=uc::kRtOverheadDirection;
    CHECK(near(toward[0]*toward[0]+toward[1]*toward[1]+toward[2]*toward[2],1.f,1e-4f) && toward[1]>0.9f);   // a unit vector, mostly up (view +y)
    CHECK(near(uc::rt_overhead_factor(0.f,1.f),1.f) && near(uc::rt_overhead_factor(1.f,1.f),1.f-uc::kRtOverheadStrength));   // unblocked: nothing; blocked: the softened strength, not black
    CHECK(uc::kRtOverheadStrength>0.3f && uc::kRtOverheadStrength<0.7f);
    CHECK(uc::kRtOverheadReach>1.5f && uc::kRtOverheadReach<6.f);   // inside the room: well short of the roof (7 units above a seat top)
    CHECK(near(uc::rt_overhead_factor(0.5f,1.f),1.f-0.5f*uc::kRtOverheadStrength));            // a soft edge: half of the rays blocked
    CHECK(near(uc::rt_overhead_factor(1.f,0.f),1.f));                                          // a surface turned away from the light takes no shadow of it (its own shading already shows that)
    CHECK(near(uc::rt_overhead_facing(-0.5f),0.f) && near(uc::rt_overhead_facing(1.f),1.f) && uc::rt_overhead_facing(0.15f)>0.f && uc::rt_overhead_facing(0.15f)<1.f);
    return 0;
}
