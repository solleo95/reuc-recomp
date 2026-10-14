#include "options.h"
#include <charconv>
#include <cmath>
namespace uc {
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments,Options options) {
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto arg = arguments[i];
        if (arg == "--headless") options.headless = true;
        else if (arg == "--validation") options.validation = true;
        else if (arg == "--log-dvd") options.log_dvd = true;
        else if (arg == "--log-sleep") options.log_sleep = true;
        else if (arg == "--gx-audit") options.gx_audit = true;
        else if (arg == "--skip-intro") options.skip_intro = true;
        else if (arg == "--profile") options.profile = true;
        else if (arg == "--capture-scene-passes") options.capture_scene_passes = true;
        else if (arg == "--unlock-60fps") options.unlock_60fps = true;
        else if (arg == "--input-probe") options.input_probe = true;
        else if (arg == "--game" || arg == "--width" || arg == "--height" || arg == "--seconds" || arg=="--test-control" || arg=="--save-root" || arg=="--config" || arg=="--render-scale" || arg=="--window-mode") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for " + std::string(arg);
            auto value = arguments[i];
            if(arg=="--window-mode") {
                if(value=="windowed") options.window_mode=WindowMode::windowed;
                else if(value=="borderless") options.window_mode=WindowMode::borderless;
                else if(value=="exclusive") options.window_mode=WindowMode::exclusive;
                else return "invalid value for " + std::string(arg);
                continue;
            }
            if(arg=="--game" || arg=="--test-control" || arg=="--save-root" || arg=="--config") {
                if(value.find('\0')!=std::string_view::npos)return "invalid path for "+std::string(arg);
                auto path=std::filesystem::path(std::u8string(value.begin(),value.end()));
                if(arg=="--game")options.game=std::move(path);
                else if(arg=="--test-control")options.test_control=std::move(path);
                else if(arg=="--save-root")options.save_root=std::move(path);
                else options.config=std::move(path);
                continue;
            }
            if(arg=="--render-scale") {
                double scale=0;
                auto parsed=std::from_chars(value.data(),value.data()+value.size(),scale);
                if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !std::isfinite(scale) || scale<0.5 || scale>6) return "invalid value for " + std::string(arg);
                options.render_scale=scale; continue;
            }
            uint32_t number = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (arg != "--seconds" && number == 0)) return "invalid value for " + std::string(arg);
            if (arg == "--width") options.width = number;
            else if (arg == "--height") options.height = number;
            else options.seconds = number;
        } else return "unknown option " + std::string(arg);
    }
    return options;
}
}
