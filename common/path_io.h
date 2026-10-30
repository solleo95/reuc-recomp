#pragma once
#include <cstdio>
#include <filesystem>

namespace uc {
// Opens a file by path on either platform (Windows: the wide-character name, so any Unicode path works). Returns null on failure.
std::FILE* fopen_path(const std::filesystem::path& path, const char* mode);
// The running executable's absolute path.
std::filesystem::path exe_path();
// Where per-user data lives: Linux $XDG_DATA_HOME/umbrella-chronicles (default ~/.local/share/umbrella-chronicles); Windows the executable's folder.
std::filesystem::path user_data_dir();
// Shows a folder in the system's file manager; false when none could be started.
bool open_folder(const std::filesystem::path& folder);
}
