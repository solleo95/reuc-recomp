#pragma once
#include "native_input.h"
#include "pointer_settings.h"
#ifdef _WIN32
#ifdef _WIN32
#include <intrin.h>
#endif
#endif
#include <SDL3/SDL.h>
namespace uc {
class HostInput {
public:
    void extent(uint32_t width,uint32_t height) { width_=width; height_=height; }
    void sensitivity(uint32_t percent) { sensitivity_=percent/100.f; }   // scales the aim only; gestures (shake, swipes) keep the raw motion
    void pointer(PointerMode mode) { pointer_=mode; }   // absolute: the cursor's window position is the pointer (lightguns); the smoothing is NativeInput's
    void density(float pixels_per_unit) { density_=pixels_per_unit; }   // window pixels per unit of the event coordinates (SDL_GetWindowPixelDensity)
    void touch_pointer(bool on) { touch_pointer_=on; }   // the pointer is a touchscreen (the Steam Deck): taps, held fingers and swipes instead of a mouse's left button
    bool event(const SDL_Event&,NativeInput&,bool captured); // True: recapture requested; first click is consumed.
    bool take_recentre() { return std::exchange(recentre_,false); }   // True once after the trackpad's cursor came near the window's edge: warp it to the middle
    void update(NativeInput&,uint64_t now_ns);   // once per loop: the delayed Confirm of a touch in a menu (SDL_GetTicksNS clock)
private:
    void touch_down(NativeInput&);
    void touch_up(NativeInput&);
    bool touch_pointer_=false,touch_in_menu_=false;   // touch_in_menu_: the current touch began outside a 3D scene and acts as Confirm
    PointerMode pointer_=PointerMode::relative;
    float density_=1;
    uint32_t width_=0,height_=0;
    double wheel_remainder_=0;
    float sensitivity_=1;
    bool recentre_=false;
    bool touching_=false;      // a finger is on the screen: the aim follows it and the fire is held
    SDL_FingerID finger_=0;     // the first finger of the touch; the others are ignored
    float cursor_x_=0,cursor_y_=0;   // the cursor's last place in window pixels: where a touch lands
    // A menu highlights the item under the pointer a frame or two after the pointer arrives, and Confirm acts on the highlighted item: the touch's Confirm waits for it
    // (kMenuPressDelayNs) and stays down for at least kMenuHoldNs, so a quick tap is one tap, not two.
    enum class MenuPress:uint8_t { none,waiting,down } menu_press_=MenuPress::none;
    uint64_t menu_press_at_=0;
};
}
