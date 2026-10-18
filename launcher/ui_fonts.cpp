#include "ui_fonts.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc::ui {
Fonts load_fonts(ImFontAtlas& atlas) {
    Fonts fonts;
    wchar_t windows[32768];const auto length=GetWindowsDirectoryW(windows,32768);
    if(!length || length>=32768)throw std::runtime_error("Windows font directory unavailable");
    const auto folder=std::filesystem::path(windows)/L"Fonts";
    // Windows fonts keep the distribution small; ImGui rasterizes glyphs on demand.
    fonts.bytes.reserve(5);   // the atlas keeps pointers into these buffers
    for(const auto* filename:{L"segoeui.ttf",L"msyh.ttc",L"malgun.ttf",L"msgothic.ttc"}) {
        std::ifstream source(folder/filename,std::ios::binary);
        std::vector<char> data(std::istreambuf_iterator<char>(source),{});
        if(data.empty()) {if(fonts.bytes.empty())throw std::runtime_error("Segoe UI font unavailable");continue;}
        if(data.size()>64*1024*1024)throw std::runtime_error("UI font too large");
        ImFontConfig config;config.FontDataOwnedByAtlas=false;config.MergeMode=!fonts.bytes.empty();
        fonts.bytes.push_back(std::move(data));auto& bytes=fonts.bytes.back();
        if(!atlas.AddFontFromMemoryTTF(bytes.data(),int(bytes.size()),16,&config))throw std::runtime_error("cannot load UI font");
    }
    fonts.sans=atlas.Fonts[0];
    std::ifstream serif_source(folder/L"georgia.ttf",std::ios::binary);
    std::vector<char> serif_data(std::istreambuf_iterator<char>(serif_source),{});
    if(serif_data.empty())throw std::runtime_error("Georgia font unavailable");
    const size_t fallback_count=fonts.bytes.size();fonts.bytes.push_back(std::move(serif_data));
    ImFontConfig serif_config;serif_config.FontDataOwnedByAtlas=false;
    fonts.serif=atlas.AddFontFromMemoryTTF(fonts.bytes.back().data(),int(fonts.bytes.back().size()),22,&serif_config);
    if(!fonts.serif)throw std::runtime_error("cannot load UI serif font");
    for(size_t i=1;i<fallback_count;++i) {
        ImFontConfig fallback;fallback.FontDataOwnedByAtlas=false;fallback.MergeMode=true;
        if(!atlas.AddFontFromMemoryTTF(fonts.bytes[i].data(),int(fonts.bytes[i].size()),22,&fallback))throw std::runtime_error("cannot merge serif fallback");
    }
    return fonts;
}
}
