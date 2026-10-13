#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <stdexcept>
namespace uc {
struct RenderExtent { uint32_t width,height; };
inline RenderExtent manual_render_extent(double scale,double aspect=16.0/9) {
    if(!std::isfinite(scale) || scale<0.5 || scale>6 || !std::isfinite(aspect) || aspect<0.25 || aspect>8) throw std::invalid_argument("invalid720p render multiplier/aspect");
    const RenderExtent extent{uint32_t(std::llround(720*aspect*scale)),uint32_t(std::llround(720*scale))};
    if(!extent.width || !extent.height || extent.width>7680 || extent.height>4320) throw std::invalid_argument("internal resolution exceeds8K");
    return extent;
}
inline double monitor_render_scale(uint32_t width,uint32_t height) {
    if(!width || !height) throw std::invalid_argument("invalid monitor extent");
    const double aspect=double(width)/height;
    manual_render_extent(0.5,aspect);
    const auto scale=std::clamp(height/720.0,0.5,std::min(6.0,7680/(720*aspect)));
    manual_render_extent(scale,aspect);
    return scale;
}
inline double device_render_scale(double scale,uint32_t max_image_dimension,double aspect=16.0/9) {
    manual_render_extent(scale,aspect);
    const double supported=std::min((uint64_t(max_image_dimension)*640/1024)/(720*aspect),(uint64_t(max_image_dimension)*480/1024)/960.0);
    if(max_image_dimension<1024 || supported<0.5) throw std::invalid_argument("unsupported internal render scale");
    return std::min(scale,supported); // Every valid logical copy may reach1024px on either axis.
}
}
