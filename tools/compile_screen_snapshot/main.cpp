// Linux: shows the compile screen in each of its states and keeps every state as a BMP in the given folder.
//   uc_compile_screen_snapshot <language code> <folder>      (the launcher's picture is read from assets/launcher/ beside this program)
#include "compile_screen.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <language> <folder>\n", argv[0]); return 2; }
    const std::string folder = argv[2];
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    uc::CompileScreen screen(argv[1]);
    const auto keep = [&](const char* name) {
        const std::string path = folder + "/" + name + ".bmp";
#ifdef _WIN32
        _putenv_s("UC_COMPILE_SCREEN_PNG", path.c_str());
#else
        setenv("UC_COMPILE_SCREEN_PNG", path.c_str(), 1);
#endif
    };
    const auto wait = [] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); };   // update() draws at most every 50 ms
    keep("starting"); screen.show();
    keep("waiting"); screen.wait_for_peer("Waiting for the other player");
    keep("compiling-1"); wait(); screen.update(37, 120);
    keep("compiling-2"); wait(); screen.update(96, 120);
    keep("compiling-3"); wait(); screen.update(120, 120);
    SDL_Quit();
    return 0;
}
