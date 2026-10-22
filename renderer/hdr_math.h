#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace uc {
// HDR output (graphics.hdr*): the game's SDR frame expanded into HDR10 at the present. Mirrored in shaders/hdr.slang.
struct HdrSettings {
    bool enabled=false;
    uint32_t peak_nits=1000;         // brightest output, 1..2000
    uint32_t paper_white_nits=200;   // SDR mid-tones and diffuse white, 80..500
    uint32_t ui_nits=200;            // the port's menu and button prompts, 80..500
    bool operator==(const HdrSettings&) const=default;
};
inline constexpr uint32_t kHdrPeakMin=1, kHdrPeakMax=2000, kHdrWhiteMin=80, kHdrWhiteMax=500;
inline bool valid_hdr_settings(const HdrSettings& s) {
    const auto white=[](uint32_t nits) { return nits>=kHdrWhiteMin && nits<=kHdrWhiteMax; };
    return s.peak_nits>=kHdrPeakMin && s.peak_nits<=kHdrPeakMax && white(s.paper_white_nits) && white(s.ui_nits);
}
// Linear SDR luminance above the knee rises to the peak: y + ((y-k)/(1-k))^2 * (peak/paper - 1), in paper-white units. The square keeps the
// slope continuous at the knee (no visible seam); below it the picture is the SDR one at paper white. A peak under paper white only clamps.
inline constexpr float kHdrKnee=.6f;
inline float hdr_expand(float y, float paper, float peak) {
    y=std::clamp(y,0.f,1.f);
    if(peak<=paper) return std::min(y*paper,peak);
    const float over=std::max(0.f,(y-kHdrKnee)/(1-kHdrKnee));
    return (y+over*over*(peak/paper-1))*paper;
}
// SMPTE ST 2084 inverse EOTF: absolute nits -> PQ signal 0..1.
inline float pq_encode(float nits) {
    const float m1=.1593017578125f, m2=78.84375f, c1=.8359375f, c2=18.8515625f, c3=18.6875f;
    const float y=std::pow(std::clamp(nits/10000.f,0.f,1.f),m1);
    return std::pow((c1+c2*y)/(1+c3*y),m2);
}
}
