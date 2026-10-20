#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace uc {
// DLSS 5 Neural Rendering (NGX feature "dlssnr", docs/superpowers/specs/2026-10-20-dlss5-neural-rendering-design.md). ONE table of fields (dlss5_fields) drives the config
// ("dlss5" object), --dlss5-option, the launcher tab and the in-game tab: a default is the member initializer below, hiding a row is `visible=false` in the table.
enum class Dlss5Preset : uint32_t { p1, p2, p3, h };
enum class Dlss5Hook : uint32_t { off, render, upscaled, present };   // where the pass runs; off = DLSS 5 is not used
enum class Dlss5UiCorrection : uint32_t { automatic, off, on };
enum class Dlss5Model : uint32_t { a, b, c };                       // DLSSNR.Style
enum class Dlss5Domain : uint32_t { scrgb_nl, signed_pq, linear };  // the space the scaling filters average in
enum class Dlss5DownFilter : uint32_t { area, bilinear, nearest };
enum class Dlss5UpFilter : uint32_t { bilinear, bicubic, nearest };
enum class Dlss5Reconstruction : uint32_t { rgb_residual, processed };   // upscaling of the network's output: its change added to the full-size picture, or the output itself
enum class Dlss5RenderPreset : uint32_t { automatic, a, b };
enum class Dlss5Encoding : uint32_t { automatic, linear709, srgb, hdr10, scrgb, scrgb_nl };   // how the game's picture is stored
enum class Dlss5NrEncoding : uint32_t { srgb, pq_sdr, linear_sdr };                           // what the network is given
enum class Dlss5ExposureValue : uint32_t { off, on, inverse };
struct Dlss5Settings {
    Dlss5Preset preset=Dlss5Preset::p1;
    Dlss5Hook hook=Dlss5Hook::off;
    Dlss5UiCorrection ui_correction=Dlss5UiCorrection::automatic;
    double intensity=1;
    Dlss5Model model=Dlss5Model::a;
    double structure=1, tone=1;
    bool character_mask=false;                // DLSSNR.UseAutoMask
    double character_intensity=1;             // DLSSNR.SkinStructureStrength
    // Scaling
    double resolution_scale=1;                // the network runs on this fraction of the frame
    Dlss5Domain scaling_domain=Dlss5Domain::scrgb_nl;
    Dlss5DownFilter downsample_filter=Dlss5DownFilter::area;
    Dlss5UpFilter upsample_filter=Dlss5UpFilter::bilinear;
    Dlss5Reconstruction scale_reconstruction=Dlss5Reconstruction::rgb_residual;
    // Advanced
    Dlss5RenderPreset render_preset=Dlss5RenderPreset::automatic;
    uint32_t passes=1;                        // evaluations in a row, each on the previous result
    Dlss5Encoding encoding=Dlss5Encoding::automatic;
    Dlss5NrEncoding nr_encoding=Dlss5NrEncoding::srgb;
    double diffuse_white_nits=500;            // the nits of signal value 1.0 (PQ conversions)
    bool block_hudless=false;                 // true: black and fade frames are not processed
    double exposure=1;
    Dlss5ExposureValue exposure_value=Dlss5ExposureValue::off;
    bool enabled() const { return hook!=Dlss5Hook::off; }
    bool operator==(const Dlss5Settings&) const=default;
};
enum class Dlss5Kind { boolean, integer, real, choice };
enum class Dlss5Section { main, scaling, advanced };
enum class Dlss5Control { combo, bar, slider };            // how the tabs draw it (booleans are an Off | On bar)
enum class Dlss5Format { plain, percent, multiplier, nits };   // the text of a slider's value
struct Dlss5Choice { std::string_view name; std::string_view label; };   // label: shown as it is; empty: the translation of "dlss5_<name>"
struct Dlss5Field {
    std::string_view key;                    // config key; the translation key of its label is "dlss5_<key>"
    Dlss5Kind kind;
    double min, max, step;                   // choice: 0..count-1
    std::span<const Dlss5Choice> choices;    // booleans carry Off | On
    double (*get)(const Dlss5Settings&);
    void (*set)(Dlss5Settings&, double);
    Dlss5Section section=Dlss5Section::main;
    Dlss5Control control=Dlss5Control::slider;
    Dlss5Format format=Dlss5Format::plain;
    bool visible=true;                       // false hides the row everywhere (it keeps its default and its config key)
    bool resettable=true;                    // a reset-to-default button beside the label
};
inline std::span<const Dlss5Field> dlss5_fields() {
    static constexpr std::array<Dlss5Choice,4> presets{{{"1","#1"},{"2","#2"},{"3","#3"},{"h","#H"}}};
    static constexpr std::array<Dlss5Choice,4> hooks{{{"off",""},{"render","Render"},{"upscaled","Upscaled"},{"present","Present"}}};
    static constexpr std::array<Dlss5Choice,3> corrections{{{"auto",""},{"off",""},{"on",""}}};
    static constexpr std::array<Dlss5Choice,3> models{{{"a","Model A"},{"b","Model B"},{"c","Model C"}}};
    static constexpr std::array<Dlss5Choice,2> off_on{{{"false",""},{"true",""}}};   // names are the text forms of a boolean; the labels are the translations of Off and On
    static constexpr std::array<Dlss5Choice,3> domains{{{"scrgb_nl","scRGB-nl"},{"signed_pq","Signed PQ"},{"linear",""}}};
    static constexpr std::array<Dlss5Choice,3> down_filters{{{"area",""},{"bilinear",""},{"nearest",""}}};
    static constexpr std::array<Dlss5Choice,3> up_filters{{{"bilinear",""},{"bicubic",""},{"nearest",""}}};
    static constexpr std::array<Dlss5Choice,2> reconstructions{{{"rgb_residual",""},{"processed",""}}};
    static constexpr std::array<Dlss5Choice,3> render_presets{{{"default",""},{"a","Preset A"},{"b","Preset B"}}};
    static constexpr std::array<Dlss5Choice,6> encodings{{{"auto",""},{"linear709","Linear BT.709"},{"srgb","sRGB"},{"hdr10","HDR10"},{"scrgb","scRGB"},{"scrgb_nl","scRGB-nl"}}};
    static constexpr std::array<Dlss5Choice,3> nr_encodings{{{"srgb","sRGB"},{"pq_sdr","PQ SDR"},{"linear_sdr","Linear SDR"}}};
    static constexpr std::array<Dlss5Choice,3> exposure_values{{{"off",""},{"on",""},{"inverse",""}}};
#define UC_DLSS5_ACCESS(member) [](const Dlss5Settings& s) { return double(s.member); }, [](Dlss5Settings& s, double v) { s.member=static_cast<decltype(s.member)>(v); }
    using S=Dlss5Section; using C=Dlss5Control; using F=Dlss5Format; using K=Dlss5Kind;
    // The order is the order of the tab, top to bottom. {key, kind, min, max, step, choices, get, set, section, control, format, visible, resettable}
    static const std::array<Dlss5Field,22> fields{{
        {"preset",K::choice,0,3,1,presets,UC_DLSS5_ACCESS(preset),S::main,C::combo,F::plain,true,false},
        {"hook",K::choice,0,3,1,hooks,UC_DLSS5_ACCESS(hook),S::main,C::bar},
        {"ui_correction",K::choice,0,2,1,corrections,UC_DLSS5_ACCESS(ui_correction),S::main,C::bar},
        {"intensity",K::real,0,2,.05,{},UC_DLSS5_ACCESS(intensity),S::main,C::slider},
        {"model",K::choice,0,2,1,models,UC_DLSS5_ACCESS(model),S::main,C::bar},
        {"structure",K::real,0,2,.05,{},UC_DLSS5_ACCESS(structure),S::main,C::slider},
        {"tone",K::real,0,2,.05,{},UC_DLSS5_ACCESS(tone),S::main,C::slider},
        {"character_mask",K::boolean,0,1,1,off_on,UC_DLSS5_ACCESS(character_mask),S::main,C::bar},
        {"character_intensity",K::real,0,2,.05,{},UC_DLSS5_ACCESS(character_intensity),S::main,C::slider},
        {"resolution_scale",K::real,.5,1,.05,{},UC_DLSS5_ACCESS(resolution_scale),S::scaling,C::slider,F::percent},
        {"scaling_domain",K::choice,0,2,1,domains,UC_DLSS5_ACCESS(scaling_domain),S::scaling,C::bar},
        {"downsample_filter",K::choice,0,2,1,down_filters,UC_DLSS5_ACCESS(downsample_filter),S::scaling,C::bar},
        {"upsample_filter",K::choice,0,2,1,up_filters,UC_DLSS5_ACCESS(upsample_filter),S::scaling,C::bar},
        {"scale_reconstruction",K::choice,0,1,1,reconstructions,UC_DLSS5_ACCESS(scale_reconstruction),S::scaling,C::bar},
        {"render_preset",K::choice,0,2,1,render_presets,UC_DLSS5_ACCESS(render_preset),S::advanced,C::bar},
        {"passes",K::integer,1,4,1,{},UC_DLSS5_ACCESS(passes),S::advanced,C::slider},
        {"encoding",K::choice,0,5,1,encodings,UC_DLSS5_ACCESS(encoding),S::advanced,C::bar},
        {"nr_encoding",K::choice,0,2,1,nr_encodings,UC_DLSS5_ACCESS(nr_encoding),S::advanced,C::bar},
        {"diffuse_white_nits",K::real,80,1000,10,{},UC_DLSS5_ACCESS(diffuse_white_nits),S::advanced,C::slider,F::nits},
        {"block_hudless",K::boolean,0,1,1,off_on,UC_DLSS5_ACCESS(block_hudless),S::advanced,C::bar},
        {"exposure",K::real,.25,4,.05,{},UC_DLSS5_ACCESS(exposure),S::advanced,C::slider,F::multiplier},
        {"exposure_value",K::choice,0,2,1,exposure_values,UC_DLSS5_ACCESS(exposure_value),S::advanced,C::bar},
    }};
#undef UC_DLSS5_ACCESS
    return fields;
}
inline const Dlss5Field* dlss5_field(std::string_view key) {
    for(const auto& field:dlss5_fields()) if(field.key==key) return &field;
    return nullptr;
}
inline double dlss5_field_default(const Dlss5Field& field) { return field.get(Dlss5Settings{}); }
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
// The `DLSSNR.Hint.Render.Preset` value NGX gets. A guess: the runtime names no presets, and it falls back to its shipping default for a number it does not know.
// The advanced render preset wins when it is not "default"; else the top preset (#1 #2 #3 #H) is sent.
inline int dlss5_preset_hint(const Dlss5Settings& settings) {
    switch(settings.render_preset) {
    case Dlss5RenderPreset::a: return 5;
    case Dlss5RenderPreset::b: return 6;
    case Dlss5RenderPreset::automatic: break;
    }
    return int(settings.preset)+1;
}
// What the tab lets the player touch. `known`: the feature report has arrived; `supported`: the runtime could create the feature on this GPU.
struct Dlss5Ui {
    bool switch_enabled;     // the Hook Method bar: usable unless the GPU cannot run it; a saved hook can always be set to Off
    bool settings_enabled;   // every other row: the feature can run here (they work while the hook is Off, so the values can be prepared)
};
inline Dlss5Ui dlss5_ui(const Dlss5Settings& settings, bool known, bool supported) {
    const bool blocked=known && !supported;
    return {!blocked || settings.enabled(), !blocked};
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
// The renderer runs the pass when a hook is chosen and the feature exists on this device.
inline bool dlss5_wanted(const Dlss5Settings& settings, bool available) { return settings.enabled() && available; }
}
