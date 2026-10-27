#pragma once

#include <filesystem>

struct ImDrawList;
struct ImVec2;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace uc::launcher {
void graphite_style();
void draw_navigation_icon(ImDrawList& draw,int icon,ImVec2 at,float size,unsigned color);
void draw_umbrella(ImDrawList& draw, ImVec2 center, float radius);
SDL_Texture* load_banner(SDL_Renderer* renderer, const std::filesystem::path& path);
bool choose_game_folder(SDL_Window* window, std::filesystem::path& path);
bool choose_image_file(SDL_Window* window, std::filesystem::path& path);   // a disc image Wit can unpack
}
