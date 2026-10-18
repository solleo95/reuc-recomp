#pragma once
#include <vector>
struct ImFont;
struct ImFontAtlas;
namespace uc::ui {
// The launcher's fonts from the Windows font folder: Segoe UI 16 (sans) and Georgia 22 (serif), each with the CJK fallbacks merged in.
// The atlas reads the bytes in place: keep the Fonts alive as long as the atlas.
struct Fonts { std::vector<std::vector<char>> bytes; ImFont* sans=nullptr; ImFont* serif=nullptr; };
Fonts load_fonts(ImFontAtlas& atlas);
}
