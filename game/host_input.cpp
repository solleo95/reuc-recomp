#include "host_input.h"
#include <cmath>
#include <stdexcept>
namespace uc {
bool HostInput::event(const SDL_Event& event,NativeInput& input,bool captured) {
    switch(event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST: input.focus(false); wheel_remainder_=0; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: input.focus(captured); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if(event.key.repeat || event.key.scancode<=SDL_SCANCODE_UNKNOWN || event.key.scancode>=SDL_SCANCODE_COUNT)break;
        input.host_button({PhysicalKind::key,uint16_t(event.key.scancode)},event.type==SDL_EVENT_KEY_DOWN);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if(!event.button.button)break;
        if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && !captured)return true;
        input.host_button({PhysicalKind::mouse,event.button.button},event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        if(!captured)break;
        const double delta=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        if(!std::isfinite(delta) || std::abs(delta)>256) { input.focus(false); throw std::invalid_argument("invalid host wheel delta"); }
        wheel_remainder_+=delta;
        const int steps=int(wheel_remainder_);
        wheel_remainder_-=steps; input.host_wheel(steps);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if(captured && input.move_raw(event.motion.xrel,event.motion.yrel,width_,height_))
            input.host_motion(event.motion.xrel,event.motion.yrel,event.motion.timestamp);
        break;
    default: break;
    }
    return false;
}
}
