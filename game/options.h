#pragma once
#include "input_prompts.h"
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
namespace uc {
enum class WindowMode { windowed,borderless,exclusive };
struct Options {
    std::filesystem::path game = "S:\\Games\\Umbrella Chronicles\\game";
    uint32_t width = 0, height = 0, seconds = 0; //0 resolves to the monitor size at startup.
    double render_scale=0; //0 selects the monitor's720p multiplier once at startup.
    WindowMode window_mode=WindowMode::windowed;
    PromptStyle prompt_style=PromptStyle::monochrome;
    bool headless = false, validation = false, log_dvd = false, log_sleep = false, gx_audit = false;
    bool skip_intro = false;
    bool profile = false;
    bool capture_scene_passes = false;
    bool unlock_60fps = false;
    bool input_probe = false;
    bool gx_gaps = false, audio_audit = false; // diagnostics: reports in <exe>/debug at exit
    std::filesystem::path test_control,save_root,config;
};
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments,Options options={});
}
