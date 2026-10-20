#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace uc {
// DLSS 5 Neural Rendering (NGX feature "dlssnr", docs/superpowers/specs/2026-10-20-dlss5-neural-rendering-design.md). One table of fields drives the config
// ("dlss5" object), --dlss5-option, the launcher tab and the in-game tab, so a setting is added in one place.
enum class Dlss5Preset : uint32_t { p1, p2, p3, h, a, b };
enum class Dlss5Filter : uint32_t { nearest, bilinear, bicubic };
enum class Dlss5Domain : uint32_t { encoded, linear };
enum class Dlss5UiCorrection : uint32_t { off, automatic, on };
enum class Dlss5ExposureMode : uint32_t { automatic, manual };
enum class Dlss5Encoding : uint32_t { automatic, srgb, gamma22, gamma24, linear };   // the SDR-relevant ones: the game's frame is display-referred RGBA8
enum class Dlss5Stage : uint32_t { render, upscaled, present };   // before the upscaler, after it before the HUD, with the HUD just before the swapchain copy
struct Dlss5Settings {
    bool enabled=false;
    Dlss5Preset preset=Dlss5Preset::p1;
    uint32_t style=0;                        // DLSSNR.Style, an index (the runtime names none)
    double intensity=1, tone_global=1, tone_local=1, structure_local=1, structure_skin=1;   // 0..1
    bool auto_mask=true;                     // the network finds the characters itself (DLSSNR.UseAutoMask)
    uint32_t passes=1;                       // evaluations in a row, each on the previous result
    double processing_scale=1;               // the network runs on this fraction of the frame, 0.5..1
    Dlss5Filter downsample_filter=Dlss5Filter::bilinear, upsample_filter=Dlss5Filter::bilinear;
    Dlss5Domain scaling_domain=Dlss5Domain::encoded;
    Dlss5UiCorrection ui_correction=Dlss5UiCorrection::automatic;
    double exposure=1;                       // 0.25..4, used by the manual exposure mode
    Dlss5ExposureMode exposure_mode=Dlss5ExposureMode::automatic;
    double diffuse_white_nits=203;           // 80..400, used when the override is on
    bool diffuse_white_override=false;
    Dlss5Encoding signal_encoding=Dlss5Encoding::automatic;
    Dlss5Stage stage=Dlss5Stage::upscaled;
    bool block_hudless=true;                 // black and fade frames are not processed
    bool debug=false;                        // verbose NGX log and a per-run line in the game log
    bool operator==(const Dlss5Settings&) const=default;
};
enum class Dlss5Kind { boolean, integer, real, choice };
struct Dlss5Choice { std::string_view name; std::string_view label; };   // label: shown as it is; empty: the translation of "dlss5_<name>"
struct Dlss5Field {
    std::string_view key;                    // config key; the translation key is "dlss5_<key>"
    Dlss5Kind kind;
    double min, max, step;                   // choice: 0..count-1
    std::span<const Dlss5Choice> choices;
    double (*get)(const Dlss5Settings&);
    void (*set)(Dlss5Settings&, double);
};
inline std::span<const Dlss5Field> dlss5_fields() {
    static constexpr std::array<Dlss5Choice,6> presets{{{"1","#1"},{"2","#2"},{"3","#3"},{"h","#H"},{"a","A"},{"b","B"}}};
    static constexpr std::array<Dlss5Choice,3> filters{{{"nearest",""},{"bilinear",""},{"bicubic",""}}};
    static constexpr std::array<Dlss5Choice,2> domains{{{"encoded",""},{"linear",""}}};
    static constexpr std::array<Dlss5Choice,3> corrections{{{"off",""},{"auto",""},{"on",""}}};
    static constexpr std::array<Dlss5Choice,2> modes{{{"auto",""},{"manual",""}}};
    static constexpr std::array<Dlss5Choice,5> encodings{{{"auto",""},{"srgb","sRGB"},{"gamma22","Gamma 2.2"},{"gamma24","Gamma 2.4"},{"linear",""}}};
    static constexpr std::array<Dlss5Choice,3> stages{{{"render",""},{"upscaled",""},{"present",""}}};
#define UC_DLSS5_ACCESS(member) [](const Dlss5Settings& s) { return double(s.member); }, [](Dlss5Settings& s, double v) { s.member=static_cast<decltype(s.member)>(v); }
    static const std::array<Dlss5Field,23> fields{{
        {"enabled",Dlss5Kind::boolean,0,1,1,{},UC_DLSS5_ACCESS(enabled)},
        {"preset",Dlss5Kind::choice,0,5,1,presets,UC_DLSS5_ACCESS(preset)},
        {"style",Dlss5Kind::integer,0,2,1,{},UC_DLSS5_ACCESS(style)},
        {"intensity",Dlss5Kind::real,0,1,.05,{},UC_DLSS5_ACCESS(intensity)},
        {"tone_global",Dlss5Kind::real,0,1,.05,{},UC_DLSS5_ACCESS(tone_global)},
        {"tone_local",Dlss5Kind::real,0,1,.05,{},UC_DLSS5_ACCESS(tone_local)},
        {"structure_local",Dlss5Kind::real,0,1,.05,{},UC_DLSS5_ACCESS(structure_local)},
        {"structure_skin",Dlss5Kind::real,0,1,.05,{},UC_DLSS5_ACCESS(structure_skin)},
        {"auto_mask",Dlss5Kind::boolean,0,1,1,{},UC_DLSS5_ACCESS(auto_mask)},
        {"passes",Dlss5Kind::integer,1,4,1,{},UC_DLSS5_ACCESS(passes)},
        {"processing_scale",Dlss5Kind::real,.5,1,.05,{},UC_DLSS5_ACCESS(processing_scale)},
        {"downsample_filter",Dlss5Kind::choice,0,2,1,filters,UC_DLSS5_ACCESS(downsample_filter)},
        {"upsample_filter",Dlss5Kind::choice,0,2,1,filters,UC_DLSS5_ACCESS(upsample_filter)},
        {"scaling_domain",Dlss5Kind::choice,0,1,1,domains,UC_DLSS5_ACCESS(scaling_domain)},
        {"ui_correction",Dlss5Kind::choice,0,2,1,corrections,UC_DLSS5_ACCESS(ui_correction)},
        {"exposure",Dlss5Kind::real,.25,4,.05,{},UC_DLSS5_ACCESS(exposure)},
        {"exposure_mode",Dlss5Kind::choice,0,1,1,modes,UC_DLSS5_ACCESS(exposure_mode)},
        {"diffuse_white_nits",Dlss5Kind::real,80,400,5,{},UC_DLSS5_ACCESS(diffuse_white_nits)},
        {"diffuse_white_override",Dlss5Kind::boolean,0,1,1,{},UC_DLSS5_ACCESS(diffuse_white_override)},
        {"signal_encoding",Dlss5Kind::choice,0,4,1,encodings,UC_DLSS5_ACCESS(signal_encoding)},
        {"stage",Dlss5Kind::choice,0,2,1,stages,UC_DLSS5_ACCESS(stage)},
        {"block_hudless",Dlss5Kind::boolean,0,1,1,{},UC_DLSS5_ACCESS(block_hudless)},
        {"debug",Dlss5Kind::boolean,0,1,1,{},UC_DLSS5_ACCESS(debug)},
    }};
#undef UC_DLSS5_ACCESS
    return fields;
}
inline const Dlss5Field* dlss5_field(std::string_view key) {
    for(const auto& field:dlss5_fields()) if(field.key==key) return &field;
    return nullptr;
}
// Empty when `value` is a legal value of the field (booleans 0/1, integers and choices whole numbers in range, reals finite and in range).
inline std::string dlss5_value_error(const Dlss5Field& field, double value) {
    const std::string name(field.key);
    if(!std::isfinite(value) || value<field.min || value>field.max) return name+" is out of range";
    if(field.kind!=Dlss5Kind::real && value!=std::floor(value)) return name+" must be a whole number";
    return {};
}
// Text form of a value (the CLI and the tests): true/false, a number, or the choice name.
inline std::string dlss5_field_text(const Dlss5Field& field, const Dlss5Settings& settings) {
    const double value=field.get(settings);
    if(field.kind==Dlss5Kind::boolean) return value!=0 ? "true" : "false";
    if(field.kind==Dlss5Kind::choice) return std::string(field.choices[size_t(value)].name);
    char text[32]; const auto end=std::to_chars(text,text+sizeof text,value).ptr;
    return std::string(text,end);
}
// Strict: the whole text must be a value of the field's type and range. `error` names the key on refusal; the settings stay untouched.
inline bool set_dlss5_field(Dlss5Settings& settings, std::string_view key, std::string_view text, std::string& error) {
    const auto* field=dlss5_field(key);
    if(!field) { error="unknown dlss5 option "+std::string(key); return false; }
    double value=0;
    if(field->kind==Dlss5Kind::boolean) {
        if(text!="true" && text!="false") { error="dlss5 "+std::string(key)+" must be true or false"; return false; }
        value=text=="true";
    } else if(field->kind==Dlss5Kind::choice) {
        bool found=false;
        for(size_t i=0;i<field->choices.size() && !found;++i) if(field->choices[i].name==text) { value=double(i); found=true; }
        if(!found) { error="invalid dlss5 "+std::string(key)+" value "+std::string(text); return false; }
    } else {
        const char* end=text.data()+text.size();
        bool ok=!text.empty();
        if(ok && field->kind==Dlss5Kind::integer) { long long whole=0; const auto parsed=std::from_chars(text.data(),end,whole); ok=parsed.ec==std::errc{} && parsed.ptr==end; value=double(whole); }
        else if(ok) { const auto parsed=std::from_chars(text.data(),end,value); ok=parsed.ec==std::errc{} && parsed.ptr==end; }
        if(!ok) { error="invalid dlss5 "+std::string(key)+" value "+std::string(text); return false; }
    }
    if(const auto problem=dlss5_value_error(*field,value); !problem.empty()) { error="dlss5 "+problem; return false; }
    field->set(settings,value);
    return true;
}
// The `Hint.Render.Preset` value NGX gets. A guess: the runtime names no presets, and it falls back to its shipping default for a number it does not know.
inline int dlss5_preset_hint(Dlss5Preset preset) { return int(preset)+1; }
// What the tab lets the player touch. `known`: the feature report has arrived; `supported`: the runtime could create the feature on this GPU.
struct Dlss5Ui {
    bool switch_enabled;     // the switch: usable unless the GPU cannot run it; a saved "on" can always be switched off
    bool settings_enabled;   // the rest: the switch is on and the GPU can run it
};
inline Dlss5Ui dlss5_ui(const Dlss5Settings& settings, bool known, bool supported) {
    const bool blocked=known && !supported;
    return {!blocked || settings.enabled, settings.enabled && !blocked};
}
// The status line of the tab. The runtime's own reason (this run's renderer: a refused feature, a failed run) wins over the report's (what the probe found on the GPU).
enum class Dlss5State { checking, unavailable, available };
struct Dlss5Status { Dlss5State state; std::string reason; };
inline Dlss5Status dlss5_status(bool known, bool supported, const std::string& report_reason, const std::string& runtime_reason) {
    if(!runtime_reason.empty()) return {Dlss5State::unavailable,runtime_reason};
    if(!known) return {Dlss5State::checking,{}};
    if(!supported) return {Dlss5State::unavailable,report_reason};
    return {Dlss5State::available,{}};
}
// The renderer runs the pass when it is switched on and the feature exists on this device.
inline bool dlss5_wanted(const Dlss5Settings& settings, bool available) { return settings.enabled && available; }
}
