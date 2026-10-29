#pragma once

#include <filesystem>

struct ImDrawList;
struct ImVec2;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace uc::launcher {
void graphite_style();
// The launcher's own widgets: every page and panel builds its selects and integer sliders with these, so they look the same everywhere.
// combo is ImGui::BeginCombo (returns whether the list is open; flags are ImGuiComboFlags) with the text and the chevron drawn by hand; unit is the UI scale.
bool combo(const char* id,const char* preview,float unit,int flags=0);
// ImGui::SetTooltip with inner padding (the style's WindowPadding is 0, so a plain tooltip sticks to its text).
void tooltip(const char* text);
// SliderInt sizes its handle to one step of the range (wide for a short range); a float slider keeps one handle width. `format` uses %d as SliderInt does.
bool slider_int(const char* id,int& value,int low,int high,const char* format="%d");
void draw_navigation_icon(ImDrawList& draw,int icon,ImVec2 at,float size,unsigned color);
void draw_umbrella(ImDrawList& draw, ImVec2 center, float radius);
SDL_Texture* load_banner(SDL_Renderer* renderer, const std::filesystem::path& path);
bool choose_game_folder(SDL_Window* window, std::filesystem::path& path);
bool choose_image_file(SDL_Window* window, std::filesystem::path& path);   // a disc image Wit can unpack
}
