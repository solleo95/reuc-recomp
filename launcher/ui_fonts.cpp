#include "ui_fonts.h"
#include <imgui.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#ifndef _WIN32
namespace uc::ui {
// Linux (SteamOS): the fonts the system ships. DejaVu Sans for Latin and Cyrillic, Noto Sans CJK merged in for Japanese, Korean and Chinese, DejaVu Serif for headings.
// macOS: Arial, Arial Unicode merged in for Japanese, Korean and Chinese, Georgia for headings (all in /System/Library/Fonts/Supplemental).
// Android: the same roles from /system/fonts.
#if defined(__APPLE__)
constexpr const char* kSansFonts[]={"/System/Library/Fonts/Supplemental/Arial.ttf","/System/Library/Fonts/Supplemental/Arial Unicode.ttf"};
constexpr const char* kSerifFonts[]={"/System/Library/Fonts/Supplemental/Georgia.ttf"};
#elif defined(__ANDROID__)   // Roboto (the static copy first, the variable one otherwise), Noto Sans CJK merged in, Noto Serif for headings
constexpr const char* kSansFonts[]={"/system/fonts/RobotoStatic-Regular.ttf","/system/fonts/NotoSansCJK-Regular.ttc"};
constexpr const char* kSerifFonts[]={"/system/fonts/NotoSerif-Regular.ttf"};
#else
constexpr const char* kSansFonts[]={"/usr/share/fonts/TTF/DejaVuSans.ttf","/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
constexpr const char* kSerifFonts[]={"/usr/share/fonts/TTF/DejaVuSerif.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"};
#endif
Fonts load_fonts(ImFontAtlas& atlas) {
    Fonts fonts;
    const auto read=[](const char* path) {
        std::ifstream source(path,std::ios::binary);
        std::vector<char> data(std::istreambuf_iterator<char>(source),{});
        std::fprintf(stderr,"[ui-font] %s bytes=%zu%s\n",path,data.size(),data.empty() ? " (missing)" : "");
        return data;
    };
    fonts.bytes.reserve(5);   // the atlas keeps pointers into these buffers
    for(const char* path:kSansFonts) {
        auto data=read(path);
        if(data.empty())continue;
        ImFontConfig config;config.FontDataOwnedByAtlas=false;config.MergeMode=!fonts.bytes.empty();
        fonts.bytes.push_back(std::move(data));auto& bytes=fonts.bytes.back();
        if(!atlas.AddFontFromMemoryTTF(bytes.data(),int(bytes.size()),16,&config))throw std::runtime_error("cannot load UI font");
    }
    if(fonts.bytes.empty())throw std::runtime_error("no UI font found (install DejaVu Sans)");
    fonts.sans=atlas.Fonts[0];
    std::vector<char> serif_data;
    for(const char* path:kSerifFonts) if(serif_data.empty())serif_data=read(path);
    if(serif_data.empty()) {fonts.serif=fonts.sans;return fonts;}   // headings in the sans font
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
#else
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
        std::fprintf(stderr,"[ui-font] %ls bytes=%zu%s\n",filename,data.size(),data.empty() ? " (missing)" : "");
        if(data.empty()) {if(fonts.bytes.empty())throw std::runtime_error("Segoe UI font unavailable");continue;}
        if(data.size()>64*1024*1024)throw std::runtime_error("UI font too large");
        ImFontConfig config;config.FontDataOwnedByAtlas=false;config.MergeMode=!fonts.bytes.empty();
        fonts.bytes.push_back(std::move(data));auto& bytes=fonts.bytes.back();
        if(!atlas.AddFontFromMemoryTTF(bytes.data(),int(bytes.size()),16,&config))throw std::runtime_error("cannot load UI font");
    }
    fonts.sans=atlas.Fonts[0];
    std::ifstream serif_source(folder/L"georgia.ttf",std::ios::binary);
    std::vector<char> serif_data(std::istreambuf_iterator<char>(serif_source),{});
    std::fprintf(stderr,"[ui-font] georgia.ttf bytes=%zu%s\n",serif_data.size(),serif_data.empty() ? " (missing)" : "");
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
#endif
