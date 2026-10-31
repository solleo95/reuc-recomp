#include "paths.h"
#include <stdexcept>
#include <unistd.h>
namespace uc::deck {
std::filesystem::path executable_dir() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) throw std::runtime_error("cannot locate the launcher executable");
    return exe.parent_path();
}
}
