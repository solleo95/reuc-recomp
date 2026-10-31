// What the Deck launcher knows about the game: whether the chosen path holds it, and the command that starts it (host test).
#include "check.h"
#include "deck/exec_game.h"
#include "deck/game_path.h"
#include <chrono>
#include <fstream>
using namespace uc::deck;
namespace fs = std::filesystem;
int main() {
    const auto root = fs::temp_directory_path() / ("uc-game-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "full" / "DATA" / "sys"); fs::create_directories(root / "empty"); fs::create_directories(root / "data-only" / "DATA");
    std::ofstream(root / "full" / "DATA" / "sys" / "main.dol") << "dol";
    std::ofstream(root / "disc.iso") << "disc";
    CHECK(inspect_game(root / "full") == GameState::found);
    CHECK(inspect_game(root / "disc.iso") == GameState::image_selected);     // a disc image in the path field: Play unpacks it first
    CHECK(inspect_game(root / "empty") == GameState::missing);
    CHECK(inspect_game(root / "data-only") == GameState::missing);           // DATA without sys/main.dol is not the game
    CHECK(inspect_game(root / "nowhere") == GameState::missing);
    CHECK(inspect_game("") == GameState::missing);

    const auto command = build_game_command("/home/deck/UC", {"--fps-limit", "60"});
    CHECK(command.path == fs::path("/home/deck/UC/uc-game.sh"));             // the launcher starts the game through the script, which starts `uc`
    CHECK(command.argv == (std::vector<std::string>{"uc-game.sh", "--fps-limit", "60"}));   // intro skipping comes from config.json (the Game tab's toggle)
    CHECK(command.env.empty());                                              // the script sets the environment of the game itself
    CHECK(build_game_command("/x", {}).argv == std::vector<std::string>{"uc-game.sh"});
    fs::remove_all(root);
    std::puts("deck game: path states and command passed");
}
