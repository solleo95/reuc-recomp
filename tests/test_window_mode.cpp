#include "check.h"
#include "window_mode.h"
#include <SDL3/SDL.h>
#include <stdexcept>
int main() try {
    CHECK(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_VIDEODRIVER","dummy",true));
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    for(auto mode:{uc::WindowMode::windowed,uc::WindowMode::borderless,uc::WindowMode::exclusive}) {
        CHECK(uc::cursor_release_key(mode,SDLK_ESCAPE));
        CHECK(uc::cursor_release_key(mode,SDLK_LALT)==(mode==uc::WindowMode::windowed));
        CHECK(uc::cursor_release_key(mode,SDLK_RALT)==(mode==uc::WindowMode::windowed));
        CHECK(!uc::cursor_release_key(mode,SDLK_A));
    }
    auto* window=SDL_CreateWindow("Window mode self-check",640,360,SDL_WINDOW_HIDDEN);
    CHECK(window);
    for(auto mode:{uc::WindowMode::windowed,uc::WindowMode::borderless,uc::WindowMode::windowed}) {
        uc::configure_window_mode(window,mode,640,360);
        CHECK(bool(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)==(mode!=uc::WindowMode::windowed));
    }
    // Dummy video has no exclusive display modes; failure preserves Windowed state.
    bool unsupported=false; try { uc::configure_window_mode(window,uc::WindowMode::exclusive,640,360); } catch(const std::runtime_error&) { unsupported=true; }
    CHECK(unsupported && !(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));
    bool rejected=false; try { uc::configure_window_mode(nullptr,uc::WindowMode::windowed,640,360); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    rejected=false; try { uc::configure_window_mode(window,static_cast<uc::WindowMode>(99),640,360); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    SDL_DestroyWindow(window); SDL_Quit();
} catch(const std::exception& error) {
    std::fprintf(stderr,"window mode self-check: %s\n",error.what()); SDL_Quit(); return 1;
}
