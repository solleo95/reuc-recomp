#pragma once
#include <filesystem>
namespace uc::deck {
enum class GameState { found, image_selected, missing };
// found: the folder holds the extracted game (DATA/sys/main.dol); image_selected: the path is a disc image (a regular file), which Play unpacks first;
// missing: anything else (including a path that does not exist).
GameState inspect_game(const std::filesystem::path& path);
}
