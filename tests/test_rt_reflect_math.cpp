#include "check.h"
#include "rt_reflect_math.h"
#include <cmath>
namespace {
bool near(float a, float b, float eps=1e-4f) { return std::fabs(a-b)<=eps; }
}
int main() {
    using uc::EffectLevel;
    // Presets (plan p5, call 5)
    CHECK(uc::rt_reflect_preset(EffectLevel::off).divisor==0 && uc::rt_reflect_preset(EffectLevel::off).rays==0);
    const auto low=uc::rt_reflect_preset(EffectLevel::low), medium=uc::rt_reflect_preset(EffectLevel::medium), high=uc::rt_reflect_preset(EffectLevel::high), ultra=uc::rt_reflect_preset(EffectLevel::ultra);
    CHECK(low.divisor==4 && medium.divisor==2 && high.divisor==2 && ultra.divisor==1 && ultra.rays==2 && low.rays==1);
    CHECK(low.cone==0.f && medium.cone==0.f && high.cone>0.f && ultra.cone>=high.cone);
    CHECK(low.reach<=medium.reach && medium.reach<=high.reach && high.reach<=ultra.reach);
    // Reflectivity of a draw: half the square root of the specular material's luminance, only when channel 1 is lit with the specular function
    CHECK(near(uc::rt_reflectivity({0.f,0.f,0.f},true),0.f));
    CHECK(near(uc::rt_reflectivity({1.f,1.f,1.f},true),0.5f,1e-3f));
    CHECK(near(uc::rt_reflectivity({0.05f,0.05f,0.05f},true),0.5f*std::sqrt(0.05f),1e-3f));
    CHECK(uc::rt_reflectivity({1.f,1.f,1.f},false)==0.f);   // an unlit second channel is a baked colour
    CHECK(uc::rt_reflectivity({2.f,2.f,2.f},true)<=0.5f+1e-4f);   // clamped
    // Eight bits in the record flags, bits 12 to 19, with kRtReflective above zero
    for(const float r:{0.f,0.07f,0.16f,0.5f}) {
        const uint32_t flags=uc::rt_reflect_pack(r,uc::kRtModelValid|(3u<<8));   // model-valid, attenuation 3: untouched by the packing
        CHECK((flags&uc::kRtModelValid) && ((flags>>8)&3u)==3u && near(uc::rt_reflect_unpack(flags),r,1.f/255.f/2+1e-4f) && ((flags&uc::kRtReflective)!=0)==(r>0.f));
    }
    CHECK(uc::kRtReflectHistory>=2 && uc::kRtReflectHistory<uc::kDenoiseMaxHistory);   // shorter than a shadow's history
    // Schlick: F0 at the normal, 1 at grazing
    CHECK(near(uc::rt_fresnel(1.f),uc::kRtFresnelF0,1e-5f) && near(uc::rt_fresnel(0.f),1.f,1e-5f) && uc::rt_fresnel(0.3f)>uc::rt_fresnel(0.8f));
    // The mirror direction of a view ray about a normal; a ray leaving the surface stays on the normal's side
    const auto r=uc::rt_reflect({0.f,-0.6f,-0.8f},{0.f,1.f,0.f});
    CHECK(near(r[0],0.f) && near(r[1],0.6f) && near(r[2],-0.8f));
    // The projection of a view position to a pixel is the inverse of the depth reconstruction (view_reconstruct.h)
    const gx::Projection projection{{{1.5f,0,0,0},{0,2.f,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
    const auto view=uc::view_params(projection);
    const std::array<float,6> viewport{0,0,1280,720,0,1};
    for(const std::array<float,3> p:{std::array<float,3>{0.4f,-0.3f,-5.f},std::array<float,3>{-2.f,1.f,-20.f}}) {
        const auto at=uc::rt_project(view,viewport,p);
        const float ndc_x=(at.pixel[0]/1280.f)*2.f-1.f, ndc_y=(at.pixel[1]/720.f)*2.f-1.f;
        const auto back=uc::reconstruct_view(view,ndc_x,ndc_y,at.depth);
        CHECK(near(back[0],p[0],1e-3f) && near(back[1],p[1],1e-3f) && near(back[2],p[2],1e-3f));
    }
    // A point behind the camera has no pixel
    CHECK(!uc::rt_project(view,viewport,{0.f,0.f,1.f}).valid);
    // Depth agreement: the frame shows the hit when its stored depth is within 3% of the hit's
    CHECK(uc::rt_hit_visible(10.f,10.2f) && !uc::rt_hit_visible(10.f,8.f) && !uc::rt_hit_visible(10.f,12.f));
    return 0;
}
