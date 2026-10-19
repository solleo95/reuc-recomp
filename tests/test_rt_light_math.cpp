#include "check.h"
#include "rt_light_math.h"
#include <cmath>
int main() {
    using uc::EffectLevel;
    CHECK(uc::rt_light_preset(EffectLevel::off).divisor==0 && uc::rt_light_preset(EffectLevel::off).rays==0);
    const auto low=uc::rt_light_preset(EffectLevel::low), medium=uc::rt_light_preset(EffectLevel::medium), high=uc::rt_light_preset(EffectLevel::high), ultra=uc::rt_light_preset(EffectLevel::ultra);
    CHECK(low.divisor==4 && medium.divisor==2 && high.divisor==2 && ultra.divisor==1 && ultra.rays==2 && low.rays==1);
    CHECK(low.reach<medium.reach && medium.reach<high.reach && high.reach<ultra.reach && low.strength<=ultra.strength && ultra.strength<=1.f);
    // The bounce adds, never removes: the result is the colour plus albedo x irradiance x strength, clamped
    CHECK(uc::rt_light_add(0.2f,0.5f,0.4f,0.6f)>0.2f);
    CHECK(std::fabs(uc::rt_light_add(0.2f,0.5f,0.4f,0.6f)-(0.2f+0.5f*0.4f*0.6f))<1e-6f);
    CHECK(uc::rt_light_add(0.2f,0.5f,0.f,0.6f)==0.2f && uc::rt_light_add(0.95f,1.f,1.f,1.f)==1.f);
    // The hit's weight: a hit near the screen centre, close by, counts fully; at the reach or at the edge it fades to nothing
    CHECK(std::fabs(uc::rt_hit_confidence(0.f,10.f,1.f)-1.f)<1e-6f && uc::rt_hit_confidence(10.f,10.f,1.f)==0.f && uc::rt_hit_confidence(5.f,10.f,0.f)==0.f);
    CHECK(uc::rt_hit_confidence(5.f,10.f,1.f)<uc::rt_hit_confidence(1.f,10.f,1.f) && uc::rt_hit_confidence(5.f,10.f,0.5f)<uc::rt_hit_confidence(5.f,10.f,1.f));
    return 0;
}
