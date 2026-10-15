#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
struct _CONTEXT;
namespace uc {
// Writes <directory>/crash-<time>-<pid>.txt for the first failure of the process (later calls return false).
void crash_report_init(std::filesystem::path directory, std::function<std::string()> stats);
bool crash_report_write(std::string_view reason, const _CONTEXT* context) noexcept;
// Reserves stack for the crash handler on the calling host thread, so a stack overflow can still be reported.
void crash_guard_thread();
}
