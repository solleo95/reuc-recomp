#include "crash_report.h"
#include "dispatch.h"
#include "trace.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#endif

namespace uc {
namespace {
std::filesystem::path g_directory;
std::function<std::string()> g_stats;
std::function<void(const std::filesystem::path&)> g_bundle;
std::atomic<bool> g_written{false};
std::atomic<bool> g_done{false};

long host_thread_id() {   // the id the system's tools show for this thread (macOS's SYS_gettid is a different call: the thread's uid and gid)
#if defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return long(id);
#else
    return long(syscall(SYS_gettid));
#endif
}

void write_frame(std::FILE* out, void* pc) {
    std::fprintf(out, "  %p", pc);
    Dl_info where;
    if (dladdr(pc, &where) && where.dli_fname) {
        const char* name = std::strrchr(where.dli_fname, '/'); name = name ? name + 1 : where.dli_fname;
        std::fprintf(out, " %s+0x%llX", name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(where.dli_fbase)));
        if (where.dli_sname) std::fprintf(out, " (%s)", where.dli_sname);
    }
    if (const uint32_t guest = guest_function_for_host(pc)) {
        std::fprintf(out, "  guest %08X", guest);
        const auto symbol = symbol_name(guest);
        if (!symbol.empty()) std::fprintf(out, " [%s]", symbol.c_str());
    }
    std::fprintf(out, "\n");
}
}

void crash_report_init(std::filesystem::path directory, std::function<std::string()> stats) {
    g_directory = std::move(directory);
    g_stats = std::move(stats);
    g_bundle = nullptr;
    g_written = false;
    g_done = false;
}

void crash_report_set_bundle(std::function<void(const std::filesystem::path&)> bundle) { g_bundle = std::move(bundle); }

// A fault on a thread whose stack overflowed needs its own stack to run the handler on (sigaltstack is per thread).
void crash_guard_thread() {
    static thread_local bool installed = false;
    if (installed) return;
    installed = true;
    constexpr size_t kSize = 128 * 1024;
    stack_t stack{};
    stack.ss_sp = std::malloc(kSize);
    stack.ss_size = kSize;
    if (stack.ss_sp) sigaltstack(&stack, nullptr);   // lives as long as the thread: never freed
}

bool crash_report_write(std::string_view reason, const _CONTEXT*) noexcept {
    if (g_directory.empty()) return false;
    if (g_written.exchange(true)) {
        // Another thread is writing: let it finish before this thread exits the process.
        for (int i = 0; i < 500 && !g_done; ++i) usleep(10000);
        return false;
    }
    struct Done { ~Done() { g_done = true; } } done;
    try {
    std::error_code error;
    std::filesystem::create_directories(g_directory, error);
    const auto now = std::time(nullptr);
    std::tm local{}; localtime_r(&now, &local);
    char name[96];
    std::snprintf(name, sizeof name, "crash-%04d%02d%02d-%02d%02d%02d-%d.txt", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, int(getpid()));
    std::FILE* out = std::fopen((g_directory / name).c_str(), "w");
    if (!out) return false;
    std::fprintf(out, "Umbrella Chronicles native port crash report\nreason: %.*s\nthread: %ld\n", int(reason.size()), reason.data(), host_thread_id());
    std::fprintf(out, "\nhost stack (guest = translated function containing the frame; the first frames are the handler's own):\n");
    std::array<void*, 64> frames{};
    const int count = backtrace(frames.data(), int(frames.size()));
    for (int i = 0; i < count; ++i) write_frame(out, frames[i]);
    std::fprintf(out, "\nlast guest functions on this thread (UC_TRACE builds only):\n");
    trace_dump(out, 32);
    if (g_stats) {
        try { std::fprintf(out, "\nruntime state:\n%s\n", g_stats().c_str()); }
        catch (...) { std::fprintf(out, "\nruntime state unavailable\n"); }
    }
    auto bundle = g_directory / name; bundle.replace_extension(".dump");
    if (g_bundle) std::fprintf(out, "\nbundle: %s\n", bundle.c_str());
    std::fclose(out);
    std::fprintf(stderr, "crash report: %s\n", (g_directory / name).c_str());
    if (g_bundle) {
        try { g_bundle(bundle); std::fprintf(stderr, "crash bundle: %s\n", bundle.c_str()); }
        catch (...) { std::fprintf(stderr, "crash bundle failed\n"); }   // the report is already on disk
    }
    return true;
    } catch (...) { return false; }
}
}
