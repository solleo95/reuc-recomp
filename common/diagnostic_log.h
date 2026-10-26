#pragma once
#include <filesystem>
#include <memory>
#include <string_view>

namespace uc {
// Construct on the entry thread before starting workers; destroy after they stop.
// Captures CRT and native stdout/stderr while preserving inherited output handles.
class DiagnosticLog {
public:
    DiagnosticLog(std::string_view app,const std::filesystem::path& root) noexcept;
    ~DiagnosticLog();
    bool active() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
const std::filesystem::path& diagnostic_directory() noexcept;
}
