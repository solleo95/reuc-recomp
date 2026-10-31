#pragma once
#include <filesystem>
namespace uc::deck {
std::filesystem::path executable_dir();   // the folder of this launcher (/proc/self/exe); Steam may start it with any working directory
}
