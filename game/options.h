#pragma once
#include "input_prompts.h"
#include <filesystem>
#include <optional>
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
    bool bloom=true;   // the game's glow around lights (graphics.bloom); off skips the bloom composite draw
    bool headless = false, validation = false, log_dvd = false, log_sleep = false, gx_audit = false;
    bool skip_intro = false;
    int guest_cpu = -1;   // benchmarks: pin the guest threads to this logical CPU and keep its SMT sibling (cpu ^ 1) free of every other thread
    std::filesystem::path det_out;   // determinism check: deterministic clock and scheduling, guest memory hashes per retrace into this file
    uint32_t det_frames = 600;
    bool deterministic = false;   // virtual clock and idle-only events without the hash trace (det_out implies it)
    std::filesystem::path demo_record, demo_play;   // every input the guest reads, recorded / played back (needs --deterministic to be repeatable)
    uint32_t stop_retrace = 0;    // deterministic runs: exit when the retrace counter reaches this
    bool mute = false;   // audio runs as usual but the output gain is 0 (test runs)
    bool no_vulkan_layers = false;   // sets VK_LOADER_LAYERS_DISABLE=~implicit~: OBS/RTSS/ReShade/Steam layers stay out of the process (stable timing in tests)
    bool profile = false;
    bool capture_scene_passes = false;
    bool unlock_60fps = false;
    // Frame rate limit (launcher "Frame rate limit", config graphics.fps_limit, --fps-limit): 0 = unlimited, otherwise 20..1000 frames per second.
    // It caps how often finished frames are presented and it chooses the game update rate: above 60 (or unlimited) the timer-driven step of game_hz
    // (at most 480), 31..60 the retrace-locked 60 Hz update, 30 or less the original 30 Hz. --game-hz still wins. Unset keeps the previous behaviour:
    // presentation capped at 240, step from unlock_60fps / game_hz.
    std::optional<uint32_t> fps_limit;
    uint32_t game_hz = 0;    // experimental: game step above 60 Hz from a wall-clock timer (61..480); 0 keeps the retrace-locked step
    bool input_probe = false;
    bool gx_gaps = false, audio_audit = false; // diagnostics: reports in <exe>/debug at exit
    std::filesystem::path test_control,save_root,config,capture_dir;
    std::string test_server; // agent test server pipe name [A-Za-z0-9_-]{1,32}
    bool god = false;        // test-mode cheat: health and ammo stay at their maxima, needs test_server
    bool skip_tutorials = false;   // tutorial pages never open (launcher "Skip tutorials", config skip_tutorials, --skip-tutorials)
    uint32_t speed = 1;     // test-mode virtual clock factor 1..32, needs test_server
};
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments,Options options={});
}
