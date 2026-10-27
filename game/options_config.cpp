#include "options_config.h"
#include <array>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
[[noreturn]] void invalid(const std::string& message) {throw std::invalid_argument("config: "+message);}
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
    if(root.contains("logs"))options.logs=boolean(root["logs"]);
    if(root.contains("graphics")) {
        const auto& graphics=root["graphics"];if(!graphics.is_object())invalid("graphics must be an object");
        if(graphics.contains("renderer")) {
            if(!graphics["renderer"].is_string())invalid("renderer must be a string");
            const auto backend=renderer_backend_from_name(graphics["renderer"].get<std::string>());
            if(!backend)invalid("renderer must be vulkan, dx11 or dx12");
            options.renderer_backend=*backend;
        }
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
        if(graphics.contains("bloom_strength")) {
            const auto& value=graphics["bloom_strength"];
            if(!value.is_number_unsigned() || value.get<uint64_t>()>kBloomStrengthMax)invalid("bloom_strength must be 0..200");
            options.bloom_strength=value.get<uint32_t>();
        }
        if(graphics.contains("crosshair_scale")) {
            const auto& value=graphics["crosshair_scale"];
            if(!value.is_number_unsigned() || value.get<uint64_t>()>100)invalid("crosshair_scale must be 0..100");
            options.crosshair_scale=value.get<uint32_t>();
        }
        if(graphics.contains("dlss5"))options.dlss5_enabled=boolean(graphics["dlss5"]);
        if(graphics.contains("hdr"))options.hdr.enabled=boolean(graphics["hdr"]);
        using Nits=uint32_t HdrSettings::*;
        for(auto [key,field]:std::array<std::pair<const char*,Nits>,3>{{{"hdr_peak_nits",&HdrSettings::peak_nits},{"hdr_paper_white_nits",&HdrSettings::paper_white_nits},{"hdr_ui_nits",&HdrSettings::ui_nits}}}) {
            if(!graphics.contains(key))continue;
            const auto& value=graphics[key];
            if(!value.is_number_unsigned() || value.get<uint64_t>()>kHdrPeakMax)invalid(std::string(key)+" out of range");
            options.hdr.*field=value.get<uint32_t>();
        }
        if(!valid_hdr_settings(options.hdr))invalid("hdr_peak_nits must be 1..2000, hdr_paper_white_nits and hdr_ui_nits 80..500");
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
            if(!value.is_number_unsigned() || value.get<uint64_t>()>16 || !valid_anisotropy(value.get<uint32_t>()))invalid("graphics.anisotropy must be 1, 2, 4, 8 or 16");
            options.effects.anisotropy=value.get<uint32_t>();
        }
        if(graphics.contains("per_pixel_lighting"))options.effects.per_pixel_lighting=boolean(graphics["per_pixel_lighting"]);
        if(graphics.contains("contact_shadows"))options.effects.contact_shadows=boolean(graphics["contact_shadows"]);
        if(graphics.contains("ambient_occlusion")) {
            const auto level=graphics["ambient_occlusion"].is_string() ? effect_level_from_name(graphics["ambient_occlusion"].get<std::string>()) : std::nullopt;
            if(!level)invalid("graphics.ambient_occlusion must be off, low, medium, high or ultra");
            options.effects.ambient_occlusion=*level;
        }
        if(graphics.contains("motion_blur")) {
            const auto level=graphics["motion_blur"].is_string() ? effect_level_from_name(graphics["motion_blur"].get<std::string>()) : std::nullopt;
            if(!level)invalid("graphics.motion_blur must be off, low, medium, high or ultra");
            options.effects.motion_blur=*level;
        }
        if(graphics.contains("ray_tracing"))options.effects.path_tracing=boolean(graphics["ray_tracing"]);   // the player's ray tracing is path tracing
        if(graphics.contains("rt_quality")) {
            const auto level=graphics["rt_quality"].is_string() ? effect_level_from_name(graphics["rt_quality"].get<std::string>()) : std::nullopt;
            if(!level || *level==EffectLevel::off)invalid("graphics.rt_quality must be low, medium, high or ultra");
            options.effects.rt_quality=*level;
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
    // A section written by the first DLSS 5 build (an "enabled" switch and a "stage") has other keys and values than the table: it is dropped (defaults) and rewritten on the next save.
    const bool legacy_dlss5=root.contains("dlss5") && root["dlss5"].is_object() && (root["dlss5"].contains("enabled") || root["dlss5"].contains("stage"));
    if(root.contains("dlss5") && !legacy_dlss5) {
        const auto& dlss5=root["dlss5"];if(!dlss5.is_object())invalid("dlss5 must be an object");
        for(const auto& field:dlss5_fields()) {
            const std::string key(field.key);
            if(!dlss5.contains(key))continue;
            const auto& json=dlss5[key];double value=0;
            switch(field.kind) {
            case Dlss5Kind::boolean:
                if(!json.is_boolean())invalid("dlss5."+key+" must be true or false");
                value=json.get<bool>();break;
            case Dlss5Kind::integer:
                if(!json.is_number_unsigned() || json.get<uint64_t>()>INT32_MAX)invalid("dlss5."+key+" must be a whole number");
                value=double(json.get<uint64_t>());break;
            case Dlss5Kind::real:
                if(!json.is_number())invalid("dlss5."+key+" must be a number");
                value=json.get<double>();break;
            case Dlss5Kind::choice: {
                if(!json.is_string())invalid("dlss5."+key+" must be a string");
                const auto name=json.get<std::string>();bool found=false;
                for(size_t i=0;i<field.choices.size() && !found;++i)if(field.choices[i].name==name) {value=double(i);found=true;}
                if(!found)invalid("dlss5."+key+" has an unknown value");
                break;
            }
            }
            if(const auto problem=dlss5_value_error(field,value);!problem.empty())invalid("dlss5."+problem);
            field.set(options.dlss5,value);
        }
    }
    return options;
}
void store_options(ConfigDocument& document,const Options& options) {
    if(options.prompt_style!=PromptStyle::monochrome && options.prompt_style!=PromptStyle::sculpted)invalid("unknown prompt_style");
    if(renderer_backend_name(options.renderer_backend).empty())invalid("unknown renderer");
    if(options.window_mode!=WindowMode::windowed && options.window_mode!=WindowMode::borderless && options.window_mode!=WindowMode::exclusive)invalid("unknown window_mode");
    if(!upscaler_from_name(upscaler_name(options.upscaler),false))invalid("unknown upscaler");
    if(upscale_mode_name(options.upscale_mode).empty())invalid("unknown upscale_mode");
    auto replacement=document;
    replacement.properties["game"]=utf8_path(options.game);
    replacement.properties["skip_intro"]=options.skip_intro;
    replacement.properties["skip_tutorials"]=options.skip_tutorials;
    replacement.properties["logs"]=options.logs;
    auto& graphics=replacement.properties["graphics"];
    if(graphics.is_null())graphics=nlohmann::ordered_json::object();
    if(!graphics.is_object())invalid("graphics must be an object");
    graphics["renderer"]=std::string(renderer_backend_name(options.renderer_backend));
    graphics["render_scale"]=options.render_scale;
    graphics["width"]=options.width;graphics["height"]=options.height;
    graphics["unlock_60fps"]=options.unlock_60fps;
    graphics["bloom"]=options.bloom;
    graphics["bloom_strength"]=options.bloom_strength;
    graphics["crosshair_scale"]=options.crosshair_scale;
    graphics["dlss5"]=options.dlss5_enabled;
    graphics["hdr"]=options.hdr.enabled;
    graphics["hdr_peak_nits"]=options.hdr.peak_nits;
    graphics["hdr_paper_white_nits"]=options.hdr.paper_white_nits;
    graphics["hdr_ui_nits"]=options.hdr.ui_nits;
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
    graphics["contact_shadows"]=options.effects.contact_shadows;
    if(effect_level_name(options.effects.ambient_occlusion).empty())invalid("unknown ambient_occlusion");
    graphics["ambient_occlusion"]=std::string(effect_level_name(options.effects.ambient_occlusion));
    if(effect_level_name(options.effects.motion_blur).empty())invalid("unknown motion_blur");
    graphics["motion_blur"]=std::string(effect_level_name(options.effects.motion_blur));
    graphics["ray_tracing"]=options.effects.path_tracing;
    if(effect_level_name(options.effects.rt_quality).empty() || options.effects.rt_quality==EffectLevel::off)invalid("unknown rt_quality");
    graphics["rt_quality"]=std::string(effect_level_name(options.effects.rt_quality));
    for(const char* old_key:{"rt_reflections","rt_lighting","rt_shadows","rt_local_shadows","rt_ambient_occlusion","ray_reconstruction","path_tracing"})graphics.erase(old_key);   // the keys of the old ray tracing
    auto& dlss5=replacement.properties["dlss5"];   // after the last use of `graphics`: a new top-level key may move the ordered object's entries
    if(dlss5.is_null() || (dlss5.is_object() && (dlss5.contains("enabled") || dlss5.contains("stage"))))dlss5=nlohmann::ordered_json::object();   // also drops the first build's section
    if(!dlss5.is_object())invalid("dlss5 must be an object");
    for(const auto& field:dlss5_fields()) {
        const double value=field.get(options.dlss5);
        if(const auto problem=dlss5_value_error(field,value);!problem.empty())invalid("dlss5."+problem);
        const std::string key(field.key);
        switch(field.kind) {
        case Dlss5Kind::boolean:dlss5[key]=value!=0;break;
        case Dlss5Kind::integer:dlss5[key]=uint32_t(value);break;
        case Dlss5Kind::real:dlss5[key]=value;break;
        case Dlss5Kind::choice:dlss5[key]=std::string(field.choices[size_t(value)].name);break;
        }
    }
    options_from_config(replacement);
    document.properties=std::move(replacement.properties);
}
}
