#include "paths.h"
#include "executable_path_posix.h"
#include <stdexcept>
#include <unistd.h>
namespace uc::deck {
std::filesystem::path executable_dir() {
    const auto exe = posix_executable_path();
    if (exe.empty()) throw std::runtime_error("cannot locate the launcher executable");
    return exe.parent_path();
}
}
