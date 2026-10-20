#pragma once
#include "dlss5_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace uc {
// The bridge around the network (shaders/dlss5_bridge.slang repeats these three formulas): the game's RGBA8 frame is turned into the signal the network
// gets and back. Decode takes the stored value to the network's signal, encode is its inverse.
constexpr double kDlss5ReferenceWhiteNits=203;   // diffuse white of an SDR frame in the HDR reference (BT.2408)
inline float dlss5_decode(Dlss5Encoding encoding, float value) {
    switch(encoding) {
    case Dlss5Encoding::automatic:
    case Dlss5Encoding::srgb: return value<=.04045f ? value/12.92f : std::pow((value+.055f)/1.055f,2.4f);
    case Dlss5Encoding::gamma22: return std::pow(value,2.2f);
    case Dlss5Encoding::gamma24: return std::pow(value,2.4f);
    case Dlss5Encoding::linear: return value;
    }
    return value;
}
inline float dlss5_encode(Dlss5Encoding encoding, float value) {
    switch(encoding) {
    case Dlss5Encoding::automatic:
    case Dlss5Encoding::srgb: return value<=.0031308f ? value*12.92f : 1.055f*std::pow(value,1/2.4f)-.055f;
    case Dlss5Encoding::gamma22: return std::pow(value,1/2.2f);
    case Dlss5Encoding::gamma24: return std::pow(value,1/2.4f);
    case Dlss5Encoding::linear: return value;
    }
    return value;
}
// Multiplier applied to the signal before the network and removed after it: the manual exposure times the chosen diffuse white relative to the reference.
inline double dlss5_gain(const Dlss5Settings& settings) {
    const double exposure=settings.exposure_mode==Dlss5ExposureMode::manual ? settings.exposure : 1.0;
    return exposure*(settings.diffuse_white_override ? settings.diffuse_white_nits/kDlss5ReferenceWhiteNits : 1.0);
}
struct Dlss5Extent { uint32_t width, height; };
// The network's size: the frame scaled, rounded down to even, never below 8.
inline Dlss5Extent dlss5_processing_extent(uint32_t width, uint32_t height, double scale) {
    const auto size=[&](uint32_t value) { return std::max<uint32_t>(8,uint32_t(std::floor(value*scale))&~1u); };
    return {size(width),size(height)};
}
}
