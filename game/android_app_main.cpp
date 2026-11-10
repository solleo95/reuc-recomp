// The Android app's entry: SDLActivity (android/) loads libmain.so and calls SDL_main, which SDL_main.h makes of the main below.
// An app's stdout and stderr go nowhere, so a thread copies them line by line into logcat (tag "uc") before the game starts; the session logs in
// <install>/logs keep their own copy as on the other platforms.
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

int uc_program_main(int argc, char** argv);   // game/uc_main.cpp

namespace {
void forward_to_logcat() {
    int ends[2];
    if (pipe(ends) != 0) return;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(ends[1], STDOUT_FILENO);
    dup2(ends[1], STDERR_FILENO);
    close(ends[1]);
    std::thread([reader = ends[0]] {
        std::string line;
        char buffer[4096];
        for (ssize_t n; (n = read(reader, buffer, sizeof buffer)) > 0;)
            for (ssize_t i = 0; i < n; ++i) {
                if (buffer[i] != '\n') { line += buffer[i]; continue; }
                __android_log_write(ANDROID_LOG_INFO, "uc", line.c_str());
                line.clear();
            }
    }).detach();
}
}  // namespace

// The process ends with the game: SDLActivity would keep it (and the game's static state) for the next start of the activity, which this game
// cannot run twice in one process.
int main(int argc, char** argv) {
    forward_to_logcat();
    const int result = uc_program_main(argc, argv);
    std::fflush(stdout);
    std::fflush(stderr);
    usleep(300000);   // the logcat thread takes the last lines
    _exit(result);
}
