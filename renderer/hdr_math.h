#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
namespace uc {
// HDR output (graphics.hdr*): the game's SDR frame expanded into HDR10 at the present. Mirrored in shaders/hdr.slang.
#ifdef _WIN32
inline constexpr bool kHdrDefault=false;
#else
inline constexpr bool kHdrDefault=true;   // Linux (Steam Deck OLED): on; the renderer falls back to SDR when the display offers no HDR
#endif
struct HdrSettings {
    bool enabled=kHdrDefault;
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
// The bloom's glow (BloomPass::glow_view, scene-referred: 1.0 = paper white) added in nits to the expanded luminance, never above the peak. The shader adds it
// per colour channel and then keeps the hue of whatever passes the peak; this is the single-value form tested here.
inline float hdr_with_glow(float y, float glow, float paper, float peak) { return std::min(hdr_expand(y,paper,peak)+std::max(0.f,glow)*paper,peak); }
// The game's own interface (its HUD, its menus) is drawn into the SDR picture, so its brightness is set where it is drawn: the draws that gx::mark_ui_draws picks carry
// this factor (1..254 of 255, in the picture's gamma space; the shader multiplies the colour by it) and the encode then shows them at about `ui_nits`. 0 = no change: HDR
// off, or the interface at least as bright as paper white (an 8-bit picture cannot be brightened this way).
inline uint8_t hdr_ui_dim(const HdrSettings& s) {
    if(!s.enabled || s.ui_nits>=s.paper_white_nits) return 0;
    return uint8_t(std::clamp(std::lround(255.f*std::pow(float(s.ui_nits)/float(s.paper_white_nits),1.f/2.2f)),1l,254l));
}
inline std::atomic<uint8_t> g_hdr_ui_dim{0};   // what the sealing of packets reads (producer thread); each renderer publishes it for its own HDR state
inline void publish_hdr_ui_dim(const HdrSettings& s, bool active) { g_hdr_ui_dim.store(active ? hdr_ui_dim(s) : 0, std::memory_order_relaxed); }
// SMPTE ST 2084 inverse EOTF: absolute nits -> PQ signal 0..1.
inline float pq_encode(float nits) {
    const float m1=.1593017578125f, m2=78.84375f, c1=.8359375f, c2=18.8515625f, c3=18.6875f;
    const float y=std::pow(std::clamp(nits/10000.f,0.f,1.f),m1);
    return std::pow((c1+c2*y)/(1+c3*y),m2);
}
}
