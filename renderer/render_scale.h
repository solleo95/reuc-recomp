#pragma once
#include <algorithm>
#include <array>
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
// Client size of a windowed start: the selected render resolution, shrunk with the same aspect when the desktop's usable area
// cannot hold it together with the window frame (borders + title bar, approximated by kWindowFrame).
inline constexpr uint32_t kWindowFrameWidth=16,kWindowFrameHeight=48;
inline RenderExtent windowed_extent(double scale,double aspect,uint32_t usable_width,uint32_t usable_height) {
    if(usable_width<=kWindowFrameWidth || usable_height<=kWindowFrameHeight) throw std::invalid_argument("desktop usable area too small");
    auto extent=manual_render_extent(scale,aspect);
    const uint64_t available_width=usable_width-kWindowFrameWidth,available_height=usable_height-kWindowFrameHeight;
    if(extent.width>available_width || extent.height>available_height) {
        const bool width_limits=available_width*extent.height<available_height*extent.width;   // the tighter axis decides
        const RenderExtent original=extent;
        if(width_limits) extent={uint32_t(available_width),uint32_t(available_width*original.height/original.width)};
        else extent={uint32_t(available_height*original.width/original.height),uint32_t(available_height)};
    }
    return extent;
}
struct EfbViewport { float x,y,width,height; };
// GX rasterizes without AA at pixel centre 7/12, Vulkan at 1/2: shift geometry by -1/12 EFB pixel (Dolphin same-frame metrics minimal there).
inline EfbViewport efb_viewport(const std::array<float,6>& v,uint32_t width,uint32_t height) {
    const float sx=width/640.f,sy=height/480.f;
    return {(v[0]-1.f/12)*sx,(v[1]-1.f/12)*sy,v[2]*sx,v[3]*sy};
}
inline double device_render_scale(double scale,uint32_t max_image_dimension,double aspect=16.0/9) {
    manual_render_extent(scale,aspect);
    const double supported=std::min((uint64_t(max_image_dimension)*640/1024)/(720*aspect),(uint64_t(max_image_dimension)*480/1024)/960.0);
    if(max_image_dimension<1024 || supported<0.5) throw std::invalid_argument("unsupported internal render scale");
    return std::min(scale,supported); // Every valid logical copy may reach1024px on either axis.
}
}
