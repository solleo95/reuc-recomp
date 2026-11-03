#pragma once
#include <cstdint>
namespace uc {
// A draw that tests depth without writing it and blends over what is there (bullet holes, marks laid on a surface) lies in the plane of the surface under it,
// so the GameCube's depth offset is not enough in a float depth buffer and it flickers (z-fighting): every backend pulls such draws slightly towards the camera.
// `z` is the draw's depth word (bit 0 test, bits 1..3 compare, bit 4 write), `blend_mode` the GX blend mode (1 = blend).
inline constexpr bool decal_depth_bias(uint32_t blend_mode,uint32_t z) { return blend_mode==1 && (z&17)==1; }
inline constexpr int kDecalDepthBias=-8;          // in smallest depth steps of the primitive (same meaning in Vulkan, D3D11 and D3D12)
inline constexpr float kDecalSlopeBias=-2.f;      // times the primitive's depth slope: seat backs and other tilted surfaces
}
