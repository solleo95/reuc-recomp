#include "compile_screen.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// Linux: the progress screen while the pipelines compile (compile_screen_win.cpp draws a topmost window with GDI). A small SDL window with a bar and a
// counter; fullscreen when the game was started from Steam (Game Mode: gamescope shows nothing else until a window appears, a first start compiles for a
// minute or more). The software renderer keeps it clear of the Vulkan device the compile is about to use.
// shortcut: the text is SDL's built-in 8x8 debug font, English only; draw it with the menu's ImGui fonts if the screen should speak the launcher's language.
namespace uc {
struct CompileScreen::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    size_t done = 0, total = 0, last_logged_step = 0;
    std::string message = "Starting";
    std::chrono::steady_clock::time_point last_draw{};
    void draw() {
        if (!renderer) return;
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        SDL_SetRenderScale(renderer, 1.f, 1.f);
        SDL_SetRenderDrawColor(renderer, 0x14, 0x18, 0x19, 255);
        SDL_RenderClear(renderer);
        // Text in SDL's 8x8 font at a whole-number scale: `wanted` for the height, reduced until the line fits in 90 % of the width (nothing is ever clipped).
        const float base = std::floor(std::min(float(width) / 360.f, float(height) / 200.f));
        const auto text = [&](float wanted, float y, const std::string& line, Uint8 r, Uint8 g, Uint8 b) {
            float scale = std::max(1.f, wanted);
            while (scale > 1.f && 8.f * scale * float(line.size()) > float(width) * 0.9f) scale -= 1.f;
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_SetRenderScale(renderer, scale, scale);
            SDL_RenderDebugText(renderer, (float(width) - 8.f * scale * float(line.size())) / 2.f / scale, y / scale, line.c_str());
            SDL_SetRenderScale(renderer, 1.f, 1.f);
        };
        const float h = float(height);
        text(base + 1, h * 0.14f, "RESIDENT EVIL", 0xEE, 0xE9, 0xDC);
        text(base, h * 0.14f + 10.f * (base + 1), "The Umbrella Chronicles", 0xEE, 0xE9, 0xDC);
        text(base, h * 0.50f, total ? "Preparing graphics  " + std::to_string(done) + " / " + std::to_string(total) : message, 0xEE, 0xE9, 0xDC);
        if (total) text(std::max(1.f, base - 1), h * 0.50f + 10.f * base, "This takes long only on the first start", 0x8F, 0x93, 0x94);
        const float bar_height = std::max(8.f, h * 0.03f);
        const SDL_FRect frame{float(width) * 0.15f, h * 0.76f, float(width) * 0.7f, bar_height};
        SDL_SetRenderDrawColor(renderer, 0x8F, 0x93, 0x94, 255);
        SDL_RenderRect(renderer, &frame);
        if (total) {
            const SDL_FRect fill{frame.x + 2, frame.y + 2, (frame.w - 4) * float(done) / float(total), frame.h - 4};
            SDL_SetRenderDrawColor(renderer, 0xC0, 0x1F, 0x2F, 255);   // Umbrella red
            SDL_RenderFillRect(renderer, &fill);
        }
        if (const char* png = std::getenv("UC_COMPILE_SCREEN_PNG")) {   // diagnostics: keep the last frame as a BMP picture (what the player sees; the name says PNG for history, the file is a BMP)
            if (SDL_Surface* frame_pixels = SDL_RenderReadPixels(renderer, nullptr)) { SDL_SaveBMP(frame_pixels, png); SDL_DestroySurface(frame_pixels); }
        }
        SDL_RenderPresent(renderer);
        last_draw = std::chrono::steady_clock::now();
    }
};

CompileScreen::CompileScreen(std::string_view) : state_(new State) {}
CompileScreen::~CompileScreen() {
    if (state_->renderer) SDL_DestroyRenderer(state_->renderer);
    if (state_->window) SDL_DestroyWindow(state_->window);
    delete state_;
}
void CompileScreen::show() {
    std::fprintf(stderr, "[compile] starting\n");
    if (state_->window) return;
    const bool from_steam = std::getenv("SteamGameId") || std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    const SDL_WindowFlags flags = from_steam ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_BORDERLESS;
    state_->window = SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", from_steam ? 1280 : 720, from_steam ? 800 : 360, flags);
    if (!state_->window) { std::fprintf(stderr, "[compile] no progress window: %s\n", SDL_GetError()); return; }
    state_->renderer = SDL_CreateRenderer(state_->window, "software");
    if (!state_->renderer) { std::fprintf(stderr, "[compile] no progress renderer: %s\n", SDL_GetError()); return; }
    state_->draw();
}
void CompileScreen::wait_for_peer(std::string_view message) {
    std::fprintf(stderr, "[compile] waiting: %s\n", std::string(message).c_str());
    state_->message = std::string(message);
    state_->total = 0;
    state_->draw();
    SDL_PumpEvents();
}
void CompileScreen::update(size_t done, size_t total) {
    state_->done = done;
    state_->total = total;
    if (total) {
        const size_t step = done * 10 / total;
        if (step != state_->last_logged_step || done == total) { state_->last_logged_step = step; std::fprintf(stderr, "[compile] %zu / %zu\n", done, total); }
    }
    if (std::chrono::steady_clock::now() - state_->last_draw > std::chrono::milliseconds(50)) state_->draw();
    SDL_PumpEvents();   // keeps the compositor from calling the window unresponsive
}
}  // namespace uc
