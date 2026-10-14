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
        if(graphics.contains("unlock_60fps"))options.unlock_60fps=boolean(graphics["unlock_60fps"]);
        if(graphics.contains("prompt_style")) {
            if(!graphics["prompt_style"].is_string())invalid("prompt_style must be a string");
            const auto style=graphics["prompt_style"].get<std::string>();
            if(style=="monochrome")options.prompt_style=PromptStyle::monochrome;
            else if(style=="sculpted")options.prompt_style=PromptStyle::sculpted;
            else invalid("unknown prompt_style");
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
    auto replacement=document;
    replacement.properties["game"]=utf8_path(options.game);
    replacement.properties["skip_intro"]=options.skip_intro;
    auto& graphics=replacement.properties["graphics"];
    if(graphics.is_null())graphics=nlohmann::ordered_json::object();
    if(!graphics.is_object())invalid("graphics must be an object");
    graphics["render_scale"]=options.render_scale;
    graphics["width"]=options.width;graphics["height"]=options.height;
    graphics["unlock_60fps"]=options.unlock_60fps;
    graphics["prompt_style"]=options.prompt_style==PromptStyle::monochrome ? "monochrome" : "sculpted";
    graphics["window_mode"]=options.window_mode==WindowMode::windowed ? "windowed" : options.window_mode==WindowMode::borderless ? "borderless" : "exclusive";
    options_from_config(replacement);
    document.properties=std::move(replacement.properties);
}
}
