#include "window_mode.h"
#include <SDL3/SDL.h>
#include <stdexcept>
namespace uc {
bool cursor_release_key(WindowMode mode,SDL_Keycode key) {
    return key==SDLK_ESCAPE || (mode==WindowMode::windowed && (key==SDLK_LALT || key==SDLK_RALT));
}
void configure_window_mode(SDL_Window* window,WindowMode mode,int width,int height) {
    if(!window || width<=0 || height<=0) throw std::invalid_argument("invalid window/output extent");
    SDL_DisplayMode exclusive{};
    const SDL_DisplayMode* selected=nullptr;
    switch(mode) {
    case WindowMode::windowed:
    case WindowMode::borderless: break;
    case WindowMode::exclusive: {
        const auto display=SDL_GetDisplayForWindow(window);
        if(!display || !SDL_GetClosestFullscreenDisplayMode(display,width,height,0,true,&exclusive)) throw std::runtime_error(SDL_GetError());
        selected=&exclusive; break;
    }
    default: throw std::invalid_argument("invalid window mode");
    }
    const bool fullscreen=mode!=WindowMode::windowed;
    if(!SDL_SetWindowFullscreenMode(window,selected) || !SDL_SetWindowFullscreen(window,fullscreen) || !SDL_SyncWindow(window)) throw std::runtime_error(SDL_GetError());
    if(bool(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=fullscreen || (fullscreen && bool(SDL_GetWindowFullscreenMode(window))!=(mode==WindowMode::exclusive))) throw std::runtime_error("requested window mode was not applied");
}
}
