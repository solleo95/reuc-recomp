#include "check.h"
#include "bloom_math.h"
#include <cmath>
int main() {
    using namespace uc;
    // Inverse expansion: black stays black, the mid-tones are untouched, SDR white counts as the headroom, nothing goes backwards.
    CHECK(bloom_expand(0,4)==0);
    CHECK(std::abs(bloom_expand(kBloomKnee,4)-kBloomKnee)<1e-6f);
    CHECK(std::abs(bloom_expand(.3f,4)-.3f)<1e-6f);
    CHECK(std::abs(bloom_expand(1,4)-4)<1e-5f);
    float last=-1;
    for(int i=0;i<=1000;++i) { const float v=bloom_expand(i/1000.f,4); CHECK(v>=last); last=v; }
    // The soft threshold keeps lit walls and skin out of the glow (a live train car turned milky without it): nothing below the knee's start, a quadratic ramp, then
    // the plain excess over the threshold, continuous and never decreasing.
    CHECK(bloom_soft_threshold(0)==0);
    CHECK(bloom_soft_threshold(kBloomThreshold-kBloomThresholdKnee)==0);
    CHECK(bloom_soft_threshold(.3f)<.001f);                           // a mid-tone wall adds next to nothing
    CHECK(std::abs(bloom_soft_threshold(kBloomThreshold+kBloomThresholdKnee)-kBloomThresholdKnee)<1e-6f);   // at the knee's end: the excess over the threshold
    CHECK(std::abs(bloom_soft_threshold(4)-(4-kBloomThreshold))<1e-6f);   // a lamp at the headroom keeps nearly all of its energy
    last=-1;
    for(int i=0;i<=4000;++i) { const float v=bloom_soft_threshold(i/1000.f); CHECK(v>=last); last=v; }
    // The mix: off and strength 0 add nothing, 100 % is the base mix (times the HDR gain in HDR), 200 % doubles it.
    CHECK(bloom_mix(BloomSettings{false,100},false)==0);
    CHECK(bloom_mix(BloomSettings{true,0},false)==0 && bloom_mix(BloomSettings{true,0},true)==0);
    CHECK(std::abs(bloom_mix(BloomSettings{true,100},false)-.15f)<1e-6f);
    CHECK(std::abs(bloom_mix(BloomSettings{true,100},true)-.375f)<1e-6f);
    CHECK(std::abs(bloom_mix(BloomSettings{true,200},false)-.30f)<1e-6f);
    // The slider's range.
    CHECK(valid_bloom_settings(BloomSettings{}));
    CHECK(valid_bloom_settings(BloomSettings{true,0}) && valid_bloom_settings(BloomSettings{true,200}));
    CHECK(!valid_bloom_settings(BloomSettings{true,201}));
    // The six levels weigh 1 in all, so the glow is a mix and not a gain.
    float sum=0;
    for(const float w:kBloomWeights) sum+=w;
    CHECK(std::abs(sum-1)<1e-6f);
    // Levels follow the screen: half size for level 0, a sixty-fourth for level 5, never below one pixel.
    CHECK(bloom_level_extent(2560,1440,0).width==1280 && bloom_level_extent(2560,1440,0).height==720);
    CHECK(bloom_level_extent(2560,1440,5).width==40 && bloom_level_extent(2560,1440,5).height==22);
    CHECK(bloom_level_extent(3,3,5).width==1 && bloom_level_extent(3,3,5).height==1);
    // Karis: a bright sample counts for less, so one hot pixel cannot flicker the whole level.
    CHECK(bloom_karis_weight(0)==1);
    CHECK(std::abs(bloom_karis_weight(3)-.25f)<1e-6f);
    return 0;
}
