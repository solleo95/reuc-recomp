#pragma once
#include "render_scale.h"
#include "upscale_mode.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace uc {
// Output / internal per axis (DLSS and FSR use the same ratios).
inline double upscale_ratio(UpscaleMode mode) {
    switch(mode) {
    case UpscaleMode::native: return 1.0;
    case UpscaleMode::quality: return 1.5;
    case UpscaleMode::balanced: return 1.724;
    case UpscaleMode::performance: return 2.0;
    case UpscaleMode::ultra_performance: return 3.0;
    }
    return 1.0;
}
inline RenderExtent internal_extent(RenderExtent output, UpscaleMode mode, RenderExtent minimum={1,1}) {
    const double ratio=upscale_ratio(mode);
    const auto axis=[&](uint32_t size,uint32_t floor) { return std::max({uint32_t(std::llround(size/ratio)),floor,1u}); };
    return {axis(output.width,minimum.width),axis(output.height,minimum.height)};
}
// FSR's ffxFsr3GetJitterPhaseCount: 8 * (output/internal)^2, rounded up; used for DLSS too.
inline uint32_t jitter_phase_count(uint32_t internal_width, uint32_t output_width) {
    const double ratio=double(output_width)/internal_width;
    return uint32_t(std::ceil(8*ratio*ratio-1e-9));
}
inline float halton(uint32_t index, uint32_t base) {
    float f=1, r=0;
    for(;index;index/=base) { f/=base; r+=f*(index%base); }
    return r;
}
// Sub-pixel offset in internal pixels, -0.5..0.5: Halton(2,3) at index frame % phases + 1.
inline std::array<float,2> jitter_offset(uint64_t frame, uint32_t phases) {
    const auto index=uint32_t(frame%std::max(phases,1u))+1;
    return {halton(index,2)-0.5f,halton(index,3)-0.5f};
}
// Texture LOD bias for scene draws (DLSS and FSR guides): log2(internal/output) - 1.
inline float mip_bias(RenderExtent internal, RenderExtent output) { return float(std::log2(double(internal.width)/output.width)-1.0); }
// RCAS attenuation for a 0.01..1 sharpness, FidelityFX remap: stops = 2 - 2 * sharpness, constant = 2^-stops.
inline float rcas_constant(double sharpness) { return float(std::exp2(-(2.0-2.0*sharpness))); }
// Streamline DLSSPreset per mode (owner's choice, matches NVIDIA's defaults): K = 11, L = 12, M = 13.
inline uint32_t dlss_preset(UpscaleMode mode) {
    return mode==UpscaleMode::performance ? 13u : mode==UpscaleMode::ultra_performance ? 12u : 11u;
}
}
