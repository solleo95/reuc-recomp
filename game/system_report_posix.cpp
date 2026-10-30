#include "system_report.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>

// Linux counterpart of system_report_win.cpp: the machine and install facts a support log needs, from /proc, /etc and the file system.
// shortcut: GPUs and drivers are not listed here (the renderer logs the Vulkan device it picks); video memory per GPU is not sampled.
namespace uc {
namespace {
std::string file_value(const char* file, const char* key) {   // "key: value" or KEY="value"
    std::ifstream in(file);
    std::string line;
    const size_t length = std::char_traits<char>::length(key);
    while (std::getline(in, line))
        if (line.compare(0, length, key) == 0 && line.size() > length) {
            auto value = line.substr(length);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == ':' || value.front() == '=' || value.front() == '"')) value.erase(0, 1);
            while (!value.empty() && (value.back() == '"' || value.back() == '\r')) value.pop_back();
            return value;
        }
    return {};
}
long long kilobytes(const char* key) { return std::atoll(file_value("/proc/meminfo", key).c_str()); }   // "<n> kB" -> n
}  // namespace

void log_system_report(const std::filesystem::path& exe_dir) {
    std::string version = "development build";
    std::error_code ignored;
    if (std::filesystem::is_regular_file(exe_dir / "release.json", ignored)) {
        version = "unreadable release.json";
        try {
            std::ifstream in(exe_dir / "release.json", std::ios::binary);
            version = nlohmann::json::parse(in).value("version", std::string("release.json without version"));
        } catch (const std::exception&) {}
    }
    std::fprintf(stderr, "[client] version=%s directory=%s\n", version.c_str(), exe_dir.string().c_str());
    utsname system{};
    uname(&system);
    std::fprintf(stderr, "[system] %s kernel=%s arch=%s session=%s desktop=%s\n", file_value("/etc/os-release", "PRETTY_NAME").c_str(), system.release, system.machine,
                 std::getenv("XDG_SESSION_TYPE") ? std::getenv("XDG_SESSION_TYPE") : "?", std::getenv("XDG_CURRENT_DESKTOP") ? std::getenv("XDG_CURRENT_DESKTOP") : "?");
    const auto space = std::filesystem::space(exe_dir, ignored);
    if (!ignored) std::fprintf(stderr, "[system] install drive free_GB=%llu total_GB=%llu\n", (unsigned long long)(space.available >> 30), (unsigned long long)(space.capacity >> 30));
    for (const char* directory : {"/usr/share/vulkan/implicit_layer.d", "/etc/vulkan/implicit_layer.d"})
        for (const auto& entry : std::filesystem::directory_iterator(directory, ignored))
            std::fprintf(stderr, "[vulkan-layer] implicit machine %s\n", entry.path().filename().string().c_str());
    std::ifstream command("/proc/self/cmdline", std::ios::binary);
    std::string arguments, argument;
    while (std::getline(command, argument, '\0')) arguments += (arguments.empty() ? "" : " ") + argument;
    std::fprintf(stderr, "[command] %s\n", arguments.c_str());
}

void log_runtime_sample() {
    static auto last_wall = std::chrono::steady_clock::now();
    static double last_cpu = 0;
    std::ifstream stat("/proc/self/stat");
    std::string text;
    std::getline(stat, text);
    const auto fields = text.substr(text.rfind(')') + 2);   // after "pid (comm) ": state is field 3
    std::istringstream in(fields);
    std::string skip;
    for (int i = 3; i < 14; ++i) in >> skip;
    double utime = 0, stime = 0;
    in >> utime >> stime;
    const double cpu = (utime + stime) / double(sysconf(_SC_CLK_TCK));
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - last_wall).count();
    const double load = wall > 0 ? (cpu - last_cpu) / wall * 100.0 : 0;
    last_wall = now;
    last_cpu = cpu;
    std::ifstream statm("/proc/self/statm");
    long long pages = 0, resident = 0;
    statm >> pages >> resident;
    std::fprintf(stderr, "[runtime] process_MB=%lld cpu_percent=%.0f RAM_free_MB=%lld\n", resident * sysconf(_SC_PAGESIZE) >> 20, load, kilobytes("MemAvailable") / 1024);
}
}  // namespace uc
