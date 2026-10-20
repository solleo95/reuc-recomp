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

void draw_umbrella(ImDrawList& draw, ImVec2 center, float radius) {
    if (!std::isfinite(radius) || radius <= 0 ||
        !std::isfinite(center.x) || !std::isfinite(center.y)) return;
    constexpr float step = 0.7853981633974483f;
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * step;
        const float b = float(i + 1) * step;
        draw.AddTriangleFilled(center,
            ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius),
            ImVec2(center.x + std::cos(b) * radius, center.y + std::sin(b) * radius),
            i % 2 == 0 ? IM_COL32(238, 233, 220, 255) : IM_COL32(157, 21, 34, 255));
    }
}

void draw_navigation_icon(ImDrawList& draw,int icon,ImVec2 at,float size,unsigned color) {
    const float s=size/30.f,stroke=std::max(1.f,2*s);
    auto p=[&](float x,float y){return ImVec2{at.x+x*s,at.y+y*s};};
    auto line=[&](float x,float y,float a,float b){draw.AddLine(p(x,y),p(a,b),color,stroke);};
    if(icon==0) {
        draw.PathLineTo(p(7,6));draw.PathLineTo(p(23,6));
        draw.PathBezierCubicCurveTo(p(27,6),p(31,24),p(26,25));
        draw.PathBezierCubicCurveTo(p(23,26),p(21,20),p(19,20));draw.PathLineTo(p(11,20));
        draw.PathBezierCubicCurveTo(p(9,20),p(7,26),p(4,25));
        draw.PathBezierCubicCurveTo(p(-1,24),p(3,6),p(7,6));draw.PathStroke(color,ImDrawFlags_Closed,stroke);
        line(6,13,12,13);line(9,10,9,16);
        draw.AddCircleFilled(p(22,11),1.5f*s,color);draw.AddCircleFilled(p(25,15),1.5f*s,color);
    } else if(icon==1) {
        draw.AddRect(p(2,3),p(28,22),color,1*s,0,stroke);line(15,22,15,27);line(8,28,22,28);
    } else if(icon==2) {
        draw.AddCircle(p(15,15),9*s,color,24,stroke);draw.AddCircle(p(15,15),3*s,color,16,stroke);
        for(int i=0;i<8;++i) {const float angle=i*0.7853981634f;line(15+9*std::cos(angle),15+9*std::sin(angle),15+13*std::cos(angle),15+13*std::sin(angle));}
    } else if(icon==4) {   // a chip: DLSS 5
        draw.AddRect(p(8,8),p(22,22),color,1*s,0,stroke);draw.AddRect(p(12,12),p(18,18),color,0,0,stroke);
        for(const float t:{11.f,15.f,19.f}) {line(t,3,t,8);line(t,22,t,27);line(3,t,8,t);line(22,t,27,t);}
    } else {
        ImVec2 speaker[]={p(2,11),p(8,11),p(15,5),p(15,25),p(8,19),p(2,19)};
        draw.AddPolyline(speaker,6,color,ImDrawFlags_Closed,stroke);
        draw.PathArcTo(p(14,15),8*s,-.8f,.8f,12);draw.PathStroke(color,0,stroke);
        draw.PathArcTo(p(14,15),14*s,-.8f,.8f,16);draw.PathStroke(color,0,stroke);
    }
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

bool choose_game_folder(SDL_Window* window, std::filesystem::path& path) {
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
    require(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR),
        "Set folder picker options");
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
