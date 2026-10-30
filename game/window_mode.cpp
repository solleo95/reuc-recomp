#include "window_mode.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <stdexcept>
namespace uc {
bool cursor_release_key(WindowMode mode,SDL_Keycode key) {
    return mode==WindowMode::windowed && (key==SDLK_LALT || key==SDLK_RALT);
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
    if(!SDL_SetWindowFullscreenMode(window,selected) || !SDL_SetWindowFullscreen(window,fullscreen)) throw std::runtime_error(SDL_GetError());
    if(!SDL_SyncWindow(window)) {
#ifdef _WIN32
        throw std::runtime_error(SDL_GetError());
#else
        // Linux: gamescope's Xwayland (how Steam starts a game in Game Mode) does not finish the sync within SDL's time limit and leaves no error text; the window is fine.
        std::fprintf(stderr,"[window] the window manager did not confirm the mode in time; continuing\n");
#endif
    }
    if(bool(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=fullscreen || (fullscreen && bool(SDL_GetWindowFullscreenMode(window))!=(mode==WindowMode::exclusive))) {
#ifdef _WIN32
        throw std::runtime_error("requested window mode was not applied");
#else
        // Linux: the compositor decides (gamescope in Game Mode owns the window, Wayland answers asynchronously); a game that is on screen must not die over it.
        std::fprintf(stderr,"[window] requested %s mode is not confirmed by the compositor; continuing\n",fullscreen ? "fullscreen" : "windowed");
#endif
    }
}
}
