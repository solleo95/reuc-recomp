#include "options_config.h"
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
[[noreturn]] void invalid(const char* message) {throw std::invalid_argument(std::string("config: ")+message);}
bool boolean(const nlohmann::ordered_json& value) {if(!value.is_boolean())invalid("expected boolean setting");return value.get<bool>();}
uint32_t extent(const nlohmann::ordered_json& value) {
    if(!value.is_number_unsigned() || value.get<uint64_t>()>INT32_MAX)invalid("invalid output extent");
    return value.get<uint32_t>();
}
std::string utf8_path(const std::filesystem::path& path) {
    auto value=path.u8string();return std::string(value.begin(),value.end());
}
}
Options options_from_config(const ConfigDocument& document) {
    Options options;const auto& root=document.properties;
    if(root.contains("game")) {
        if(!root["game"].is_string())invalid("game must be a path string");
        const auto path=root["game"].get<std::string>();
        if(path.empty() || path.find('\0')!=std::string::npos)invalid("game path is empty or contains NUL");
        options.game=std::filesystem::path(std::u8string(path.begin(),path.end()));
    }
    if(root.contains("skip_intro"))options.skip_intro=boolean(root["skip_intro"]);
    if(root.contains("skip_tutorials"))options.skip_tutorials=boolean(root["skip_tutorials"]);
    if(root.contains("graphics")) {
        const auto& graphics=root["graphics"];if(!graphics.is_object())invalid("graphics must be an object");
        if(graphics.contains("render_scale")) {
            if(!graphics["render_scale"].is_number())invalid("render_scale must be a number");
            const auto scale=graphics["render_scale"].get<double>();
            if(!std::isfinite(scale) || (scale!=0 && (scale<.5 || scale>6)))invalid("render_scale must be0(monitor) or0.5..6");
            options.render_scale=scale;
        }
        if(graphics.contains("width"))options.width=extent(graphics["width"]);
        if(graphics.contains("height"))options.height=extent(graphics["height"]);
        if(graphics.contains("fps_limit")) {
            if(!graphics["fps_limit"].is_number_unsigned())invalid("fps_limit must be a non-negative integer");
            const auto limit=graphics["fps_limit"].get<uint64_t>();
            if(limit!=0 && (limit<20 || limit>1000))invalid("fps_limit must be 0 (unlimited) or 20..1000");
            options.fps_limit=uint32_t(limit);
        }
        if(graphics.contains("unlock_60fps"))options.unlock_60fps=boolean(graphics["unlock_60fps"]);
        if(graphics.contains("bloom"))options.bloom=boolean(graphics["bloom"]);
        if(graphics.contains("prompt_style")) {
            if(!graphics["prompt_style"].is_string())invalid("prompt_style must be a string");
            const auto style=graphics["prompt_style"].get<std::string>();
            if(style=="monochrome")options.prompt_style=PromptStyle::monochrome;
            else if(style=="sculpted")options.prompt_style=PromptStyle::sculpted;
            else invalid("unknown prompt_style");
        }
        if(graphics.contains("upscaler")) {
            if(!graphics["upscaler"].is_string())invalid("upscaler must be a string");
            const auto kind=upscaler_from_name(graphics["upscaler"].get<std::string>(),false);
            if(!kind)invalid("unknown upscaler");
            options.upscaler=*kind;
        }
        if(graphics.contains("upscale_mode")) {
            if(!graphics["upscale_mode"].is_string())invalid("upscale_mode must be a string");
            const auto mode=upscale_mode_from_name(graphics["upscale_mode"].get<std::string>());
            if(!mode)invalid("unknown upscale_mode");
            options.upscale_mode=*mode;
        }
        if(graphics.contains("sharpening"))options.sharpening=boolean(graphics["sharpening"]);
        if(graphics.contains("sharpness")) {
            if(!graphics["sharpness"].is_number() || !valid_sharpness(graphics["sharpness"].get<double>()))invalid("sharpness must be 0.01..1");
            options.sharpness=graphics["sharpness"].get<double>();
        }
        if(graphics.contains("frame_generation"))options.frame_generation=boolean(graphics["frame_generation"]);
        if(graphics.contains("frame_generation_multiplier")) {
            const auto& value=graphics["frame_generation_multiplier"];
            if(!value.is_number_unsigned() || !valid_frame_generation_multiplier(value.get<uint64_t>()))invalid("frame_generation_multiplier must be 2..6");
            options.frame_generation_multiplier=value.get<uint32_t>();
        }
        if(graphics.contains("anisotropy")) {
            const auto& value=graphics["anisotropy"];
            if(!value.is_number_unsigned() || !valid_anisotropy(value.get<uint32_t>()))invalid("graphics.anisotropy must be 1, 2, 4, 8 or 16");
            options.effects.anisotropy=value.get<uint32_t>();
        }
        if(graphics.contains("per_pixel_lighting"))options.effects.per_pixel_lighting=boolean(graphics["per_pixel_lighting"]);
        if(graphics.contains("ambient_occlusion")) {
            const auto level=graphics["ambient_occlusion"].is_string() ? effect_level_from_name(graphics["ambient_occlusion"].get<std::string>()) : std::nullopt;
            if(!level)invalid("graphics.ambient_occlusion must be off, low, medium, high or ultra");
            options.effects.ambient_occlusion=*level;
        }
        if(graphics.contains("window_mode")) {
            if(!graphics["window_mode"].is_string())invalid("window_mode must be a string");
            const auto mode=graphics["window_mode"].get<std::string>();
            if(mode=="windowed")options.window_mode=WindowMode::windowed;
            else if(mode=="borderless")options.window_mode=WindowMode::borderless;
            else if(mode=="exclusive")options.window_mode=WindowMode::exclusive;
            else invalid("unknown window_mode");
        }
    }
    return options;
}
void store_options(ConfigDocument& document,const Options& options) {
    if(options.prompt_style!=PromptStyle::monochrome && options.prompt_style!=PromptStyle::sculpted)invalid("unknown prompt_style");
    if(options.window_mode!=WindowMode::windowed && options.window_mode!=WindowMode::borderless && options.window_mode!=WindowMode::exclusive)invalid("unknown window_mode");
    if(!upscaler_from_name(upscaler_name(options.upscaler),false))invalid("unknown upscaler");
    if(upscale_mode_name(options.upscale_mode).empty())invalid("unknown upscale_mode");
    auto replacement=document;
    replacement.properties["game"]=utf8_path(options.game);
    replacement.properties["skip_intro"]=options.skip_intro;
    replacement.properties["skip_tutorials"]=options.skip_tutorials;
    auto& graphics=replacement.properties["graphics"];
    if(graphics.is_null())graphics=nlohmann::ordered_json::object();
    if(!graphics.is_object())invalid("graphics must be an object");
    graphics["render_scale"]=options.render_scale;
    graphics["width"]=options.width;graphics["height"]=options.height;
    graphics["unlock_60fps"]=options.unlock_60fps;
    graphics["bloom"]=options.bloom;
    if(options.fps_limit)graphics["fps_limit"]=*options.fps_limit;
    else graphics.erase("fps_limit");
    graphics["prompt_style"]=options.prompt_style==PromptStyle::monochrome ? "monochrome" : "sculpted";
    graphics["window_mode"]=options.window_mode==WindowMode::windowed ? "windowed" : options.window_mode==WindowMode::borderless ? "borderless" : "exclusive";
    graphics["upscaler"]=std::string(upscaler_name(options.upscaler));
    graphics["upscale_mode"]=std::string(upscale_mode_name(options.upscale_mode));
    graphics["sharpening"]=options.sharpening;
    graphics["sharpness"]=options.sharpness;
    graphics["frame_generation"]=options.frame_generation;
    graphics["frame_generation_multiplier"]=options.frame_generation_multiplier;
    graphics["anisotropy"]=options.effects.anisotropy;
    graphics["per_pixel_lighting"]=options.effects.per_pixel_lighting;
    if(effect_level_name(options.effects.ambient_occlusion).empty())invalid("unknown ambient_occlusion");
    graphics["ambient_occlusion"]=std::string(effect_level_name(options.effects.ambient_occlusion));
    options_from_config(replacement);
    document.properties=std::move(replacement.properties);
}
}
