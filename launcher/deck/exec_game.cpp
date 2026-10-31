#include "exec_game.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace uc::deck {
GameCommand build_game_command(const std::filesystem::path& install, const std::vector<std::string>& extra) {
    GameCommand command;
    command.path = install / "uc-game.sh";
    command.argv = {"uc-game.sh"};
    command.argv.insert(command.argv.end(), extra.begin(), extra.end());
    return command;   // the script sets the environment of the game (the Deck's own controller through HIDAPI, borderless at the display's size) and runs `uc`
}
#ifndef _WIN32
unsigned ensure_executables(const std::filesystem::path& install) {
    unsigned changed = 0;
    for (const char* name : {"uc", "uc_launcher", "uc-deck.sh", "uc-game.sh", "tools/wit/wit"}) {
        const auto path = install / name;
        struct stat info{};
        if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & S_IXUSR)) continue;
        if (::chmod(path.c_str(), (info.st_mode & 07777) | S_IRUSR | S_IXUSR) == 0) ++changed;
    }
    return changed;
}
std::string exec_game(const std::filesystem::path& install, const std::vector<std::string>& extra) {
    auto command = build_game_command(install, extra);
    if (::chdir(install.c_str()) != 0) return std::string("cannot enter ") + install.string() + ": " + std::strerror(errno);
    for (const auto& [name, value] : command.env) {
        if (value) ::setenv(name.c_str(), value->c_str(), 1); else ::unsetenv(name.c_str());
    }
    ensure_executables(install);   // the script may have lost its mode in a zip extracted by a tool that drops them
    std::vector<char*> argv;
    for (auto& item : command.argv) argv.push_back(item.data());
    argv.push_back(nullptr);
    ::execv(command.path.c_str(), argv.data());
    return std::string("cannot start ") + command.path.string() + ": " + std::strerror(errno);
}
#endif
}
