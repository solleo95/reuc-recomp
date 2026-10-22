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
    // PQ (SMPTE ST 2084): 0 nits is code 0 at 10 bits, 10000 nits is 1, 100 nits about 0.508.
    CHECK(pq_encode(0)*1023<.5f);
    CHECK(std::abs(pq_encode(10000)-1)<1e-5f);
    CHECK(std::abs(pq_encode(100)-.5081f)<1e-3f);
    CHECK(pq_encode(-5)==pq_encode(0));
    // Ranges the config and the sliders share.
    CHECK(valid_hdr_settings(HdrSettings{}));
    CHECK(!valid_hdr_settings(HdrSettings{true,0,200,200}) && !valid_hdr_settings(HdrSettings{true,2001,200,200}));
    CHECK(!valid_hdr_settings(HdrSettings{true,1000,79,200}) && !valid_hdr_settings(HdrSettings{true,1000,200,501}));
    CHECK(valid_hdr_settings(HdrSettings{true,1,80,500}));
    return 0;
}
