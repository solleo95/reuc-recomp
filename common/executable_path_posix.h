#pragma once
// The running program's path on POSIX, links resolved: /proc/self/exe on Linux, _NSGetExecutablePath on macOS. Empty when it cannot be found.
// Android: the app's process is app_process64, so the app names the game's folder in UC_INSTALL_DIR and the program is "<that folder>/uc": every path
// the game keeps beside itself (game/, saves/, config.json, data/, logs/) lands there, as in the Windows and macOS installs.
#include <cstdlib>
#include <filesystem>
#include <system_error>
#if defined(__APPLE__)
#include <cstdint>
#include <cstring>
#include <mach-o/dyld.h>
#include <string>
#endif
namespace uc {
inline std::filesystem::path posix_executable_path() {
    std::error_code error;
#if defined(__ANDROID__)
    if (const char* folder = std::getenv("UC_INSTALL_DIR"); folder && *folder) return std::filesystem::path(folder) / "uc";
#endif
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // asks for the size
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    auto resolved = std::filesystem::canonical(path, error);   // the path it was started by may hold links and "..", /proc/self/exe never does
#else
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    return error ? std::filesystem::path() : resolved;
}
}  // namespace uc
