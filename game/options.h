#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
namespace uc {
struct Options {
    std::filesystem::path game = "S:\\Games\\Umbrella Chronicles\\game";
    uint32_t width = 1280, height = 720, seconds = 0;
    bool headless = false, validation = false, log_dvd = false, log_sleep = false, gx_audit = false;
};
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments);
}
