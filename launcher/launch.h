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
GameProcess launch_game(const std::filesystem::path& executable,const std::filesystem::path& config,const Options* diagnostic=nullptr);
bool game_running(const GameProcess&);
}
