#include "options.h"
#include <charconv>
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
namespace {
constexpr std::array<std::string_view,5> kUpscalerNames{"off","dlss","fsr","blit","smaa"};
constexpr std::array<std::string_view,5> kUpscaleModeNames{"native","quality","balanced","performance","ultra_performance"};
}
std::filesystem::path default_game_folder() {
    std::wstring value(32768,L'\0');
    value.resize(GetModuleFileNameW(nullptr,value.data(),DWORD(value.size())));
    return std::filesystem::path(value).parent_path()/"game";
}
std::optional<UpscalerKind> upscaler_from_name(std::string_view name, bool allow_test) {
    for (size_t i = 0; i < kUpscalerNames.size(); ++i)
        if (name == kUpscalerNames[i] && (allow_test || UpscalerKind(i) != UpscalerKind::blit)) return UpscalerKind(i);
    return std::nullopt;
}
std::optional<UpscaleMode> upscale_mode_from_name(std::string_view name) {
    for (size_t i = 0; i < kUpscaleModeNames.size(); ++i) if (name == kUpscaleModeNames[i]) return UpscaleMode(i);
    return std::nullopt;
}
std::string_view upscaler_name(UpscalerKind kind) { return size_t(kind) < kUpscalerNames.size() ? kUpscalerNames[size_t(kind)] : std::string_view{}; }
std::string_view upscale_mode_name(UpscaleMode mode) { return size_t(mode) < kUpscaleModeNames.size() ? kUpscaleModeNames[size_t(mode)] : std::string_view{}; }
bool valid_sharpness(double sharpness) { return std::isfinite(sharpness) && sharpness >= 0.01 && sharpness <= 1.0; }
bool valid_frame_generation_multiplier(uint64_t multiplier) { return multiplier >= 2 && multiplier <= 6; }
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments,Options options) {
    uint16_t net_port=net::default_port;
    bool port_set=false,net_loopback=false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto arg = arguments[i];
        if(arg=="--net-host" || arg=="--net-join") {
            if(options.netplay)return "choose only one of --net-host and --net-join";
            net::Endpoint endpoint; endpoint.host=arg=="--net-host";
            if(!endpoint.host) {
                if(++i>=arguments.size())return "missing value for --net-join";
                endpoint.address=std::string(arguments[i]);
                if(!net::valid_address(endpoint.address))return "--net-join requires a numeric IPv4 address";
            }
            options.netplay=endpoint;
        }
        else if(arg=="--net-loopback")net_loopback=true;
        else if(arg=="--net-port") {
            if(++i>=arguments.size())return "missing value for --net-port";
            unsigned port=0;const auto value=arguments[i];const auto parsed=std::from_chars(value.data(),value.data()+value.size(),port);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !port || port>65535)return "invalid value for --net-port";
            net_port=uint16_t(port);port_set=true;
        }
        else if(arg=="--net-tick") {
            if(++i>=arguments.size())return "missing value for --net-tick";
            const auto value=arguments[i];unsigned tick=0;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),tick);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || (tick!=30 && tick!=60 && tick!=120 && tick!=144 && tick!=240))return "invalid value for --net-tick (30, 60, 120, 144 or 240)";
            options.net_tick=tick;
        }
        else if (arg == "--renderer") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --renderer";
            const auto backend = renderer_backend_from_name(arguments[i]);
            if (!backend) return "invalid value for --renderer (vulkan, dx11 or dx12)";
            options.renderer_backend = *backend;
        }
        else if (arg == "--headless") options.headless = true;
        else if (arg == "--validation") options.validation = true;
        else if (arg == "--log-dvd") options.log_dvd = true;
        else if (arg == "--log-sleep") options.log_sleep = true;
        else if (arg == "--gx-audit") options.gx_audit = true;
        else if (arg == "--skip-intro") options.skip_intro = true;
        else if (arg == "--guest-cpu") {
            if (++i >= arguments.size()) return "missing value for --guest-cpu";
            int cpu = -1; auto parsed = std::from_chars(arguments[i].data(), arguments[i].data() + arguments[i].size(), cpu);
            if (parsed.ec != std::errc{} || parsed.ptr != arguments[i].data() + arguments[i].size() || cpu < 0 || cpu > 62) return "invalid value for --guest-cpu";
            options.guest_cpu = cpu;
        }
        else if (arg == "--mute") options.mute = true;
        else if (arg == "--no-mods") options.no_mods = true;
        else if (arg == "--deterministic") options.deterministic = true;
        else if (arg == "--no-vulkan-layers") options.no_vulkan_layers = true;
        else if (arg == "--profile") options.profile = true;
        else if (arg == "--capture-scene-passes") options.capture_scene_passes = true;
        else if (arg == "--unlock-60fps") options.unlock_60fps = true;
        else if (arg == "--no-ultrawide") options.ultrawide = false;
        else if (arg == "--no-bloom") options.bloom = false;
        else if (arg == "--bloom-strength") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --bloom-strength";
            uint32_t strength = 0; const auto value = arguments[i];
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), strength);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !valid_bloom_settings({true, strength})) return "invalid value for --bloom-strength";
            options.bloom_strength = strength;
        }
        else if (arg == "--input-probe") options.input_probe = true;
        else if (arg == "--gx-gaps") options.gx_gaps = true;
        else if (arg == "--audio-audit") options.audio_audit = true;
        else if (arg == "--god") options.god = true;
        else if (arg == "--skip-tutorials") options.skip_tutorials = true;
        else if (arg == "--no-sharpening") options.sharpening = false;
        else if (arg == "--features") options.features = true;
        else if (arg == "--per-pixel-lighting") options.effects.per_pixel_lighting = true;
        else if (arg == "--contact-shadows") options.effects.contact_shadows = true;
        else if (arg == "--ambient-occlusion") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --ambient-occlusion";
            const auto level = effect_level_from_name(arguments[i]);
            if (!level) return "invalid value for --ambient-occlusion";
            options.effects.ambient_occlusion = *level;
        }
        else if (arg == "--motion-blur") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --motion-blur";
            const auto level = effect_level_from_name(arguments[i]);
            if (!level) return "invalid value for --motion-blur";
            options.effects.motion_blur = *level;
        }
        else if (arg == "--ray-tracing") options.effects.path_tracing = true;   // the player's ray tracing is path tracing (graphics_effects.h)
        else if (arg == "--rt-quality") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --rt-quality";
            const auto level = effect_level_from_name(arguments[i]);
            if (!level || *level == EffectLevel::off) return "invalid value for --rt-quality";   // the switch is the off state
            options.effects.rt_quality = *level;
        }
        else if (arg == "--dlss5") { options.dlss5_enabled = true; if (!options.dlss5.enabled()) options.dlss5.hook = Dlss5Hook::upscaled; }   // after the upscaler, before the HUD; a hook chosen by --dlss5-option stays
        else if (arg == "--dlss5-option") {   // key=value, the config's strict parser
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --dlss5-option";
            options.dlss5_enabled = true;
            const auto separator = arguments[i].find('=');
            std::string error;
            if (separator == std::string_view::npos || !set_dlss5_field(options.dlss5, arguments[i].substr(0, separator), arguments[i].substr(separator + 1), error)) return error.empty() ? "invalid value for --dlss5-option" : error;
        }
        else if (arg == "--anisotropy") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for --anisotropy";
            uint32_t level = 0; const auto value = arguments[i];
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), level);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !valid_anisotropy(level)) return "invalid value for --anisotropy";
            options.effects.anisotropy = level;
        }
        else if (arg == "--upscaler" || arg == "--upscale-mode" || arg == "--sharpness" || arg == "--frame-generation") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for " + std::string(arg);
            const auto value = arguments[i];
            if (arg == "--upscaler") {
                if (auto kind = upscaler_from_name(value, true)) options.upscaler = *kind;
                else return "invalid value for --upscaler";
            } else if (arg == "--upscale-mode") {
                if (auto mode = upscale_mode_from_name(value)) options.upscale_mode = *mode;
                else return "invalid value for --upscale-mode";
            } else if (arg == "--sharpness") {
                double sharpness = 0;
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), sharpness);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !valid_sharpness(sharpness)) return "invalid value for --sharpness";
                options.sharpness = sharpness; options.sharpening = true;
            } else {
                uint32_t multiplier = 0;
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), multiplier);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (multiplier != 0 && !valid_frame_generation_multiplier(multiplier))) return "invalid value for --frame-generation";
                options.frame_generation = multiplier != 0;
                if (multiplier) options.frame_generation_multiplier = multiplier;
            }
        }
        else if (arg == "--game" || arg == "--width" || arg == "--height" || arg == "--seconds" || arg=="--test-control" || arg=="--test-server" || arg=="--speed" || arg=="--capture-dir" || arg=="--save-root" || arg=="--config" || arg=="--render-scale" || arg=="--window-mode" || arg=="--hud-position" || arg=="--pointer" || arg=="--game-hz" || arg=="--fps-limit" || arg=="--det-out" || arg=="--det-frames" || arg=="--demo-record" || arg=="--demo-play" || arg=="--stop-retrace") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for " + std::string(arg);
            auto value = arguments[i];
            if(arg=="--window-mode") {
                if(value=="windowed") options.window_mode=WindowMode::windowed;
                else if(value=="borderless") options.window_mode=WindowMode::borderless;
                else if(value=="exclusive") options.window_mode=WindowMode::exclusive;
                else return "invalid value for " + std::string(arg);
                continue;
            }
            if(arg=="--pointer") {
                if(value=="relative") options.pointer=PointerMode::relative;
                else if(value=="absolute") options.pointer=PointerMode::absolute;
                else return "invalid value for --pointer (relative or absolute)";
                continue;
            }
            if(arg=="--hud-position") {
                if(value=="auto") options.hud_position=HudPosition::automatic;
                else if(value=="center") options.hud_position=HudPosition::center;
                else if(value=="edges") options.hud_position=HudPosition::edges;
                else return "invalid value for --hud-position (auto, center or edges)";
                continue;
            }
            if(arg=="--test-server") {
                if(value.size()>32 || !std::all_of(value.begin(),value.end(),[](char c){return std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='-';}))return "invalid value for --test-server";
                options.test_server=std::string(value); continue;
            }
            if(arg=="--game" || arg=="--det-out" || arg=="--demo-record" || arg=="--demo-play" || arg=="--test-control" || arg=="--save-root" || arg=="--config" || arg=="--capture-dir") {
                if(value.find('\0')!=std::string_view::npos)return "invalid path for "+std::string(arg);
                auto path=std::filesystem::path(std::u8string(value.begin(),value.end()));
                if(arg=="--game")options.game=std::move(path);
                else if(arg=="--det-out")options.det_out=std::move(path);
                else if(arg=="--demo-record")options.demo_record=std::move(path);
                else if(arg=="--demo-play")options.demo_play=std::move(path);
                else if(arg=="--test-control")options.test_control=std::move(path);
                else if(arg=="--save-root")options.save_root=std::move(path);
                else if(arg=="--capture-dir")options.capture_dir=std::move(path);
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
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (arg != "--seconds" && arg != "--fps-limit" && number == 0)) return "invalid value for " + std::string(arg);
            if (arg == "--speed") { if (number > 32) return "invalid value for --speed"; options.speed = number; }
            else if (arg == "--width") options.width = number;
            else if (arg == "--height") options.height = number;
            else if (arg == "--fps-limit") { if (number != 0 && (number < 20 || number > 1000)) return "invalid value for --fps-limit"; options.fps_limit = number; }
            else if (arg == "--det-frames") options.det_frames = number;
            else if (arg == "--stop-retrace") options.stop_retrace = number;
            else if (arg == "--game-hz") { if (number < 61 || number > 480) return "invalid value for --game-hz"; options.game_hz = number; }
            else options.seconds = number;
        } else return "unknown option " + std::string(arg);
    }
    if(port_set && !options.netplay)return "--net-port requires --net-host or --net-join";
    if(net_loopback && !options.netplay)return "--net-loopback requires --net-host or --net-join";
    if(options.netplay) {
        options.netplay->port=net_port;
        options.netplay->loopback=net_loopback;
        if(net_loopback && options.netplay->address!="127.0.0.1")return "--net-loopback requires 127.0.0.1";
        if(!options.demo_play.empty() || !options.demo_record.empty())return "network play cannot record or replay demos";
        // Both players run identical fixed steps on the virtual clock: 30 Hz is the original, 60 Hz its doubling, above that timed steps.
        options.deterministic=true;options.no_mods=true;options.game_hz=options.net_tick>60 ? options.net_tick : 0;options.unlock_60fps=options.net_tick==60;
        options.skip_intro=true;options.skip_tutorials=true;
    }
    if (!options.demo_play.empty() && !options.demo_record.empty()) return "--demo-play and --demo-record exclude each other";
    if (!options.demo_play.empty() && !options.deterministic && options.det_out.empty()) return "--demo-play requires --deterministic";
    if (options.speed != 1 && options.test_server.empty()) return "--speed requires --test-server";
    if (options.god && options.test_server.empty()) return "--god requires --test-server";
    return options;
}
}
