#include "check.h"
#include "rr_math.h"
#include <cmath>
int main() {
    // Roughness: matte is fully rough, polished is smoother, clamped, never below 0.3
    CHECK(uc::rr_roughness(0.f)==1.f && std::fabs(uc::rr_roughness(0.5f)-0.3f)<1e-6f && uc::rr_roughness(1.f)>=0.3f-1e-6f && uc::rr_roughness(-1.f)==1.f);
    CHECK(uc::rr_roughness(0.1f)<uc::rr_roughness(0.f) && uc::rr_roughness(0.3f)<uc::rr_roughness(0.1f));
    // The specular albedo of the guide: reflectance 0 gives 0 bias and scale; it is near F0 for a smooth surface seen head-on and grows toward grazing angles
    CHECK(uc::rr_specular_albedo(0.f,0.5f,0.7f)==0.f);
    const float head_on=uc::rr_specular_albedo(0.04f,0.3f,1.f), grazing=uc::rr_specular_albedo(0.04f,0.3f,0.1f);
    CHECK(head_on>0.03f && head_on<0.08f);
    CHECK(grazing>head_on);
    CHECK(std::fabs(uc::rr_specular_albedo(0.04f,0.3f,-0.5f)-uc::rr_specular_albedo(0.04f,0.3f,0.5f))<1e-6f);   // the sign of n.v does not matter
    CHECK(uc::rr_specular_albedo(0.8f,0.2f,0.8f)>uc::rr_specular_albedo(0.04f,0.2f,0.8f));
    // The decision
    CHECK(uc::rr_use(true,true,true) && !uc::rr_use(false,true,true) && !uc::rr_use(true,false,true) && !uc::rr_use(true,true,false));
    return 0;
}
