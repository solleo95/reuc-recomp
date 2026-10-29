#pragma once
#include "native_input.h"
#include "pointer_settings.h"
#ifdef _WIN32
#include <intrin.h>
#endif
#include <SDL3/SDL.h>
namespace uc {
class HostInput {
public:
    void extent(uint32_t width,uint32_t height) { width_=width; height_=height; }
    void sensitivity(uint32_t percent) { sensitivity_=percent/100.f; }   // scales the aim only; gestures (shake, swipes) keep the raw motion
    void pointer(PointerMode mode) { pointer_=mode; }   // absolute: the cursor's window position is the pointer (lightguns); the smoothing is NativeInput's
    void density(float pixels_per_unit) { density_=pixels_per_unit; }   // window pixels per unit of the event coordinates (SDL_GetWindowPixelDensity)
    bool event(const SDL_Event&,NativeInput&,bool captured); // True: recapture requested; first click is consumed.
private:
    PointerMode pointer_=PointerMode::relative;
    float density_=1;
    uint32_t width_=0,height_=0;
    double wheel_remainder_=0;
    float sensitivity_=1;
};
}
