#include "check.h"
#include "hdr_math.h"
#include <cmath>
int main() {
    using namespace uc;
    // Expansion: black stays black, the mid-tones follow paper white, SDR white reaches the peak, nothing goes backwards.
    CHECK(hdr_expand(0,200,1000)==0);
    CHECK(std::abs(hdr_expand(.3f,200,1000)-60)<1e-3f);
    CHECK(std::abs(hdr_expand(kHdrKnee,200,1000)-kHdrKnee*200)<1e-3f);
    CHECK(std::abs(hdr_expand(1,200,1000)-1000)<1e-2f);
    float last=-1;
    for(int i=0;i<=1000;++i) { const float nits=hdr_expand(i/1000.f,200,1000); CHECK(nits>=last); last=nits; }
    // A peak below paper white clamps instead of inverting the curve.
    CHECK(std::abs(hdr_expand(.5f,400,100)-100)<1e-3f);
    CHECK(std::abs(hdr_expand(.1f,400,100)-40)<1e-3f);
    CHECK(std::abs(hdr_expand(1,200,200)-200)<1e-3f);
    // Bloom glow (scene-referred, 1.0 = paper white) is added in nits and never passes the peak; no glow is the plain expansion.
    CHECK(hdr_with_glow(.3f,0,200,1000)==hdr_expand(.3f,200,1000));
    CHECK(hdr_with_glow(0,0,200,1000)==0);
    CHECK(std::abs(hdr_with_glow(0,.5f,200,1000)-100)<1e-3f);
    CHECK(std::abs(hdr_with_glow(.5f,.5f,200,1000)-200)<1e-3f);
    CHECK(std::abs(hdr_with_glow(1,9,200,1000)-1000)<1e-3f);
    CHECK(std::abs(hdr_with_glow(.5f,5,400,100)-100)<1e-3f);   // a peak below paper white clamps, no inversion
    CHECK(hdr_with_glow(.2f,-1,200,1000)==hdr_expand(.2f,200,1000));   // a negative glow adds nothing
    // PQ (SMPTE ST 2084): 0 nits is code 0 at 10 bits, 10000 nits is 1, 100 nits about 0.508.
    CHECK(pq_encode(0)*1023<.5f);
    CHECK(std::abs(pq_encode(10000)-1)<1e-5f);
    CHECK(std::abs(pq_encode(100)-.5081f)<1e-3f);
    CHECK(pq_encode(-5)==pq_encode(0));
    // Interface brightness of the game's own interface: none while HDR is off or the interface is not dimmer than paper white; else (ui/paper)^(1/2.2) of 255.
    CHECK(hdr_ui_dim(HdrSettings{false,1000,300,100})==0 && hdr_ui_dim(HdrSettings{true,1000,300,300})==0 && hdr_ui_dim(HdrSettings{true,1000,300,500})==0);
    CHECK(hdr_ui_dim(HdrSettings{true,1000,300,150})==uint8_t(std::lround(255*std::pow(.5,1/2.2))));   // 186
    CHECK(hdr_ui_dim(HdrSettings{true,1000,500,80})>=1 && hdr_ui_dim(HdrSettings{true,1000,500,499})==254);
    publish_hdr_ui_dim(HdrSettings{true,1000,300,150},true); CHECK(g_hdr_ui_dim.load()==186);
    publish_hdr_ui_dim(HdrSettings{true,1000,300,150},false); CHECK(g_hdr_ui_dim.load()==0);
    // Ranges the config and the sliders share.
    CHECK(valid_hdr_settings(HdrSettings{}));
    CHECK(!valid_hdr_settings(HdrSettings{true,0,200,200}) && !valid_hdr_settings(HdrSettings{true,2001,200,200}));
    CHECK(!valid_hdr_settings(HdrSettings{true,1000,79,200}) && !valid_hdr_settings(HdrSettings{true,1000,200,501}));
    CHECK(valid_hdr_settings(HdrSettings{true,1,80,500}));
    return 0;
}
