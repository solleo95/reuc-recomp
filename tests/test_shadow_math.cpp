#include "check.h"
#include "graphics_effects.h"
#include "gx_transforms.h"
#include "shadow_math.h"
#include <cmath>
namespace {
std::array<float,3> apply(const uc::ShadowFit& fit, std::array<float,3> p) {   // view -> light clip (xy in -1..1, z in 0..1)
    std::array<float,4> r{};
    for(int i=0;i<4;++i) r[i]=fit.view_to_light[i][0]*p[0]+fit.view_to_light[i][1]*p[1]+fit.view_to_light[i][2]*p[2]+fit.view_to_light[i][3];
    return {r[0],r[1],r[2]};
}
gx::CollectedLight light(std::array<float,3> position) { return {gx::LightKind::directional,position,{0,0,1},{1,1,1},10}; }
}
int main() {
    const float tangent=std::tan(30.f*3.14159265f/180), aspect=16.f/9, n=1, f=1000;
    const auto projection=gx::make_projection({1.f/(tangent*aspect),0,1.f/tangent,0,-f/(f-n),-f*n/(f-n)},0);
    {   // a light shining along the view axis (it sits behind the camera: position +z): points inside the slice map inside the map
        const auto fit=uc::fit_shadow(projection,light({0,0,1e6f}),30.f,2048);
        CHECK(fit);
        const auto inside=apply(*fit,{0,0,-10});
        CHECK(std::abs(inside[0])<=1 && std::abs(inside[1])<=1 && inside[2]>=0 && inside[2]<=1);
        const auto beyond=apply(*fit,{0,0,-40});
        CHECK(beyond[2]<0 || beyond[2]>1 || std::abs(beyond[0])>1 || std::abs(beyond[1])>1);   // outside the slice: outside the volume
        CHECK(fit->extent>0 && fit->depth_range>0 && fit->texel_world>0);
    }
    {   // snapping: a sub-texel move of the frustum does not move the box, a whole texel does
        auto fit_a=uc::fit_shadow(projection,light({1e6f,1e6f,1e6f}),30.f,2048);
        auto shifted=projection; shifted[0][3]+=0.0f;   // same projection: identical fit
        auto fit_b=uc::fit_shadow(shifted,light({1e6f,1e6f,1e6f}),30.f,2048);
        CHECK(fit_a && fit_b && fit_a->view_to_light==fit_b->view_to_light);
        const auto fit_c=uc::fit_shadow(projection,light({1e6f,1e6f,1e6f}),30.f,1024);
        CHECK(fit_c && fit_c->texel_world>fit_a->texel_world);   // a coarser map has bigger texels
        const float texel=fit_a->texel_world;
        const auto p0=apply(*fit_a,{1,2,-8}), p1=apply(*fit_a,{1+texel,2,-8});
        CHECK(p0[0]!=p1[0] || p0[1]!=p1[1]);
    }
    CHECK(!uc::fit_shadow(projection,light({0,0,0}),30.f,2048));   // a zero direction has no light basis
    CHECK(uc::shadow_preset(uc::EffectLevel::off).resolution==0);
    CHECK(uc::shadow_preset(uc::EffectLevel::low).resolution==1024 && !uc::shadow_preset(uc::EffectLevel::low).soft);
    CHECK(uc::shadow_preset(uc::EffectLevel::high).resolution==2048 && uc::shadow_preset(uc::EffectLevel::high).soft);
    CHECK(uc::shadow_preset(uc::EffectLevel::ultra).resolution==4096 && uc::shadow_preset(uc::EffectLevel::ultra).blocker_taps>uc::shadow_preset(uc::EffectLevel::high).blocker_taps);
    {   // the dominant light changed
        auto a=light({0,1e6f,0}), b=a;
        CHECK(!uc::light_changed(a,b) && !uc::light_changed(std::nullopt,std::nullopt));
        b.position[0]=1e2f; CHECK(!uc::light_changed(a,b));   // 1e-4 of the distance: the same direction
        b.position={1e6f,0,0}; CHECK(uc::light_changed(a,b));
        CHECK(uc::light_changed(a,std::nullopt) && uc::light_changed(std::nullopt,a));
    }
    return 0;
}
