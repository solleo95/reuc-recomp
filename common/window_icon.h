#pragma once
#include "window_icon_data.h"
#include <SDL3/SDL.h>
namespace uc {
// Linux: the game's icon in the window (title bar, task switcher). Without it the compositor shows its generic Wayland icon. Windows takes the icon from assets/app.rc.
inline void set_window_icon(SDL_Window* window) {
    SDL_Surface* icon = SDL_CreateSurfaceFrom(kWindowIconSize, kWindowIconSize, SDL_PIXELFORMAT_RGBA32, const_cast<unsigned char*>(kWindowIconPixels), kWindowIconSize * 4);
    if (!icon) return;   // decoration: the window works without it
    SDL_SetWindowIcon(window, icon);   // SDL copies the pixels
    SDL_DestroySurface(icon);
}
}
