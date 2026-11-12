#include "system_report.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <crt_externs.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mach/mach.h>
#include <string>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

// macOS counterpart of system_report_win.cpp and system_report_posix.cpp: the machine and install facts a support log needs, from sysctl and Mach.
// shortcut: GPUs and drivers are not listed here (the renderer logs the Vulkan device it picks); video memory is not sampled (unified memory).
namespace uc {
namespace {
std::string sysctl_text(const char* name) {
    char text[256]{};
    size_t size = sizeof text - 1;
    return sysctlbyname(name, text, &size, nullptr, 0) == 0 ? std::string(text) : std::string();
}
long long free_megabytes() {   // free and reclaimable (inactive, purgeable) pages: what Activity Monitor counts as available
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS) return 0;
    const unsigned long long pages = (unsigned long long)stats.free_count + stats.inactive_count + stats.purgeable_count;
    return (long long)((pages * (unsigned long long)vm_page_size) >> 20);
}
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
    std::fprintf(stderr, "[system] macOS %s (%s) kernel=%s arch=%s model=%s\n", sysctl_text("kern.osproductversion").c_str(), sysctl_text("kern.osversion").c_str(),
                 system.release, system.machine, sysctl_text("hw.model").c_str());
    const auto space = std::filesystem::space(exe_dir, ignored);
    if (!ignored) std::fprintf(stderr, "[system] install drive free_GB=%llu total_GB=%llu\n", (unsigned long long)(space.available >> 30), (unsigned long long)(space.capacity >> 30));
    for (const char* directory : {"/opt/homebrew/etc/vulkan/implicit_layer.d", "/usr/local/etc/vulkan/implicit_layer.d", "/usr/local/share/vulkan/implicit_layer.d"})
        for (const auto& entry : std::filesystem::directory_iterator(directory, ignored))
            std::fprintf(stderr, "[vulkan-layer] implicit machine %s\n", entry.path().filename().string().c_str());
    std::string arguments;
    char** argv = *_NSGetArgv();
    for (int i = 0; i < *_NSGetArgc(); ++i) arguments += (arguments.empty() ? "" : " ") + std::string(argv[i]);
    std::fprintf(stderr, "[command] %s\n", arguments.c_str());
}

void log_runtime_sample() {
    static auto last_wall = std::chrono::steady_clock::now();
    static double last_cpu = 0;
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double cpu = double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - last_wall).count();
    const double load = wall > 0 ? (cpu - last_cpu) / wall * 100.0 : 0;
    last_wall = now;
    last_cpu = cpu;
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const long long footprint = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS ? (long long)(vm.phys_footprint >> 20) : 0;
    std::fprintf(stderr, "[runtime] process_MB=%lld cpu_percent=%.0f RAM_free_MB=%lld\n", footprint, load, free_megabytes());
}
}  // namespace uc
