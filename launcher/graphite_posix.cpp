#include "graphite.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace uc::launcher {
// Linux: the PNG is decoded with stb_image (the Windows launcher uses WIC) and uploaded as an RGBA texture.
SDL_Texture* load_banner(SDL_Renderer* renderer, const std::filesystem::path& path) {
    if (!renderer) throw std::runtime_error("Banner requires an SDL renderer");
    int width = 0, height = 0, channels = 0;
    std::vector<unsigned char> file;   // read here, decoded from memory: a path with non-ASCII letters works on Windows as well (stbi_load takes a narrow name)
    { std::ifstream in(path, std::ios::binary); file.assign(std::istreambuf_iterator<char>(in), {}); }
    unsigned char* pixels = file.empty() ? nullptr : stbi_load_from_memory(file.data(), int(file.size()), &width, &height, &channels, 4);
    if (!pixels) throw std::runtime_error("Cannot read " + path.string() + ": " + (file.empty() ? "no such file" : stbi_failure_reason()));
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    const bool uploaded = texture && SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
    stbi_image_free(pixels);
    if (!uploaded) {
        const std::string reason = SDL_GetError();
        SDL_DestroyTexture(texture);
        throw std::runtime_error("Cannot create banner texture: " + reason);
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

namespace {
// What the dialog's callback fills in. It lives on the heap and is shared by the waiter and the callback: whoever comes second deletes it, so a wait that gave up
// (the launcher was closed) leaves nothing for a late callback to write into.
struct DialogResult { std::atomic<int> state{0}; std::string path; };   // 0 waiting, 1 answered, 2 abandoned by the waiter
void SDLCALL on_chosen(void* userdata, const char* const* files, int) {   // files: null on an error, empty when cancelled
    auto* result = static_cast<DialogResult*>(userdata);
    if (files && files[0]) result->path = files[0];
    if (result->state.exchange(1) == 2) delete result;
}
// The system's open dialog (SDL asks the desktop portal or zenity), waited for here the way the Windows dialog is: the launcher's own window keeps being pumped.
bool choose_path(SDL_Window* window, std::filesystem::path& path, bool folder) {
    auto* result = new DialogResult;
    const std::string start = path.empty() ? std::string() : path.string();
    if (folder) SDL_ShowOpenFolderDialog(on_chosen, result, window, start.empty() ? nullptr : start.c_str(), false);
    else {
        static const SDL_DialogFileFilter filters[] = {{"Disc images", "iso;wbfs;ciso;wdf;wia"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(on_chosen, result, window, filters, 2, start.empty() ? nullptr : start.c_str(), false);
    }
    // Closing the launcher (or half an hour) ends the wait, so a desktop portal that never answers cannot hold it for ever.
    for (int waited_ms = 0; result->state == 0 && waited_ms < 30 * 60 * 1000; waited_ms += 10) {
        SDL_PumpEvents();
        if (SDL_HasEvent(SDL_EVENT_QUIT)) break;
        SDL_Delay(10);
    }
    if (result->state.exchange(2) == 0) return false;   // no answer: the callback, if it still comes, deletes `result`
    const std::string chosen = result->path;
    delete result;
    if (chosen.empty()) return false;
    path = chosen;
    return true;
}
}  // namespace
bool choose_game_folder(SDL_Window* window, std::filesystem::path& path) { return choose_path(window, path, true); }
bool choose_image_file(SDL_Window* window, std::filesystem::path& path) { return choose_path(window, path, false); }
}
