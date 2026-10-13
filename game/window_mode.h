#pragma once
#include "options.h"
#ifdef _WIN32
#include <intrin.h> // Load Clang intrinsic guards before SDL3.2 compatibility definitions.
#endif
#include <SDL3/SDL_keycode.h>
struct SDL_Window;
namespace uc {
void configure_window_mode(SDL_Window* window,WindowMode mode,int width,int height);
bool cursor_release_key(WindowMode mode,SDL_Keycode key);
}
