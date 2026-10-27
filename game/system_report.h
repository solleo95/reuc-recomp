#pragma once
#include <filesystem>
namespace uc {
// Writes the machine and install facts a support log needs to stderr (the diagnostic log): client version, Windows edition, GPUs with driver and VRAM,
// free disk space, implicit Vulkan layers, the command line. Call once after the DiagnosticLog is active.
void log_system_report(const std::filesystem::path& exe_dir);
// Once a second while the log is written: process memory, CPU load of the process and the machine, free RAM and the video memory held per GPU.
void log_runtime_sample();
}
