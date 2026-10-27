#pragma once
#include "gx_transforms.h"
#include "render_scale.h"
#include <span>

namespace uc {
struct NativeViewport {
    EfbViewport viewport;
    std::array<int32_t,4> clip; // left, top, right, bottom; pixel centres inside the original viewport
};
// D3D's implicit viewport scissor floors each edge. Integral bounds preserve GX's fractional calibration without losing the last row/column.
inline NativeViewport native_viewport(EfbViewport original,gx::Projection& projection,std::span<float,2> shift) {
    const float right=original.x+original.width,bottom=original.y+original.height;
    if(!std::isfinite(original.x) || !std::isfinite(original.y) || !std::isfinite(right) || !std::isfinite(bottom) || original.width<=0 || original.height<=0 ||
       original.x<-32768 || original.y<-32768 || right>32767 || bottom>32767) throw std::invalid_argument("invalid native viewport bounds");
    NativeViewport result{{std::floor(original.x),std::floor(original.y),0,0},
        {int32_t(std::ceil(original.x-.5f)),int32_t(std::ceil(original.y-.5f)),int32_t(std::ceil(right-.5f)),int32_t(std::ceil(bottom-.5f))}};
    result.viewport.width=std::ceil(right)-result.viewport.x; result.viewport.height=std::ceil(bottom)-result.viewport.y;
    const float sx=original.width/result.viewport.width,sy=original.height/result.viewport.height;
    const float tx=(2*(original.x-result.viewport.x)+original.width-result.viewport.width)/result.viewport.width;
    const float ty=(2*(original.y-result.viewport.y)+original.height-result.viewport.height)/result.viewport.height;
    // GX clip Y points down; the shared vertex shader flips it for D3D after this projection and shift.
    for(size_t i=0;i<4;++i) { projection[0][i]=projection[0][i]*sx+projection[3][i]*tx; projection[1][i]=projection[1][i]*sy+projection[3][i]*ty; }
    shift[0]*=sx; shift[1]*=sy;
    return result;
}
// Camera fallback consumes already compensated clip coordinates, so change both matrix bases.
inline gx::Projection native_camera_motion(EfbViewport viewport,const gx::Projection& camera) {
    gx::Projection transform{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    std::array<float,2> shift{};native_viewport(viewport,transform,shift);
    gx::Projection inverse{{{1/transform[0][0],0,0,-transform[0][3]/transform[0][0]},
        {0,1/transform[1][1],0,-transform[1][3]/transform[1][1]},{0,0,1,0},{0,0,0,1}}};
    gx::Projection intermediate{},result{};
    for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col)for(size_t k=0;k<4;++k)intermediate[row][col]+=camera[row][k]*inverse[k][col];
    for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col)for(size_t k=0;k<4;++k)result[row][col]+=transform[row][k]*intermediate[k][col];
    return result;
}
}
