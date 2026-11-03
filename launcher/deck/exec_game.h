#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace uc::deck {
struct GameCommand {
    std::filesystem::path path;                                         // <install>/uc
    std::vector<std::string> argv;                                      // argv[0] = "uc", then the arguments
    std::vector<std::pair<std::string, std::optional<std::string>>> env;   // name -> value to set, or nullopt to remove
};
// The command the launcher starts the game with: <install>/uc.sh (scripts/linux/uc.sh), which sets the environment (the Deck's own controller is read
// directly through SDL's HIDAPI driver, which is where the gyro is) and runs `uc --window-mode borderless`. --skip-intro is not forced: it comes from config.json,
// the Game tab's toggle. scripts/linux/uc-launcher.sh, the Steam entry, starts the launcher.
GameCommand build_game_command(const std::filesystem::path& install, const std::vector<std::string>& extra);
#ifndef _WIN32
// A zip extracted by a tool that drops Unix modes leaves `uc`, `uc_launcher` and `tools/wit/wit` unexecutable: gives each of them the owner's execute bit.
// Best effort (a file system that cannot hold modes, a missing file: skipped). Returns how many files it changed.
unsigned ensure_executables(const std::filesystem::path& install);
// Replaces this process with the game (the launcher has done its job; Steam sees one process). Returns only when the exec failed, with the reason.
std::string exec_game(const std::filesystem::path& install, const std::vector<std::string>& extra);
#endif
}
