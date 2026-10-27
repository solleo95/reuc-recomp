#pragma once
#include "raster_effects_math.h"
#include <cstddef>

namespace uc {
// Shared Slang constant layouts; explicit padding matches both DXBC and DXIL.
struct NativeOcclusionParams {
    float viewport[4],view_a[4],view_b[2]; uint32_t region[2],ao_size[2];
    float radius=.5f,falloff=.6f; uint32_t slices=0,steps=0,noise=0,flags=0;
    float strength=1,reserved=0,depth_range[2],light[4]; uint32_t contact_size[2],reserved_size[2];
};
static_assert(sizeof(NativeOcclusionParams)==128 && offsetof(NativeOcclusionParams,depth_range)==88 && offsetof(NativeOcclusionParams,light)==96);
struct NativeShadowBlock { float rows[3][4],info[4]; uint32_t taps[4]; float to_light[4]; };
static_assert(sizeof(NativeShadowBlock)==96);
struct NativeBlurParams { uint32_t output_size[2],render_size[2]; float shutter; uint32_t samples,tile_size=16,reserved=0; };
static_assert(sizeof(NativeBlurParams)==32);
}
