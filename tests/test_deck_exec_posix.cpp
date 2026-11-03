// Linux/Deck only (tests/linux/CMakeLists.txt): the launcher makes its own programs executable at start (a zip extracted by a tool that drops Unix modes), and
// the exec of a restarted launcher goes to the path it is given.
#include "check.h"
#include "deck/exec_game.h"
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace uc::deck;
namespace {
void write(const fs::path& path, std::string_view text, mode_t mode) {
    fs::create_directories(path.parent_path()); { std::ofstream(path, std::ios::binary) << text; } ::chmod(path.c_str(), mode);
}
mode_t mode_of(const fs::path& path) { struct stat info{}; CHECK(::stat(path.c_str(), &info) == 0); return info.st_mode & 0777; }
}
int main() {
    const auto root = fs::temp_directory_path() / ("uc-exec-posix-" + std::to_string(::getpid()));
    fs::remove_all(root);
    write(root / "uc", "x", 0644); write(root / "uc_launcher", "x", 0600); write(root / "tools/wit/wit", "x", 0644); write(root / "data/other.bin", "x", 0644);
    write(root / "uc-launcher.sh", "x", 0644); write(root / "uc.sh", "x", 0644);
    CHECK(ensure_executables(root) == 5);
    for (const char* name : {"uc", "uc_launcher", "tools/wit/wit", "uc-launcher.sh", "uc.sh"}) CHECK((mode_of(root / name) & 0100) && (mode_of(root / name) & 0400));   // the owner can run it
    CHECK(mode_of(root / "data/other.bin") == 0644);                                  // nothing else is touched
    CHECK(ensure_executables(root) == 0);                                             // already executable: nothing to do
    fs::remove(root / "tools/wit/wit");
    CHECK(ensure_executables(root) == 0);                                             // a missing file is not an error here (Play and the unpack report it)
    fs::remove_all(root);
    std::puts("deck exec posix: passed");
}
