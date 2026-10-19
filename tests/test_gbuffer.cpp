#include "check.h"
#include "gbuffer.h"
#include <cmath>
int main() {
    const auto near=[](float a,float b,float tolerance) { return std::abs(a-b)<=tolerance; };
    for(const auto& normal:{std::array<float,3>{0,0,1},{0.6f,0,0.8f},{-0.577350f,-0.577350f,-0.577350f},{0,1,0},{0,0,-1},{-1,0,0},{0.3f,-0.4f,-0.866f}}) {
        const float length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        const std::array<float,3> unit{normal[0]/length,normal[1]/length,normal[2]/length};
        const auto texel=uc::decode_gbuffer(uc::encode_gbuffer(unit,0.25f,uc::kGBufferHasNormal|uc::kGBufferLit));
        CHECK(near(texel.normal[0],unit[0],0.001f) && near(texel.normal[1],unit[1],0.001f) && near(texel.normal[2],unit[2],0.001f));   // octahedral round trip, both hemispheres
        CHECK(near(texel.fog,0.25f,1.f/65535) && texel.flags==(uc::kGBufferHasNormal|uc::kGBufferLit));
    }
    const auto none=uc::decode_gbuffer({0,0,0,0});   // a cleared texel: no surface
    CHECK(none.flags==0 && none.normal[0]==0 && none.normal[1]==0 && none.normal[2]==0 && none.fog==0);
    const auto unlit=uc::decode_gbuffer(uc::encode_gbuffer({0,0,0},1.f,0));   // a draw without a normal keeps its fog and a zero normal
    CHECK(unlit.flags==0 && unlit.normal[2]==0 && near(unlit.fog,1.f,1.f/65535));
    CHECK(uc::decode_gbuffer(uc::encode_gbuffer({0,0,1},0,uc::kGBufferReflective|uc::kGBufferTessellated)).flags==(uc::kGBufferReflective|uc::kGBufferTessellated));
    return 0;
}
