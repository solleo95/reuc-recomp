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
// After the report is written, `bundle` gets <report stem>.dump (not created) to put the machine state of the crash in; the report names it. An exception from it is dropped.
void crash_report_set_bundle(std::function<void(const std::filesystem::path&)> bundle);
bool crash_report_write(std::string_view reason, const _CONTEXT* context) noexcept;
// Reserves stack for the crash handler on the calling host thread, so a stack overflow can still be reported.
void crash_guard_thread();
}
