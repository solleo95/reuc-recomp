#include "compile_screen.h"
#include "graphite.h"
#include "localization.h"
#include "path_io.h"
#include "ui_fonts.h"
#include "window_icon.h"
#include "imgui.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// The progress screen while the pipelines compile, the same on Windows and Linux: the launcher's train picture, darkened, behind the title and a progress bar, in the
// launcher's fonts and language. A borderless window of its own (Windows: topmost and never taking the focus, and hidden for the first 400 ms of the compile so a start with
// a warm driver cache never flashes it); fullscreen on Linux when the game was started from Steam (Game Mode: gamescope shows nothing else until a window appears, a first
// start compiles for a minute or more). SDL's software renderer keeps it clear of the Vulkan device the compile is about to use; ImGui draws the text and the bar on it
// through its own context (the game's menu context is left as it was).
namespace uc {
namespace {
constexpr ImU32 kIvory = IM_COL32(238, 233, 220, 255), kMuted = IM_COL32(168, 172, 172, 255);

ImVec2 text_size(ImFont* font, float size, const char* text) { return font->CalcTextSizeA(size, 1e9f, 0, text); }
void centered(ImDrawList& draw, ImFont* font, float size, float centre_x, float y, ImU32 color, const char* text) {
    draw.AddText(font, size, {std::round(centre_x - text_size(font, size, text).x / 2), std::round(y)}, color, text);
}
}

struct CompileScreen::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* backdrop = nullptr;   // the picture and its darkening, drawn once at the window's size
    ImGuiContext* context = nullptr;
    ui::Fonts fonts;
    ImFont* serif = nullptr;
    ImFont* sans = nullptr;
    bool backend = false, attempted = false;
    std::string language, starting, starting_detail, title, detail;
    size_t done = 0, total = 0, last_logged_step = 0;
    std::string message;   // what the game waits for (a peer in network play); empty: the compile's own title
    std::chrono::steady_clock::time_point last_draw{}, created = std::chrono::steady_clock::now();

