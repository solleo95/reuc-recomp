#pragma once
#include "graphics_effects.h"

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
}
