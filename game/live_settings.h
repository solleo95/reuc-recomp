#pragma once
#include "options_config.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
namespace uc {
// Where a changed setting goes while the game runs (uc_main wires these to the renderer, frame limiter, game pacing and input).
struct SettingsTargets {
    std::function<void(double)> render_scale;                  // 0 = the monitor's scale
    std::function<void(const Options&)> upscaling;             // upscaler, quality, sharpening, frame generation
    std::function<void(std::optional<uint32_t>)> fps_limit;
    std::function<void(bool)> bloom;
    std::function<void(const GraphicsEffects&)> effects;       // anisotropic filtering and the later effects
    std::function<void(PromptStyle)> prompt_style;
    std::function<void(const Bindings&)> bindings;
};
// The in-game settings: the options saved in config.json (not the command-line overrides of a run). Every change goes to the targets
// whose fields changed and is saved at once; a refused save leaves the change applied and reports why.
class LiveSettings {
public:
    LiveSettings(std::filesystem::path config, ConfigDocument loaded, SettingsTargets targets);
    const Options& options() const { return options_; }
    const Bindings& bindings() const { return document_.input; }
    const std::optional<std::string>& error() const { return error_; }
    void apply(const Options& next);
    void apply(const Bindings& next);
private:
    void save();
    std::filesystem::path path_;
    ConfigDocument document_;
    Options options_;
    SettingsTargets targets_;
    std::optional<std::string> error_;
};
enum class FrameGenChoice { off, dlss, fsr };
FrameGenChoice frame_generation_choice(const Options&);
// Frame generation needs its own upscaler: choosing DLSS or FSR generation switches the upscaler to match (FSR: one generated frame).
Options with_frame_generation(Options, FrameGenChoice);
Options with_upscaler(Options, UpscalerKind);   // Off and SMAA also turn frame generation off
}
