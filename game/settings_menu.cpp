#include "settings_menu.h"
#include "graphite.h"
#include "localization.h"
#include "prompt_icons.h"
#include "render_scale.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace uc {
namespace {
constexpr UpscaleMode kModes[]={UpscaleMode::native,UpscaleMode::quality,UpscaleMode::balanced,UpscaleMode::performance,UpscaleMode::ultra_performance};
constexpr uint32_t kLimits[]={30,60,90,120,144,165,240,360,0};   // the launcher's list; 0 = unlimited
bool compact_checkbox(const char* id,const char* label,bool& value,float unit) {
    const auto padding=ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{padding.x,0});
    bool changed=ImGui::Checkbox(id,&value);
    ImGui::PopStyleVar();
    ImGui::SameLine(0,10*unit);
    ImGui::BeginGroup(); ImGui::TextWrapped("%s",label); ImGui::EndGroup();
    if(ImGui::IsItemClicked()) { value=!value; changed=true; }
    return changed;
}
}
SettingsMenu::SettingsMenu(LiveSettings& live,std::string language,std::function<const FeatureReport*()> features,uint32_t monitor_width,uint32_t monitor_height)
    : live_(live),language_(std::move(language)),features_(std::move(features)),monitor_width_(monitor_width),monitor_height_(monitor_height),editor_(live.bindings()) {}