    // The train picture fills the window like CSS "cover", then a dark veil (heavier towards the bottom, where the bar and the numbers are) keeps the text readable.
    void build_backdrop(int width, int height) {
        backdrop = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
        if (!backdrop) return;
        SDL_Texture* art = nullptr;
        try { art = launcher::load_banner(renderer, exe_path().parent_path() / "assets" / "launcher" / "graphite-train.png"); }
        catch (const std::exception& e) { std::fprintf(stderr, "[compile] background: %s\n", e.what()); }   // decoration: the plain colour is used instead
        SDL_SetRenderTarget(renderer, backdrop);
        SDL_SetRenderDrawColor(renderer, 0x14, 0x18, 0x19, 255);
        SDL_RenderClear(renderer);
        if (art) {
            const float wanted = float(width) / float(height), have = float(art->w) / float(art->h);
            SDL_FRect source{0, 0, float(art->w), float(art->h)};
            if (wanted > have) { source.h = float(art->w) / wanted; source.y = (float(art->h) - source.h) / 2; }
            else { source.w = float(art->h) * wanted; source.x = (float(art->w) - source.w) / 2; }
            SDL_RenderTexture(renderer, art, &source, nullptr);
            SDL_DestroyTexture(art);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        const SDL_FColor top{0.04f, 0.05f, 0.05f, 0.62f}, bottom{0.04f, 0.05f, 0.05f, 0.92f};
        const SDL_Vertex veil[4] = {{{0, 0}, top, {0, 0}}, {{float(width), 0}, top, {0, 0}}, {{float(width), float(height)}, bottom, {0, 0}}, {{0, float(height)}, bottom, {0, 0}}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(renderer, nullptr, veil, 4, order, 6);
        SDL_SetRenderTarget(renderer, nullptr);
    }
    void init_ui() {
        const auto previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();   // its own atlas: the game's menu context is not touched
        ImGui::GetIO().IniFilename = nullptr;
        try { fonts = ui::load_fonts(*ImGui::GetIO().Fonts); serif = fonts.serif; sans = fonts.sans; }
        catch (const std::exception& e) { std::fprintf(stderr, "[compile] fonts: %s\n", e.what()); serif = sans = ImGui::GetIO().Fonts->AddFontDefault(); }
        backend = ImGui_ImplSDLRenderer3_Init(renderer);
        ImGui::SetCurrentContext(previous);
    }
    void shutdown_ui() {
        if (!context) return;
        const auto previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        if (backend) ImGui_ImplSDLRenderer3_Shutdown();
        ImGui::DestroyContext(context);   // the context that was current before ours is restored below (DestroyContext may have cleared it)
        ImGui::SetCurrentContext(previous == context ? nullptr : previous);
        context = nullptr; backend = false;
    }
    void draw() {
        if (!renderer || !context || !backend) return;
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        const auto previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.DisplaySize = {float(width), float(height)};
        io.DeltaTime = 1.f / 20.f;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        auto& draw = *ImGui::GetBackgroundDrawList();
        // 1 at 1280x720 on a full screen; the small window (960x540) draws its text 1.6 times as large, so it reads at the size the monitor shows it
        const bool full_screen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        const float s = std::max(.5f, float(height) / (full_screen ? 720.f : 450.f)), cx = float(width) / 2;
        // the game's name
        centered(draw, serif, 58 * s, cx, height * .13f, kIvory, "Resident Evil");
        centered(draw, sans, 26 * s, cx, height * .13f + 74 * s, kMuted, "The Umbrella Chronicles");
        // what is going on
        const bool counting = total > 0;
        const char* headline = !message.empty() ? message.c_str() : counting ? title.c_str() : starting.c_str();
        const float line_y = height * .56f;
        centered(draw, serif, 38 * s, cx, line_y, kIvory, headline);
        if (message.empty()) centered(draw, sans, 22 * s, cx, line_y + 56 * s, kMuted, counting ? detail.c_str() : starting_detail.c_str());   // the start has its line too (the renderer and DLSS load)
        if (counting) {
            const float bar_width = std::min(float(width) - 80 * s, 880 * s), bar_height = std::max(6.f, 9 * s);
            const ImVec2 left{cx - bar_width / 2, height * .78f};
            draw.AddRectFilled(left, {left.x + bar_width, left.y + bar_height}, IM_COL32(255, 255, 255, 36), bar_height / 2);
            const float fraction = std::clamp(float(done) / float(total), 0.f, 1.f);
            // rounded like the track (a multi-colour rectangle cannot be rounded): the colour runs from red to the hot red with the progress
            if (fraction > 0) draw.AddRectFilled(left, {left.x + std::max(bar_height, bar_width * fraction), left.y + bar_height}, IM_COL32(157 + int(43 * fraction), 21 + int(12 * fraction), 34 + int(13 * fraction), 255), bar_height / 2);
            const std::string count = std::to_string(done) + " / " + std::to_string(total);
            const ImVec2 size = text_size(sans, 22 * s, count.c_str());
            draw.AddText(sans, 22 * s, {std::round(left.x + bar_width - size.x), std::round(left.y + bar_height + 14 * s)}, kMuted, count.c_str());
        }
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0x14, 0x18, 0x19, 255);
        SDL_RenderClear(renderer);
        if (backdrop) SDL_RenderTexture(renderer, backdrop, nullptr, nullptr);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        ImGui::SetCurrentContext(previous);
        if (const char* png = std::getenv("UC_COMPILE_SCREEN_PNG")) {   // diagnostics: keep the last frame as a BMP picture (what the player sees; the name says PNG for history, the file is a BMP)
            if (SDL_Surface* frame_pixels = SDL_RenderReadPixels(renderer, nullptr)) { SDL_SaveBMP(frame_pixels, png); SDL_DestroySurface(frame_pixels); }
        }
        SDL_RenderPresent(renderer);
        last_draw = std::chrono::steady_clock::now();
    }
};

CompileScreen::CompileScreen(std::string_view language) : state_(new State) {
    state_->language = std::string(language);
    const auto text = [&](const char* key) { return std::string(launcher::text(state_->language, key)); };
    state_->starting = text("compile_starting"); state_->starting_detail = text("compile_starting_detail"); state_->title = text("compile_title"); state_->detail = text("compile_detail");
}
CompileScreen::~CompileScreen() {
    state_->shutdown_ui();
    if (state_->backdrop) SDL_DestroyTexture(state_->backdrop);
    if (state_->renderer) SDL_DestroyRenderer(state_->renderer);
    if (state_->window) SDL_DestroyWindow(state_->window);
    delete state_;
}
void CompileScreen::show() {
    if (state_->window || state_->attempted) return;
    state_->attempted = true;   // a window that cannot be made is not retried at every update
    std::fprintf(stderr, "[compile] starting\n");
#ifdef _WIN32
    const bool from_steam = false;
    // A window that is always on top, out of the task bar and never takes the focus from what the player is doing (the game window opens behind it).
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Resident Evil: The Umbrella Chronicles");
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 540);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
    state_->window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
#else
    const bool from_steam = std::getenv("SteamGameId") || std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    const SDL_WindowFlags flags = from_steam ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_BORDERLESS;
    state_->window = SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", from_steam ? 1280 : 960, from_steam ? 800 : 540, flags);
#endif
    if (!state_->window) { std::fprintf(stderr, "[compile] no progress window: %s\n", SDL_GetError()); return; }
    set_window_icon(state_->window);
    state_->renderer = SDL_CreateRenderer(state_->window, "software");
    if (!state_->renderer) { std::fprintf(stderr, "[compile] no progress renderer: %s\n", SDL_GetError()); return; }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(state_->window, &width, &height);
    state_->build_backdrop(width, height);
    state_->init_ui();
    state_->draw();
}
void CompileScreen::wait_for_peer(std::string_view message) {
    std::fprintf(stderr, "[compile] waiting: %s\n", std::string(message).c_str());
    show();   // the wait is its own screen (network play): it opens the window if the compile did not
    state_->message = std::string(message);
    state_->total = 0;
    state_->draw();
    SDL_PumpEvents();
}
void CompileScreen::update(size_t done, size_t total) {
#ifdef _WIN32   // test runs never call show(): a compile that takes longer than 400 ms shows the screen all the same
    if (!state_->attempted && std::chrono::steady_clock::now() - state_->created > std::chrono::milliseconds(400)) show();
#endif
    state_->done = done;
    state_->total = total;
    if (total) {
        state_->message.clear();
        const size_t step = done * 10 / total;
        if (step != state_->last_logged_step || done == total) { state_->last_logged_step = step; std::fprintf(stderr, "[compile] %zu / %zu\n", done, total); }
    }
    if (std::chrono::steady_clock::now() - state_->last_draw > std::chrono::milliseconds(50)) state_->draw();
    SDL_PumpEvents();   // keeps the compositor from calling the window unresponsive
}
}  // namespace uc
