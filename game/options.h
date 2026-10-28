#pragma once
#include "bloom_math.h"
#include "dlss5_settings.h"
#include "gamepad_config.h"
#include "graphics_effects.h"
#include "hdr_math.h"
#include "input_prompts.h"
#include "upscale_mode.h"
#include "netplay.h"
#include "renderer_backend.h"
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
namespace uc {
enum class WindowMode { windowed,borderless,exclusive };
std::filesystem::path default_game_folder();   // "game" next to the executable
constexpr uint32_t kMouseSensitivityMin=10, kMouseSensitivityMax=300;
struct Options {
    RendererBackend renderer_backend=RendererBackend::vulkan;
    std::optional<net::Endpoint> netplay;
    uint32_t net_tick=60;   // Simulation steps per second both players run in network play: 60 or 30 (the original rate).
    std::filesystem::path game = default_game_folder();
    uint32_t width = 0, height = 0, seconds = 0; //0 resolves to the monitor size at startup.
    double render_scale=0; //0 selects the monitor's720p multiplier once at startup.
    WindowMode window_mode=WindowMode::windowed;
    PromptStyle prompt_style=PromptStyle::sculpted;   // cyan highlight ("Style 1"); monochrome is "Style 2"
    bool bloom=true;   // the glow around lights (graphics.bloom); the renderer's own bloom replaces the game's, off means no bloom at all
    uint32_t bloom_strength=100;   // percent of the base mix, 0..200 (graphics.bloom_strength); 0 equals off
    BloomSettings bloom_settings() const { return {bloom,bloom_strength}; }
    uint32_t mouse_sensitivity=100;   // percent of the raw mouse motion the aim follows, kMouseSensitivityMin..Max (config mouse_sensitivity)
    GamepadSettings gamepad;   // the "gamepad" object of config.json: control type, model, sticks, gyro and the gamepad's bindings
    uint32_t crosshair_scale=100;   // aim reticle size in percent of the game's, 0..100 (graphics.crosshair_scale); 0 hides it
    HdrSettings hdr;   // graphics.hdr / hdr_peak_nits / hdr_paper_white_nits / hdr_ui_nits; SDR while Windows HDR is off
    UpscalerKind upscaler=UpscalerKind::off;
    UpscaleMode upscale_mode=UpscaleMode::quality;
    bool sharpening=false;   // RCAS after the upscale (graphics.sharpening, --sharpness / --no-sharpening)
    double sharpness=0.5;    // 0.01..1
    bool frame_generation=false;   // DLSS-G with DLSS, FSR FG with FSR; forced off in deterministic runs
    uint32_t frame_generation_multiplier=2;   // presented frames per rendered frame, 2..6
    GraphicsEffects effects;   // graphics.anisotropy and the later effect options
    bool dlss5_enabled=false;  // graphics.dlss5 (launcher "DLSS 5", --dlss5): off hides the in-game tab and keeps the DLL unloaded; the game clears it when nvngx_dlssnr.dll is missing
    Dlss5Settings dlss5;       // the "dlss5" object of config.json (DLSS 5 Neural Rendering tab); only used while dlss5_enabled
    bool features=false;   // --features: print what DLSS/FSR/frame generation this machine supports (JSON, for the launcher) and exit
    bool headless = false, validation = false, log_dvd = false, log_sleep = false, gx_audit = false;
    bool skip_intro = false;
    int guest_cpu = -1;   // benchmarks: pin the guest threads to this logical CPU and keep its SMT sibling (cpu ^ 1) free of every other thread
    std::filesystem::path det_out;   // determinism check: deterministic clock and scheduling, guest memory hashes per retrace into this file
    uint32_t det_frames = 600;
    bool deterministic = false;   // virtual clock and idle-only events without the hash trace (det_out implies it)
    std::filesystem::path demo_record, demo_play;   // every input the guest reads, recorded / played back (needs --deterministic to be repeatable)
    uint32_t stop_retrace = 0;    // deterministic runs: exit when the retrace counter reaches this
    bool mute = false;   // audio runs as usual but the output gain is 0 (test runs)
    bool no_mods = false;   // do not look for or load mods (the test harness passes it unless a scenario needs mods)
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
    bool logs = false;   // launcher "Enable logs", config logs: a release client (see public_release.h) writes logs and crash reports again; the launcher passes UC_LOGS=1 to uc.exe
    uint32_t speed = 1;     // test-mode virtual clock factor 1..32, needs test_server
};
// Names used by the CLI and config.json; blit is accepted only with allow_test (CLI).
std::optional<UpscalerKind> upscaler_from_name(std::string_view name, bool allow_test);
std::optional<UpscaleMode> upscale_mode_from_name(std::string_view name);
std::string_view upscaler_name(UpscalerKind);
std::string_view upscale_mode_name(UpscaleMode);
bool valid_sharpness(double);   // 0.01..1
bool valid_frame_generation_multiplier(uint64_t);   // 2..6
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments,Options options={});
}
