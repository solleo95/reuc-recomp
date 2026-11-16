#pragma once
// What the desktop launcher (launcher/main.cpp) needs from the operating system, in one place: the Windows build and the Linux build of the same launcher differ
// only here. Header-only.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#else
#include "executable_path_posix.h"
#include <unistd.h>
#endif
#include <SDL3/SDL.h>

namespace uc::platform {
// The folder of the running program.
inline std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring value(32768, L'\0'); const auto length = GetModuleFileNameW(nullptr, value.data(), DWORD(value.size()));
    if (!length || length >= value.size()) throw std::runtime_error("cannot locate launcher executable");
    value.resize(length); return std::filesystem::path(value).parent_path();
#else
    const auto self = posix_executable_path();
    if (!self.has_parent_path()) throw std::runtime_error("cannot locate launcher executable");
    return self.parent_path();
#endif
}
// What the Windows launcher offers that the Linux one does not (yet): the Direct3D 11/12 renderers, and the network play button with its lobby.
#ifdef _WIN32
inline constexpr bool kDirectX = true, kNetworkPlay = true;
#else
inline constexpr bool kDirectX = false, kNetworkPlay = false;
#endif
inline unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
// A path from a wide command-line argument. Linux: the launcher parses its arguments as wide characters (as on Windows); the C++ library would convert them back with the
// global locale ("C" at start: fails on anything but ASCII), so they are encoded as UTF-8 here, which is what a Linux path is.
inline std::filesystem::path path_from_wide(const wchar_t* text) {
#ifdef _WIN32
    return std::filesystem::path(text);
#else
    std::string utf8;
    for (const wchar_t* at = text; *at; ++at) {
        const char32_t value = static_cast<char32_t>(*at);
        if (value < 0x80) utf8.push_back(char(value));
        else if (value < 0x800) { utf8.push_back(char(0xC0 | (value >> 6))); utf8.push_back(char(0x80 | (value & 0x3F))); }
        else if (value < 0x10000) { utf8.push_back(char(0xE0 | (value >> 12))); utf8.push_back(char(0x80 | ((value >> 6) & 0x3F))); utf8.push_back(char(0x80 | (value & 0x3F))); }
        else { utf8.push_back(char(0xF0 | (value >> 18))); utf8.push_back(char(0x80 | ((value >> 12) & 0x3F))); utf8.push_back(char(0x80 | ((value >> 6) & 0x3F))); utf8.push_back(char(0x80 | (value & 0x3F))); }
    }
    return std::filesystem::path(utf8);
#endif
}
// The game program next to the launcher.
inline const char* game_program() {
#ifdef _WIN32
    return "uc.exe";
#else
    return "uc";
#endif
}
// UTF-8 text as wide characters (UTF-16 on Windows, UTF-32 on Linux), for the font coverage checks. Strict: nullopt for text that is not valid UTF-8; otherwise
// an invalid byte becomes U+FFFD.
inline std::optional<std::wstring> utf8_to_wide(std::string_view text, bool strict) {
#ifdef _WIN32
    int length = MultiByteToWideChar(CP_UTF8, strict ? MB_ERR_INVALID_CHARS : 0, text.data(), int(text.size()), nullptr, 0);
    if (length < 0 || (length == 0 && !text.empty())) { if (strict) return std::nullopt; length = 0; }
    std::wstring wide(size_t(length), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, strict ? MB_ERR_INVALID_CHARS : 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
#else
    std::wstring wide;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const int extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
        bool valid = extra >= 0 && i + size_t(extra) < text.size();   // every continuation byte is there
        char32_t value = valid ? (extra == 0 ? lead : lead & (0x3Fu >> extra)) : 0;
        for (int k = 1; valid && k <= extra; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + size_t(k)]);
            if ((next & 0xC0) != 0x80) valid = false; else value = (value << 6) | (next & 0x3F);
        }
        if (!valid) { if (strict) return std::nullopt; wide.push_back(L'\xFFFD'); ++i; continue; }
        wide.push_back(wchar_t(value)); i += size_t(extra) + 1;
    }
    return wide;
#endif
}
// Window chrome the platform offers: the dark title bar on Windows 10/11; nothing elsewhere.
inline void style_window(SDL_Window* window) {
#ifdef _WIN32
    auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd) { BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)); }
#else
    (void)window;
#endif
}
// Logs which GPU the launcher's own renderer runs on ("[ui-gpu]" line); the Direct3D 11 renderer of Windows only.
inline void log_renderer_gpu(SDL_Renderer* renderer) {
#ifdef _WIN32
    if (auto* device = static_cast<ID3D11Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr))) {
        IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description))) {
            char name[512]{}; WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
            std::fprintf(stderr, "[ui-gpu] adapter=%s vendor=%04x device=%04x VRAM_MB=%llu feature_level=%x\n", name, description.VendorId, description.DeviceId, static_cast<unsigned long long>(description.DedicatedVideoMemory / (1024 * 1024)), unsigned(device->GetFeatureLevel()));
        } else std::fprintf(stderr, "[ui-gpu] adapter query unavailable\n");
        if (adapter) adapter->Release();
        if (dxgi) dxgi->Release();
    }
#else
    (void)renderer;
#endif
}
}  // namespace uc::platform
