#include "graphite.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace uc::launcher {
namespace {
using Microsoft::WRL::ComPtr;

void require(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + ": HRESULT " +
                                 std::to_string(static_cast<uint32_t>(result)));
}

struct ComSession {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
    ComSession() = default;
    ComSession(const ComSession&) = delete;
    ComSession& operator=(const ComSession&) = delete;
};

struct TaskMemoryDeleter {
    void operator()(wchar_t* value) const { CoTaskMemFree(value); }
};
}

SDL_Texture* load_banner(SDL_Renderer* renderer, const std::filesystem::path& path) {
    if (!renderer) throw std::runtime_error("Banner requires an SDL renderer");
    ComSession com;
    if (com.result != RPC_E_CHANGED_MODE) require(com.result, "Initialize banner COM");
    ComPtr<IWICImagingFactory> factory;
    require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())), "Create banner WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()), "Open banner PNG");
    GUID container{};
    require(decoder->GetContainerFormat(&container), "Read banner format");
    if (container != GUID_ContainerFormatPng) throw std::runtime_error("Banner must be PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    require(decoder->GetFrame(0, frame.GetAddressOf()), "Decode banner frame");
    UINT width = 0, height = 0;
    require(frame->GetSize(&width, &height), "Read banner dimensions");
    if (!width || !height || width > 4096 || height > 4096)
        throw std::runtime_error("Banner dimensions must be 1..4096 pixels");
    const size_t stride = size_t(width) * 4;
    if (height > std::numeric_limits<size_t>::max() / stride ||
        stride * height > std::numeric_limits<UINT>::max() ||
        stride * height > size_t(4096) * 4096 * 4)
        throw std::runtime_error("Banner RGBA buffer exceeds size limit");
    ComPtr<IWICFormatConverter> converter;
    require(factory->CreateFormatConverter(converter.GetAddressOf()), "Create banner RGBA converter");
    require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Convert banner to RGBA");
    std::vector<BYTE> pixels(stride * height);
    require(converter->CopyPixels(nullptr, UINT(stride), UINT(pixels.size()), pixels.data()),
        "Read banner RGBA pixels");
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_RGBA32,
                              pixels.data(), int(stride)), SDL_DestroySurface);
    if (!surface) throw std::runtime_error(std::string("Create banner surface: ") + SDL_GetError());
    auto* texture = SDL_CreateTextureFromSurface(renderer, surface.get());
    if (!texture) throw std::runtime_error(std::string("Create banner texture: ") + SDL_GetError());
    return texture;
}

namespace {
// A folder picker, or a file picker for the disc images that Wit reads.
bool choose_path(SDL_Window* window, std::filesystem::path& path, bool folder) {
    if (!window) throw std::runtime_error("Folder picker requires an SDL window");
    const auto properties = SDL_GetWindowProperties(window);
    if (!properties) throw std::runtime_error(std::string("Read folder picker window: ") + SDL_GetError());
    const auto owner = static_cast<HWND>(SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!owner) throw std::runtime_error("Folder picker requires a Windows HWND");
    ComSession com;
    require(com.result, "Initialize folder picker STA COM");
    ComPtr<IFileOpenDialog> dialog;
    require(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf())), "Create folder picker");
    FILEOPENDIALOGOPTIONS options{};
    require(dialog->GetOptions(&options), "Read folder picker options");
    require(dialog->SetOptions(options | (folder ? FOS_PICKFOLDERS : 0) | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR),
        "Set folder picker options");
    if (!folder) {
        const COMDLG_FILTERSPEC types[] = {{L"*.iso, *.wbfs, *.ciso, *.wdf, *.wia", L"*.iso;*.wbfs;*.ciso;*.wdf;*.wia"}, {L"*.*", L"*.*"}};
        require(dialog->SetFileTypes(UINT(std::size(types)), types), "Set disc image types");
    }
    const HRESULT result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    require(result, "Show folder picker");
    ComPtr<IShellItem> item;
    require(dialog->GetResult(item.GetAddressOf()), "Read selected folder");
    PWSTR raw = nullptr;
    const HRESULT name_result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    std::unique_ptr<wchar_t, TaskMemoryDeleter> name(raw);
    require(name_result, "Read selected folder path");
    if (!name || !*name) throw std::runtime_error("Folder picker returned an empty path");
    std::filesystem::path selected(name.get());
    path.swap(selected);
    return true;
}
}

bool choose_game_folder(SDL_Window* window, std::filesystem::path& path) { return choose_path(window, path, true); }
bool choose_image_file(SDL_Window* window, std::filesystem::path& path) { return choose_path(window, path, false); }
}
