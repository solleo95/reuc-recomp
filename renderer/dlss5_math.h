#pragma once
#include "dlss5_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace uc {
// The bridge around the network (shaders/dlss5_bridge.slang repeats these formulas). Signal value L is linear with 1.0 = diffuse white (diffuse_white_nits); a stored
// RGBA8 value is turned into L by the chosen Encoding, scaled by the exposure gain, brought to the Scaling Domain for the filters and to the NR Signal Encoding for the
// network; the way back is the inverse of each step. All functions are per channel and keep the sign of their input (filters overshoot below zero).
inline float dlss5_sign(float v) { return v<0 ? -1.f : 1.f; }
inline float dlss5_srgb_eotf(float v) { const float a=std::fabs(v); return dlss5_sign(v)*(a<=.04045f ? a/12.92f : std::pow((a+.055f)/1.055f,2.4f)); }
inline float dlss5_srgb_oetf(float l) { const float a=std::fabs(l); return dlss5_sign(l)*(a<=.0031308f ? a*12.92f : 1.055f*std::pow(a,1/2.4f)-.055f); }
constexpr float kPqM1=.1593017578125f, kPqM2=78.84375f, kPqC1=.8359375f, kPqC2=18.8515625f, kPqC3=18.6875f;
inline float dlss5_pq_oetf(float nits) {   // absolute luminance in nits to the PQ signal 0..1
    const float y=std::pow(std::clamp(nits/10000.f,0.f,1.f),kPqM1);
    return std::pow((kPqC1+kPqC2*y)/(1+kPqC3*y),kPqM2);
}
inline float dlss5_pq_eotf(float e) {
    const float p=std::pow(std::clamp(e,0.f,1.f),1/kPqM2);
    return 10000.f*std::pow(std::max(p-kPqC1,0.f)/(kPqC2-kPqC3*p),1/kPqM1);
}
inline float dlss5_decode_source(const Dlss5Settings& s, float stored) {
    switch(s.encoding) {
    case Dlss5Encoding::automatic:
    case Dlss5Encoding::srgb:
    case Dlss5Encoding::scrgb_nl: return dlss5_srgb_eotf(stored);
    case Dlss5Encoding::linear709:
    case Dlss5Encoding::scrgb: return stored;
    case Dlss5Encoding::hdr10: return dlss5_pq_eotf(stored)/float(s.diffuse_white_nits);
    }
    return stored;
}
inline float dlss5_encode_source(const Dlss5Settings& s, float l) {
    switch(s.encoding) {
    case Dlss5Encoding::automatic:
    case Dlss5Encoding::srgb:
    case Dlss5Encoding::scrgb_nl: return dlss5_srgb_oetf(l);
    case Dlss5Encoding::linear709:
    case Dlss5Encoding::scrgb: return l;
    case Dlss5Encoding::hdr10: return dlss5_pq_oetf(l*float(s.diffuse_white_nits));
    }
    return l;
}
inline float dlss5_nr_encode(const Dlss5Settings& s, float l) {
    switch(s.nr_encoding) {
    case Dlss5NrEncoding::srgb: return dlss5_srgb_oetf(l);
    case Dlss5NrEncoding::pq_sdr: return dlss5_sign(l)*dlss5_pq_oetf(std::fabs(l)*float(s.diffuse_white_nits));
    case Dlss5NrEncoding::linear_sdr: return l;
    }
    return l;
}
inline float dlss5_nr_decode(const Dlss5Settings& s, float e) {
    switch(s.nr_encoding) {
    case Dlss5NrEncoding::srgb: return dlss5_srgb_eotf(e);
    case Dlss5NrEncoding::pq_sdr: return dlss5_sign(e)*dlss5_pq_eotf(std::fabs(e))/float(s.diffuse_white_nits);
    case Dlss5NrEncoding::linear_sdr: return e;
    }
    return e;
}
inline float dlss5_domain_to(const Dlss5Settings& s, float l) {
    switch(s.scaling_domain) {
    case Dlss5Domain::scrgb_nl: return dlss5_srgb_oetf(l);
    case Dlss5Domain::signed_pq: return dlss5_sign(l)*dlss5_pq_oetf(std::fabs(l)*float(s.diffuse_white_nits));
    case Dlss5Domain::linear: return l;
    }
    return l;
}
inline float dlss5_domain_from(const Dlss5Settings& s, float d) {
    switch(s.scaling_domain) {
    case Dlss5Domain::scrgb_nl: return dlss5_srgb_eotf(d);
    case Dlss5Domain::signed_pq: return dlss5_sign(d)*dlss5_pq_eotf(std::fabs(d))/float(s.diffuse_white_nits);
    case Dlss5Domain::linear: return d;
    }
    return d;
}
// Multiplier applied to the signal before the network and removed after it: Use Exposure Value On multiplies by the exposure, Inverse divides, Off leaves the signal.
inline double dlss5_pre_gain(const Dlss5Settings& s) {
    switch(s.exposure_value) {
    case Dlss5ExposureValue::on: return s.exposure;
    case Dlss5ExposureValue::inverse: return 1.0/s.exposure;
    case Dlss5ExposureValue::off: break;
    }
    return 1.0;
}
struct Dlss5Extent { uint32_t width, height; };
// The network's size: the frame scaled, rounded down to even, never below 8.
inline Dlss5Extent dlss5_processing_extent(uint32_t width, uint32_t height, double scale) {
    const auto size=[&](uint32_t value) { return std::max<uint32_t>(8,uint32_t(std::floor(value*scale))&~1u); };
    return {size(width),size(height)};
}
}
