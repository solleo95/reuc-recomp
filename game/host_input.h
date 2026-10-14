#pragma once
#include "native_input.h"
#ifdef _WIN32
#include <intrin.h>
#endif
#include <SDL3/SDL.h>
namespace uc {
class HostInput {
public:
    void extent(uint32_t width,uint32_t height) { width_=width; height_=height; }
    bool event(const SDL_Event&,NativeInput&,bool captured); // True: recapture requested; first click is consumed.
private:
    uint32_t width_=0,height_=0;
    double wheel_remainder_=0;
};
}
