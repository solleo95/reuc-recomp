#include "game_path.h"
namespace uc::deck {
namespace fs = std::filesystem;
GameState inspect_game(const fs::path& path) {
    if (path.empty()) return GameState::missing;
    std::error_code ec;
    if (fs::is_regular_file(path / "DATA" / "sys" / "main.dol", ec)) return GameState::found;
    if (fs::is_regular_file(path, ec)) return GameState::image_selected;
    return GameState::missing;
}
}
