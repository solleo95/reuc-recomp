#include "pointer_feed.h"
#include <algorithm>
#include <cmath>
namespace uc {
SDL_Event pointer_event_to_sdl(const PointerEvent& event,const PointerWindow& window) {
    SDL_Event sdl{};
    sdl.type=SDL_EVENT_USER;   // "nothing": the event cannot be used
    const float density=window.density>0 && std::isfinite(window.density) ? window.density : 1.f;
    switch(event.kind) {
    case PointerEvent::Kind::button:
        if(!event.button) break;
        sdl.type=event.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        sdl.button.windowID=window.id; sdl.button.which=SDL_MouseID(event.device); sdl.button.button=event.button; sdl.button.down=event.down; sdl.button.clicks=1;
        sdl.button.timestamp=SDL_GetTicksNS();
        break;
    case PointerEvent::Kind::wheel:
        if(!std::isfinite(event.x)) break;
        sdl.type=SDL_EVENT_MOUSE_WHEEL;
        sdl.wheel.windowID=window.id; sdl.wheel.which=SDL_MouseID(event.device); sdl.wheel.y=std::clamp(event.x,-100.f,100.f); sdl.wheel.direction=SDL_MOUSEWHEEL_NORMAL;   // HostInput refuses more than 256 notches: a long turn is a hundred
        sdl.wheel.timestamp=SDL_GetTicksNS();
        break;
    case PointerEvent::Kind::relative:
        if(!std::isfinite(event.x) || !std::isfinite(event.y)) break;
        sdl.type=SDL_EVENT_MOUSE_MOTION;
        sdl.motion.windowID=window.id; sdl.motion.which=SDL_MouseID(event.device); sdl.motion.xrel=event.x; sdl.motion.yrel=event.y;
        sdl.motion.timestamp=SDL_GetTicksNS();
        break;
    case PointerEvent::Kind::absolute:
        if(!window.width || !window.height || !std::isfinite(event.x) || !std::isfinite(event.y) || event.x<0 || event.x>1 || event.y<0 || event.y>1) break;
        sdl.type=SDL_EVENT_MOUSE_MOTION;
        sdl.motion.windowID=window.id; sdl.motion.which=SDL_MouseID(event.device);
        sdl.motion.x=event.x*window.width/density; sdl.motion.y=event.y*window.height/density;
        sdl.motion.timestamp=SDL_GetTicksNS();
        break;
    }
    return sdl;
}
bool feed_pointer_event(HostInput& host,NativeInput& input,const PointerEvent& event,const PointerWindow& window,bool captured) {
    const SDL_Event sdl=pointer_event_to_sdl(event,window);
    if(sdl.type==SDL_EVENT_USER) return false;
    host.extent(window.width,window.height);
    host.density(window.density>0 && std::isfinite(window.density) ? window.density : 1.f);
    if(event.kind==PointerEvent::Kind::relative || event.kind==PointerEvent::Kind::absolute) host.pointer(event.kind==PointerEvent::Kind::absolute ? PointerMode::absolute : PointerMode::relative);
    return host.event(sdl,input,captured);
}
}
