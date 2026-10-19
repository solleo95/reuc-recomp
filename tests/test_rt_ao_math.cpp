#include "check.h"
#include "rt_ao_math.h"
#include <cmath>
namespace {
bool near(float a, float b, float eps=1e-4f) { return std::fabs(a-b)<=eps; }
float dot(const std::array<float,3>& a, const std::array<float,3>& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
}
int main() {
    using uc::EffectLevel;
    // Presets (plan p4, call 1 and the design's table): off is off, the divisor shrinks and the rays and radius grow with the level
    CHECK(uc::rt_ao_preset(EffectLevel::off).divisor==0 && uc::rt_ao_preset(EffectLevel::off).rays==0);
    const auto low=uc::rt_ao_preset(EffectLevel::low), medium=uc::rt_ao_preset(EffectLevel::medium), high=uc::rt_ao_preset(EffectLevel::high), ultra=uc::rt_ao_preset(EffectLevel::ultra);
    CHECK(low.divisor==4 && low.rays==1 && medium.divisor==2 && medium.rays==1 && high.divisor==2 && high.rays==2 && ultra.divisor==1 && ultra.rays==2);   // measured (S11): full resolution costs 1.0 ms at 2 rays, half resolution about a third
    CHECK(low.radius<=medium.radius && medium.radius<=high.radius && high.radius<=ultra.radius && low.radius>0.f);
    CHECK(low.strength>0.f && low.strength<=1.f && ultra.strength<=1.f);
    // The factor: no blocked ray changes nothing, all blocked rays take the preset's strength
    CHECK(near(uc::rt_ao_factor(0.f,0.8f),1.f) && near(uc::rt_ao_factor(1.f,0.8f),0.2f) && near(uc::rt_ao_factor(0.5f,0.8f),0.6f));
    // Cosine-weighted directions: unit length, in the normal's hemisphere, mean cosine 2/3 over the whole (u1, u2) grid
    for(const std::array<float,3> normal:{std::array<float,3>{0,1,0},std::array<float,3>{0,0,1},std::array<float,3>{0.6f,0.f,-0.8f},std::array<float,3>{-0.577350f,0.577350f,0.577350f}}) {
        double sum=0; int count=0; bool hemisphere=true, unit=true;
        for(int i=0;i<32;++i) for(int j=0;j<32;++j) {
            const auto d=uc::rt_ao_direction(normal,(float(i)+0.5f)/32.f,(float(j)+0.5f)/32.f);
            const float c=dot(d,normal);
            hemisphere=hemisphere && c>=-1e-5f; unit=unit && near(dot(d,d),1.f,1e-3f);
            sum+=c; ++count;
        }
        CHECK(hemisphere && unit);
        CHECK(std::fabs(sum/count-2.0/3.0)<0.02);
    }
    return 0;
}
