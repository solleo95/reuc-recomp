#include "crash_report.h"
#include "dispatch.h"
#include "trace.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace uc {
namespace {
std::filesystem::path g_directory;
std::function<std::string()> g_stats;
std::atomic<bool> g_written{false};
// Stack of the most recent C++ throw on this thread: fatal() reports run after the catch unwound it.
thread_local std::array<void*, 48> t_throw_frames{};
thread_local unsigned short t_throw_count = 0;

LONG CALLBACK record_throw(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) // MSVC C++ exception
        t_throw_count = RtlCaptureStackBackTrace(1, ULONG(t_throw_frames.size()), t_throw_frames.data(), nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void write_frame(std::FILE* out, void* pc) {
    std::fprintf(out, "  %p", pc);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(pc), &module)) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
            std::fprintf(out, " %ls+0x%llX", name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(module)));
        }
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
    g_written = false;
    static bool installed = false;
    if (!installed) { AddVectoredExceptionHandler(1, record_throw); installed = true; }
}

bool crash_report_write(std::string_view reason, const _CONTEXT* context) {
    if (g_directory.empty() || g_written.exchange(true)) return false;
    std::error_code error;
    std::filesystem::create_directories(g_directory, error);
    const auto now = std::time(nullptr);
    std::tm local{}; localtime_s(&local, &now);
    char name[96];
    std::snprintf(name, sizeof name, "crash-%04d%02d%02d-%02d%02d%02d-%lu.txt", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, GetCurrentProcessId());
    std::FILE* out = _wfopen((g_directory / name).c_str(), L"w");
    if (!out) return false;
    std::fprintf(out, "Umbrella Chronicles native port crash report\nreason: %.*s\nthread: %lu\n", int(reason.size()), reason.data(), GetCurrentThreadId());
    if (context) std::fprintf(out, "faulting rip: %p\n", reinterpret_cast<void*>(context->Rip));
    std::fprintf(out, "\nhost stack (guest = translated function containing the frame):\n");
    if (context) write_frame(out, reinterpret_cast<void*>(context->Rip));
    std::array<void*, 64> frames{};
    const auto count = RtlCaptureStackBackTrace(1, ULONG(frames.size()), frames.data(), nullptr);
    for (USHORT i = 0; i < count; ++i) write_frame(out, frames[i]);
    if (t_throw_count) {
        std::fprintf(out, "\nlast C++ throw on this thread:\n");
        for (unsigned i = 0; i < t_throw_count; ++i) write_frame(out, t_throw_frames[i]);
    }
    std::fprintf(out, "\nlast guest functions on this thread (UC_TRACE builds only):\n");
    trace_dump(out, 32);
    if (g_stats) {
        try { std::fprintf(out, "\nruntime state:\n%s\n", g_stats().c_str()); }
        catch (...) { std::fprintf(out, "\nruntime state unavailable\n"); }
    }
    std::fclose(out);
    std::fprintf(stderr, "crash report: %ls\n", (g_directory / name).c_str());
    return true;
}
}
