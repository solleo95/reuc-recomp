#include "check.h"
#include "upscale_math.h"
#include <cmath>
using uc::RenderExtent;
using uc::UpscaleMode;
int main() {
    const RenderExtent q{2560,1440};
    CHECK((uc::internal_extent(q,UpscaleMode::native)==RenderExtent{2560,1440}));
    CHECK((uc::internal_extent(q,UpscaleMode::quality)==RenderExtent{1707,960}));
    CHECK((uc::internal_extent(q,UpscaleMode::balanced)==RenderExtent{1485,835}));
    CHECK((uc::internal_extent(q,UpscaleMode::performance)==RenderExtent{1280,720}));
    CHECK((uc::internal_extent(q,UpscaleMode::ultra_performance)==RenderExtent{853,480}));
    CHECK((uc::internal_extent({3840,2160},UpscaleMode::ultra_performance)==RenderExtent{1280,720}));
    CHECK((uc::internal_extent({640,360},UpscaleMode::ultra_performance,{320,180})==RenderExtent{320,180}));   // never below the SDK minimum
    CHECK((uc::internal_extent({2,2},UpscaleMode::ultra_performance)==RenderExtent{1,1}));
    CHECK(uc::upscale_ratio(UpscaleMode::balanced)==1.724);
    CHECK(uc::jitter_phase_count(1280,2560)==32 && uc::jitter_phase_count(2560,2560)==8 && uc::jitter_phase_count(1707,2560)==18);
    auto j=uc::jitter_offset(0,8); CHECK(j[0]==0.0f && std::abs(j[1]+1.f/6)<1e-6f);   // Halton(1) = (1/2, 1/3)
    auto k=uc::jitter_offset(8,8); CHECK(j==k);   // wraps at the phase count
    auto h=uc::jitter_offset(1,8); CHECK(std::abs(h[0]+0.25f)<1e-6f && std::abs(h[1]-1.f/6)<1e-6f);   // Halton(2) = (1/4, 2/3)
    CHECK(std::abs(uc::mip_bias({1280,720},{2560,1440})+2.f)<1e-6f);
    CHECK(std::abs(uc::mip_bias({2560,1440},{2560,1440})+1.f)<1e-6f);
    CHECK(uc::rcas_constant(1.0)==1.0f && std::abs(uc::rcas_constant(0.5)-0.5f)<1e-6f && std::abs(uc::rcas_constant(0.01)-0.25348f)<1e-4f);
    CHECK(uc::dlss_preset(UpscaleMode::native)==11 && uc::dlss_preset(UpscaleMode::quality)==11 && uc::dlss_preset(UpscaleMode::balanced)==11);
    CHECK(uc::dlss_preset(UpscaleMode::performance)==13 && uc::dlss_preset(UpscaleMode::ultra_performance)==12);
    return 0;
}
