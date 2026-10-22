#include "live_settings.h"
#include <exception>
#include <utility>
namespace uc {
LiveSettings::LiveSettings(std::filesystem::path config,ConfigDocument loaded,SettingsTargets targets)
    : path_(std::move(config)),document_(std::move(loaded)),options_(options_from_config(document_)),targets_(std::move(targets)) {}
void LiveSettings::apply(const Options& next) {
    const auto& was=options_;
    const bool scale=next.render_scale!=was.render_scale;
    const bool upscaling=next.upscaler!=was.upscaler || next.upscale_mode!=was.upscale_mode || next.sharpening!=was.sharpening || next.sharpness!=was.sharpness
        || next.frame_generation!=was.frame_generation || next.frame_generation_multiplier!=was.frame_generation_multiplier;
    const bool limit=next.fps_limit!=was.fps_limit, bloom=next.bloom_settings()!=was.bloom_settings(), style=next.prompt_style!=was.prompt_style;
    const bool effects=next.effects!=was.effects, dlss5=next.dlss5!=was.dlss5, crosshair=next.crosshair_scale!=was.crosshair_scale, hdr=next.hdr!=was.hdr;
    if(!scale && !upscaling && !limit && !bloom && !style && !effects && !dlss5 && !crosshair && !hdr) return;
    options_=next;
    if(effects) targets_.effects(next.effects);
    if(dlss5) targets_.dlss5(next.dlss5);
    if(scale) targets_.render_scale(next.render_scale);
    if(upscaling) targets_.upscaling(next);
    if(limit) targets_.fps_limit(next.fps_limit);
    if(bloom) targets_.bloom(next.bloom_settings());
    if(crosshair) targets_.crosshair_scale(next.crosshair_scale);
    if(style) targets_.prompt_style(next.prompt_style);
    if(hdr) targets_.hdr(next.hdr);
    save();
}
void LiveSettings::apply(const Bindings& next) {
    if(next==document_.input) return;
    // The target validates (NativeInput::bindings throws on an invalid set) before anything is stored.
    try { targets_.bindings(next); } catch(const std::exception& error) { error_=error.what(); return; }
    document_.input=next;
    save();
}
const nlohmann::ordered_json& LiveSettings::mods_section() const {
    static const nlohmann::ordered_json none;
    const auto at=document_.properties.find("mods");
    return at==document_.properties.end() ? none : *at;
}
void LiveSettings::apply_mods(const nlohmann::ordered_json& section) {
    if(mods_section()==section) return;
    document_.properties["mods"]=section;
    save();
}
void LiveSettings::save() {
    try {
        auto next=document_;store_options(next,options_);save_config(path_,next);
        document_=std::move(next);error_.reset();
    } catch(const std::exception& error) { error_=error.what(); }
}
FrameGenChoice frame_generation_choice(const Options& options) {
    if(!options.frame_generation || options.upscaler==UpscalerKind::off) return FrameGenChoice::off;
    return options.upscaler==UpscalerKind::fsr ? FrameGenChoice::fsr : FrameGenChoice::dlss;
}
Options with_frame_generation(Options options,FrameGenChoice choice) {
    if(choice==FrameGenChoice::off) { options.frame_generation=false; return options; }
    const auto kind=choice==FrameGenChoice::dlss ? UpscalerKind::dlss : UpscalerKind::fsr;
    if(options.upscaler!=kind || kind==UpscalerKind::fsr) options.frame_generation_multiplier=2;   // a DLSS multiplier from the launcher stays with DLSS
    options.upscaler=kind; options.frame_generation=true;
    return options;
}
Options with_upscaler(Options options,UpscalerKind kind) {
    if(kind==UpscalerKind::off || kind==UpscalerKind::smaa) options.frame_generation=false;   // SMAA has no generator
    else if(options.frame_generation && kind!=options.upscaler) options.frame_generation_multiplier=2;   // the generator follows its upscaler
    options.upscaler=kind;
    return options;
}
}
