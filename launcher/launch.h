#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <memory>
#include "options.h"
namespace uc {
struct ProcessCloser {void operator()(void*) const;};
using GameProcess=std::unique_ptr<void,ProcessCloser>;
std::wstring quote_windows_arg(std::wstring_view);
GameProcess launch_game(const std::filesystem::path& executable,const std::filesystem::path& config,const Options* diagnostic=nullptr,int guest_cpu=-1);
bool game_running(const GameProcess&);
// The last line of `output` that is a JSON object (Streamline prints its signature checks to stdout before the report).
std::string last_json_line(std::string_view output);
// Runs `uc.exe --features` (what DLSS, FSR and frame generation this PC supports) and returns its JSON line; empty on failure or timeout.
std::string query_features(const std::filesystem::path& executable,const std::filesystem::path& config,unsigned timeout_ms=30000);
}