const char* SettingsMenu::tr(std::string_view key) const { return launcher::text(language_,key).data(); }
void SettingsMenu::setup() {
    fonts_=ui::load_fonts(*ImGui::GetIO().Fonts);   // the previous menu context (if any) is gone: its atlas no longer reads the old bytes
}
void SettingsMenu::capture_event(const SDL_Event& event) {
    if((event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) && !event.key.repeat) editor_.event({PhysicalKind::key,uint16_t(event.key.scancode)},event.key.down);
    else if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) editor_.event({PhysicalKind::mouse,event.button.button},event.button.down);
    else if(event.type==SDL_EVENT_MOUSE_WHEEL) editor_.wheel(event.wheel.y==0 ? 0 : ((event.wheel.y>0)!=(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED) ? 1 : -1));
    else if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) editor_.focus(false);
}
void SettingsMenu::commit() {
    if(pending_) { const auto next=*pending_; pending_.reset(); live_.apply(next); }
    if(!editor_.capturing() && editor_.bindings()!=live_.bindings()) live_.apply(editor_.bindings());
}
void SettingsMenu::build() {
    const auto& io=ImGui::GetIO();
    const float font_scale=std::max(.75f,io.DisplaySize.y/1080.f);
    const float unit=font_scale*.75f;
    const float font_size=std::round(22*font_scale);
    ImGui::GetStyle()=ImGuiStyle{};   // ScaleAllSizes must always start from unscaled metrics, including fields the theme leaves at defaults.
    launcher::graphite_style(); ImGui::GetStyle().ScaleAllSizes(unit);
    const float previous_frame_height=font_size+2*std::floor(11*font_scale);
    ImGui::GetStyle().FramePadding.y=(previous_frame_height*.75f-font_size)*.5f;
    ImGui::GetStyle().FrameBorderSize=ImGui::GetStyle().PopupBorderSize=1;   // ScaleAllSizes floors thin borders to zero below 1x
    ImGui::GetStyle().WindowPadding={28*unit,22*unit};
    const ImVec2 size{std::min(io.DisplaySize.x-40*font_scale,560*font_scale),std::min(io.DisplaySize.y-40*font_scale,900*font_scale)};
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Once,{.5f,.5f});
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.94f);
    ImGui::PushFont(fonts_.sans,font_size);
    ImGui::Begin("##settings",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(fonts_.serif,std::round(32*font_scale));ImGui::TextWrapped("%s",tr("graphics_title"));ImGui::PopFont();
    ImGui::Dummy({0,6*unit});
    if(ImGui::BeginTabBar("tabs")) {
        if(ImGui::BeginTabItem(tr("menu_main"))) { main_tab(unit); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem(tr("menu_controls"))) { controls_tab(unit); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem(tr("effects"))) { effects_tab(unit); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if(const auto& error=live_.error()) {
        ImGui::PushStyleColor(ImGuiCol_Text,{1,.52f,.52f,1});
        ImGui::TextWrapped("%s",tr(error->find("changed since loading")!=std::string::npos ? "stale" : "error"));
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",error->c_str());
    }
    ImGui::End();
    ImGui::PopFont();
}
void SettingsMenu::main_tab(float unit) {
    auto options=current();
    const auto* features=features_();
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("graphics",{0,0});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    ImGui::TextUnformatted(tr("scale"));
    const auto scale_label=[&](double scale) {
        const double effective=scale ? scale : monitor_render_scale(monitor_width_,monitor_height_);
        const auto extent=manual_render_extent(effective,double(monitor_width_)/monitor_height_);
        char value[100]; std::snprintf(value,sizeof value,"%.2g× · %u × %u",effective,extent.width,extent.height);
        return scale ? (scale==1 ? "1× (Native) · "+std::to_string(extent.width)+" × "+std::to_string(extent.height) : std::string(value))
                     : std::string(tr("monitor"))+" · "+std::to_string(extent.width)+" × "+std::to_string(extent.height);
    };
    if(ImGui::BeginCombo("##scale",scale_label(options.render_scale).c_str())) {
        for(const double scale:{0.,1.,1.5,2.,3.}) if(ImGui::Selectable(scale_label(scale).c_str(),options.render_scale==scale)) options.render_scale=scale;
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("fps_limit"));
    const uint32_t current_limit=options.fps_limit.value_or(options.unlock_60fps ? 60 : 30);
    const auto limit_label=[&](uint32_t value) { return value ? std::to_string(value)+" FPS" : std::string(tr("unlimited")); };
    if(ImGui::BeginCombo("##fps",limit_label(current_limit).c_str(),ImGuiComboFlags_HeightLarge)) {
        for(const uint32_t value:kLimits) if(ImGui::Selectable(limit_label(value).c_str(),current_limit==value)) options.fps_limit=value;
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    const auto supported=[&](UpscalerKind kind,bool generation,std::string& why) {
        if(kind==UpscalerKind::off) return true;
        if(failed_.contains(kind) || (generation && failed_generation_.contains(kind))) { why=tr("launcher_only"); return false; }
        if(!features) { why=tr("checking"); return false; }
        if(kind==UpscalerKind::smaa) { if(!features->effects) why=tr("effects_unavailable"); return features->effects; }   // SMAA runs on the effects path and has no generator
        const bool ok=kind==UpscalerKind::dlss ? (generation ? features->dlss_g : features->dlss) : (generation ? features->fsr_fg : features->fsr);
        const auto& reason=kind==UpscalerKind::dlss ? (generation ? features->dlss_g_reason : features->dlss_reason) : (generation ? features->fsr_fg_reason : features->fsr_reason);
        if(!ok) why=reason.empty() ? tr("unsupported") : reason;
        return ok;
    };
    const auto kind_label=[&](UpscalerKind kind) { return kind==UpscalerKind::dlss ? "DLSS" : kind==UpscalerKind::fsr ? "FSR" : kind==UpscalerKind::smaa ? "SMAA" : tr("off"); };
    ImGui::TextUnformatted(tr("upscaling"));
    if(ImGui::BeginCombo("##upscaling",kind_label(options.upscaler))) {
        for(const auto kind:{UpscalerKind::off,UpscalerKind::smaa,UpscalerKind::dlss,UpscalerKind::fsr}) {
            std::string why; const bool ok=supported(kind,false,why);
            ImGui::BeginDisabled(!ok && kind!=options.upscaler);   // a saved choice this PC cannot run stays visible and can be left
            if(ImGui::Selectable(kind_label(kind),options.upscaler==kind)) {
                options=with_upscaler(options,kind);
                std::string ignored;   // frame generation moves along only where the new kind has a generator
                if(options.frame_generation && !supported(kind,true,ignored)) options.frame_generation=false;
            }
            ImGui::EndDisabled();
            if(!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",why.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("quality"));
    const bool upscaling=options.upscaler!=UpscalerKind::off;
    const bool scaled=options.upscaler==UpscalerKind::dlss || options.upscaler==UpscalerKind::fsr;   // SMAA renders at the output size: no quality choice
    const auto mode_label=[&](UpscaleMode mode) -> std::string {
        switch(mode) {
        case UpscaleMode::native: return options.upscaler==UpscalerKind::fsr ? "Native AA" : "DLAA";
        case UpscaleMode::quality: return tr("quality");
        case UpscaleMode::balanced: return tr("balanced");
        case UpscaleMode::performance: return tr("performance");
        case UpscaleMode::ultra_performance: return tr("ultra_performance");
        }
        return {};
    };
    ImGui::BeginDisabled(!scaled);
    if(ImGui::BeginCombo("##quality",scaled ? mode_label(options.upscale_mode).c_str() : "—")) {
        for(const auto mode:kModes) if(ImGui::Selectable(mode_label(mode).c_str(),options.upscale_mode==mode)) options.upscale_mode=mode;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Dummy({0,6*unit});
    ImGui::BeginDisabled(!upscaling);
    const auto sharpening_label=std::string(tr("sharpening"))+" (RCAS)";
    compact_checkbox("##sharpening",sharpening_label.c_str(),options.sharpening,unit);
    ImGui::BeginDisabled(!options.sharpening);
    float sharpness=float(options.sharpness);
    if(ImGui::SliderFloat("##sharpness",&sharpness,.01f,1.f,"%.2f")) options.sharpness=std::clamp(std::round(double(sharpness)*100)/100,.01,1.);
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("framegen"));
    const auto choice=frame_generation_choice(options);
    const auto choice_label=[&](FrameGenChoice value) -> std::string {
        if(value==FrameGenChoice::off) return tr("off");
        const uint32_t multiplier=value==frame_generation_choice(options) ? options.frame_generation_multiplier : 2;
        return std::string(value==FrameGenChoice::dlss ? "DLSS" : "FSR")+" X"+std::to_string(multiplier);
    };
    if(ImGui::BeginCombo("##framegen",choice_label(choice).c_str())) {
        for(const auto value:{FrameGenChoice::off,FrameGenChoice::dlss,FrameGenChoice::fsr}) {
            std::string why;
            const bool ok=value==FrameGenChoice::off || (supported(value==FrameGenChoice::dlss ? UpscalerKind::dlss : UpscalerKind::fsr,false,why) &&
                                                         supported(value==FrameGenChoice::dlss ? UpscalerKind::dlss : UpscalerKind::fsr,true,why));
            ImGui::BeginDisabled(!ok && value!=choice);
            if(ImGui::Selectable(choice_label(value).c_str(),choice==value)) options=with_frame_generation(options,value);
            ImGui::EndDisabled();
            if(!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",why.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    ImGui::Dummy({0,14*unit}); ImGui::SeparatorText(tr("other"));
    bool no_bloom=!options.bloom;   // checked = no glow; graphics.bloom keeps true = glow
    if(compact_checkbox("##bloom",tr("bloom"),no_bloom,unit)) options.bloom=!no_bloom;
    ImGui::EndChild();
    pending_=options;
}
void SettingsMenu::effects_tab(float unit) {
    auto options=current();
    const auto* features=features_();
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("effects",{0,0});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    ImGui::TextUnformatted(tr("anisotropy"));
    const uint32_t most=features ? features->anisotropy_max : 16;   // until the report arrives the renderer's own clamp decides
    const auto level_label=[&](uint32_t level) { return level==1 ? std::string(tr("off")) : std::to_string(level)+"×"; };
    if(ImGui::BeginCombo("##anisotropy",level_label(options.effects.anisotropy).c_str())) {
        for(const uint32_t level:{1u,2u,4u,8u,16u}) {
            const bool ok=level<=most;
            ImGui::BeginDisabled(!ok && level!=options.effects.anisotropy);   // a saved level this GPU cannot run stays visible and can be left
            if(ImGui::Selectable(level_label(level).c_str(),options.effects.anisotropy==level)) options.effects.anisotropy=level;
            ImGui::EndDisabled();
            if(!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr("unsupported"));
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    // Effects that need the screen-space path are greyed out on a GPU without it; a saved "on" stays visible and can be switched off.
    const bool path=!features || features->effects;
    ImGui::BeginDisabled(!path && !options.effects.per_pixel_lighting);
    compact_checkbox("##ppl",tr("per_pixel_lighting"),options.effects.per_pixel_lighting,unit);
    ImGui::EndDisabled();
    if(!path && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr("effects_unavailable"));
    const auto preset_label=[&](EffectLevel level) { return level==EffectLevel::off ? std::string(tr("off")) : std::string(tr(level==EffectLevel::low ? "level_low" : level==EffectLevel::medium ? "level_medium" : level==EffectLevel::high ? "level_high" : "level_ultra")); };
    const auto level_combo=[&](const char* id,const char* title,EffectLevel& value) {   // the presets of an effect; greyed out like the other screen-space effects
        ImGui::Dummy({0,6*unit});
        ImGui::TextUnformatted(tr(title));
        ImGui::BeginDisabled(!path && value==EffectLevel::off);
        if(ImGui::BeginCombo(id,preset_label(value).c_str())) {
            for(const auto level:{EffectLevel::off,EffectLevel::low,EffectLevel::medium,EffectLevel::high,EffectLevel::ultra}) if(ImGui::Selectable(preset_label(level).c_str(),value==level)) value=level;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if(!path && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr("effects_unavailable"));
    };
    level_combo("##ao","ambient_occlusion",options.effects.ambient_occlusion);
    ImGui::Dummy({0,6*unit});
    ImGui::BeginDisabled(!path && !options.effects.contact_shadows);
    compact_checkbox("##contact",tr("contact_shadows"),options.effects.contact_shadows,unit);
    ImGui::EndDisabled();
    if(!path && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr("effects_unavailable"));
    ImGui::PopItemWidth();
    ImGui::EndChild();
    pending_=options;
}
void SettingsMenu::controls_tab(float unit) {
    auto options=current();
    ImGui::Dummy({0,8*unit});
    ImGui::TextUnformatted(tr("style"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    const char* styles[]={tr("gray"),tr("cyan")};
    if(ImGui::BeginCombo("##style",styles[size_t(options.prompt_style)])) {
        for(size_t i=0;i<2;++i) if(ImGui::Selectable(styles[i],size_t(options.prompt_style)==i)) options.prompt_style=PromptStyle(i);
        ImGui::EndCombo();
    }
    {   // the three mouse pictures of the chosen style
        const float ph=ImGui::GetFrameHeight(); const auto p=ImGui::GetCursorScreenPos(); auto* draw=ImGui::GetWindowDrawList();
        for(int i=0;i<3;++i) draw_mouse_icon(*draw,{p.x+i*ph,p.y},{ph*.65f,ph},{i==2 ? PhysicalKind::wheel : PhysicalKind::mouse,uint16_t(i==1 ? 3 : 1)},false,options.prompt_style);
        ImGui::Dummy({ph*3,ph});
    }
    pending_=options;
    ImGui::Dummy({0,6*unit});
    const auto physical_name=[&](PhysicalInput input) -> std::string {
        if(input.kind==PhysicalKind::key) {
            const auto* name=SDL_GetScancodeName(SDL_Scancode(input.code));
            return name && *name ? name : std::string(tr("key"))+" "+std::to_string(input.code);
        }
        if(input.kind==PhysicalKind::wheel) return tr(input.code==1 ? "wheel_up" : "wheel_down");
        if(input.code==1) return tr("lmb"); if(input.code==3) return tr("rmb"); if(input.code==2) return tr("mmb");
        return std::string(tr("mouse"))+" "+std::to_string(input.code);
    };
    const auto binding_name=[&](const Binding& binding) -> std::string {
        if(binding.kind==BindingKind::interact_fire) return tr("interact_fire");
        if(binding.kind==BindingKind::interact_motion) return tr("interact_motion");
        if(binding.kind==BindingKind::qte_motion) return tr("rapid");
        std::string text; for(auto modifier:binding.modifiers) text+=physical_name(modifier)+" + ";
        return text+physical_name(binding.input);
    };
    if(ImGui::Button(tr("defaults"))) editor_.restore_defaults();
    if(editor_.capturing()) { ImGui::SameLine(); ImGui::TextWrapped("%s",tr("capture")); }
    if(editor_.error()) ImGui::TextWrapped("%s: %s",tr("rejected"),editor_.error()->c_str());
    ImGui::BeginDisabled(editor_.capturing().has_value());
    if(ImGui::BeginChild("bindings",{0,0})) {
        for(size_t i=0;i<size_t(Action::count);++i) {
            const auto action=Action(i); ImGui::PushID(int(i));
            ImGui::TextWrapped("%s",tr(action_names[i]));
            const auto& entries=editor_.bindings()[i];
            std::string names; for(const auto& entry:entries) { if(!names.empty()) names+='\n'; names+=binding_name(entry); }
            if(names.empty()) names=tr("unbound");
            if(ImGui::Button(names.c_str(),{ImGui::GetContentRegionAvail().x*.75f,0})) editor_.begin_capture(action,true);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",names.c_str());
            if(ImGui::BeginPopupContextItem("binding-menu")) {
                for(size_t j=0;j<entries.size();++j) {
                    ImGui::PushID(int(j)); const auto label=std::string(tr("remove"))+": "+binding_name(entries[j]);
                    const bool remove=ImGui::MenuItem(label.c_str()); ImGui::PopID();
                    if(remove) { editor_.remove(action,j); break; }
                }
                ImGui::EndPopup();
            }
            const bool special=action==Action::grenade || action==Action::knife || action==Action::qte_shake;
            if(ImGui::Button("+",{46*unit,0})) { if(special) ImGui::OpenPopup("add-binding"); else editor_.begin_capture(action); }
            if(ImGui::BeginPopup("add-binding")) {
                if(ImGui::MenuItem(tr("add"))) editor_.begin_capture(action);
                if(action==Action::grenade && ImGui::MenuItem(tr("chord"))) editor_.add(action,{BindingKind::interact_fire,{},{}});
                if(action==Action::knife && ImGui::MenuItem(tr("gesture"))) editor_.add(action,{BindingKind::interact_motion,{},{}});
                if(action==Action::qte_shake && ImGui::MenuItem(tr("motion"))) editor_.add(action,{BindingKind::qte_motion,{},{}});
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if(ImGui::Button(tr("clear"))) editor_.clear(action);
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();
}
}
