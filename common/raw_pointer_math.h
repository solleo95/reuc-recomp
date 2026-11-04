#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace uc {
struct PixelRect { int left=0,top=0,width=0,height=0; };
// Windows Raw Input reports an absolute pointing device (a light gun, a tablet) as 0..65535 over the primary monitor, or over the whole virtual desktop when the report says so.
// The position inside the window's client area, 0..1 with y down, clamped: a linear map of the raw value, so nothing is added up and the aim is exact from the middle to the edges.
// A window or desktop without size gives the middle.
inline std::array<float,2> raw_absolute_to_window(int32_t last_x,int32_t last_y,PixelRect desktop,PixelRect window_client) {
    if(desktop.width<=0 || desktop.height<=0 || window_client.width<=0 || window_client.height<=0) return {.5f,.5f};
    const double screen_x=desktop.left+double(last_x)/65535.0*desktop.width,screen_y=desktop.top+double(last_y)/65535.0*desktop.height;
    return {float(std::clamp((screen_x-window_client.left)/window_client.width,0.0,1.0)),float(std::clamp((screen_y-window_client.top)/window_client.height,0.0,1.0))};
}
}
