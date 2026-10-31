#pragma once
#include "graphics_effects.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace uc {
struct AoPreset { uint32_t slices,steps; bool full_resolution; };
inline AoPreset ao_preset(EffectLevel level) {
    switch(level) {
    case EffectLevel::low: return {1,2,false};
    case EffectLevel::medium: return {2,2,false};
    case EffectLevel::high: return {3,3,true};
    case EffectLevel::ultra: return {9,3,true};
    case EffectLevel::off: break;
    }
    return {0,0,false};
}
struct BlurPreset { float shutter; uint32_t samples; };
inline BlurPreset blur_preset(EffectLevel level) {
    switch(level) {
    case EffectLevel::low: return {0.3f,8};
    case EffectLevel::medium: return {0.5f,12};
    case EffectLevel::high: return {0.7f,20};
    case EffectLevel::ultra: return {0.7f,32};
    case EffectLevel::off: break;
    }
    return {0,0};
}
// Samples a blur of `length` output pixels needs: two per pixel of its length, at least 8, at most the preset's count; `full` (UC_BLUR_FULL_SAMPLES, a debugging aid)
// keeps the preset's count. shaders/motion_blur.slang computes the same.
inline uint32_t blur_sample_count(uint32_t preset_samples, float length, bool full) {
    if (full) return preset_samples;
    return std::min(preset_samples, std::max(8u, uint32_t(std::ceil(2.0f * length))));
}
// The unblurred centre sample counts as many times less as the blur takes fewer samples than the preset (1 with the full count): a short blur with 8 of 32 samples is not
// sharper than a long one. shaders/motion_blur.slang computes the same.
inline float blur_centre_weight(uint32_t count, uint32_t preset_samples) { return preset_samples ? float(count) / float(preset_samples) : 1.f; }
inline bool blur_full_samples() { const char* value = std::getenv("UC_BLUR_FULL_SAMPLES"); return value && *value == '1'; }
}
