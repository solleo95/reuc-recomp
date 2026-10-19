#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace uc {
// The own denoiser's temporal rules (rt_denoiser.h); shaders/rt_denoise.slang does the same arithmetic.
constexpr uint32_t kDenoiseMaxHistory=32;          // frames: the blend weight of a new sample never drops below 1/32
constexpr float kDenoiseDepthTolerance=0.02f;      // a reprojected history pixel is the same surface within 2% of the view depth...
constexpr float kDenoiseNormalCos=0.906307787f;    // ...and with normals at most 25 degrees apart (cos 25)
// Weight of the new sample when the accumulated history (this frame included) is `length` frames long.
inline float history_alpha(uint32_t length) { return std::max(1.f/float(std::max(length,1u)),1.f/float(kDenoiseMaxHistory)); }
// depth, history_depth: view distances (history 0 = no surface); normal_dot: dot of the current and the history normal.
inline bool reproject_valid(float depth, float history_depth, float normal_dot) {
    return std::fabs(depth-history_depth)<=kDenoiseDepthTolerance*depth && normal_dot>=kDenoiseNormalCos;
}
}
