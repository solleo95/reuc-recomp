#pragma once
#include "gx_transforms.h"
#include <array>
namespace uc {
// Rebuilds a view-space position from a pixel's NDC xy and its stored depth, for a GX perspective projection
// (rows {a,0,b,0} {0,c,d,0} {0,0,e,f} {0,0,-1,0}; depth = clip.z / clip.w in 0..1). The screen-space effects read it through shaders/view_space.slang,
// which does the same arithmetic.
//   ndc.x = (a x + b z) / -z,  ndc.y = (c y + d z) / -z,  depth = (e z + f) / -z   =>   z = -f / (depth + e),  x = (ndc.x + b) (-z) / a,  y = (ndc.y + d) (-z) / c
struct ViewParams { float x_scale, y_scale, x_offset, y_offset, depth_a, depth_b; };
inline ViewParams view_params(const gx::Projection& projection) {
    return {projection[0][0],projection[1][1],projection[0][2],projection[1][2],projection[2][2],projection[2][3]};
}
inline std::array<float,3> reconstruct_view(const ViewParams& params, float ndc_x, float ndc_y, float depth) {
    const float z=-params.depth_b/(depth+params.depth_a);
    return {(ndc_x+params.x_offset)*-z/params.x_scale,(ndc_y+params.y_offset)*-z/params.y_scale,z};
}
}
