#include "path_io.h"
#include <cstdlib>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace uc {
#ifdef _WIN32
std::FILE* fopen_path(const std::filesystem::path& path, const char* mode) {
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));   // modes are ASCII ("rb", "w")
    return _wfopen(path.c_str(), wide_mode.c_str());
}
std::filesystem::path exe_path() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size())));
    return buffer;
}
std::filesystem::path user_data_dir() { return exe_path().parent_path(); }
bool open_folder(const std::filesystem::path& folder) {
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
#else
std::FILE* fopen_path(const std::filesystem::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }
std::filesystem::path exe_path() {
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : path;
}
std::filesystem::path user_data_dir() {
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return std::filesystem::path(data) / "umbrella-chronicles";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/tmp") / ".local/share/umbrella-chronicles";
}
bool open_folder(const std::filesystem::path& folder) {
    const std::string argument = folder.string();
    char* const arguments[] = {const_cast<char*>("xdg-open"), const_cast<char*>(argument.c_str()), nullptr};
    pid_t child = 0;
    return posix_spawnp(&child, "xdg-open", nullptr, nullptr, arguments, environ) == 0;   // not waited for: the file manager outlives the call
}
#endif
}
