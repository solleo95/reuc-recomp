#include "settings_menu.h"
#include "frame_limit_cap.h"
#include "dlss5_panel.h"
#include "graphite.h"
#include "localization.h"
#include "prompt_icons.h"
#include "render_scale.h"
#include "rt_ui.h"
#ifdef _WIN32
#include <intrin.h>
#endif
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
constexpr uint32_t kDefaultReticleColor[2]={0xFF3030,0x3264FF};   // what the game draws the first / second player's reticle with, as the colour pickers show it
ImU32 rgb_u32(uint32_t rgb,float alpha=1) { return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,int(std::clamp(alpha,0.f,1.f)*255+.5f)); }
// One sample reticle for the Reticle tab: the green ammo arc, the circle with its four gaps, the four white marks, the laser dot and the wake, as the options draw them.
void draw_reticle_sample(ImDrawList& draw,ImVec2 centre,float radius,const ReticleOptions& options,uint32_t rgb,float alpha,float scale,bool tail,float unit,ImVec2 box_min,ImVec2 box_max) {
    if(scale<=0 || alpha<=0) return;
    radius*=scale;
    const float thick=std::max(1.5f,2.5f*unit*scale);
    if(tail && !options.hide_tail) {   // the wake: a curve that ends in the centre, as long as the option says (shortened and flattened to stay inside the preview frame)
        const float length=std::min(radius*2.6f*options.tail_length/100.f,(centre.x-box_min.x-4)/.95f); ImVec2 points[24]; float reach=1;
        for(int i=0;i<24;++i) { const float t=float(i)/23, along=length*(1-t); points[i]={centre.x-along*.95f,centre.y+along*.55f*(1-t)*(1-t)+along*.25f*std::sin(t*3.1f)}; }
        for(const auto& p:points) reach=std::max(reach,std::abs(p.y-centre.y)/std::max(1.f,std::min(centre.y-box_min.y,box_max.y-centre.y)-4));
        for(auto& p:points) p.y=centre.y+(p.y-centre.y)/reach;
        for(int i=0;i<23;++i) draw.AddLine(points[i],points[i+1],rgb_u32(rgb,alpha*(.15f+.65f*float(i)/22)),thick*(.5f+float(i)/22));
    }
    if(!options.hide_arc) {   // the green ammo gauge: short ticks around the left side of the circle, from its lower left over the left to the upper right, as in the game
        for(int i=0;i<22;++i) {
            const float a=1.92f+float(i)/21*3.32f, inner=radius*1.14f, outer=radius*1.32f;
            draw.AddLine({centre.x+std::cos(a)*inner,centre.y+std::sin(a)*inner},{centre.x+std::cos(a)*outer,centre.y+std::sin(a)*outer},IM_COL32(40,190,140,int(255*alpha)),thick*.8f);
        }
    }
    for(int quarter=0;quarter<4;++quarter) {   // the circle: four arcs with a gap at every compass point
        const float gap=.10f, start=-3.14159f/2+quarter*3.14159f/2+gap, stop=-3.14159f/2+(quarter+1)*3.14159f/2-gap;
        draw.PathArcTo(centre,radius,start,stop,12); draw.PathStroke(rgb_u32(rgb,alpha),0,thick);
    }
    if(!options.hide_marks) for(int quarter=0;quarter<4;++quarter) {
        const float a=quarter*3.14159f/2, nx=std::cos(a), ny=std::sin(a);
        draw.AddLine({centre.x+nx*radius*.25f,centre.y+ny*radius*.25f},{centre.x+nx*radius*.55f,centre.y+ny*radius*.55f},IM_COL32(245,245,245,int(230*alpha)),thick);
    }
    if(!options.hide_dot) draw.AddCircleFilled(centre,std::max(1.5f,radius*.09f*options.dot_size/100.f),rgb_u32(rgb,alpha),16);
}
}
SettingsMenu::SettingsMenu(LiveSettings& live,std::string language,std::function<const FeatureReport*()> features,uint32_t monitor_width,uint32_t monitor_height,RendererBackend backend)
    : live_(live),language_(std::move(language)),features_(std::move(features)),monitor_width_(monitor_width),monitor_height_(monitor_height),backend_ui_(backend_ui(backend)),editor_(live.bindings()),pad_editor_(live.options().gamepad.bindings,BindingTable::gamepad),wii_editor_(live.options().wiimote.bindings,BindingTable::wiimote),
      p2_editor_(live.options().player2.bindings),p2_pad_editor_(live.options().player2.gamepad.bindings,BindingTable::gamepad) {}
const char* SettingsMenu::tr(std::string_view key) const { return launcher::text(language_,key).data(); }
void SettingsMenu::setup() {
    fonts_=ui::load_fonts(*ImGui::GetIO().Fonts);   // the previous menu context (if any) is gone: its atlas no longer reads the old bytes
}
void SettingsMenu::capture_event(const SDL_Event& event) {
    auto& editor=p2_editor_.capturing() ? p2_editor_ : editor_;   // the table of the player whose binding waits for an input
    if((event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) && !event.key.repeat) editor.event({PhysicalKind::key,uint16_t(event.key.scancode)},event.key.down);
    else if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) editor.event({PhysicalKind::mouse,event.button.button},event.button.down);
    else if(event.type==SDL_EVENT_MOUSE_WHEEL) editor.wheel(event.wheel.y==0 ? 0 : ((event.wheel.y>0)!=(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED) ? 1 : -1));
    else if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) editor.focus(false);
}
void SettingsMenu::commit() {
    if(mod_menu_) mod_menu_->commit();   // every loop, menu open or not: a script may change its settings at any time
    if(pending_) { const auto next=*pending_; pending_.reset(); live_.apply(next); }
    if(!editor_.capturing() && editor_.bindings()!=live_.bindings()) live_.apply(editor_.bindings());
    if(!p2_editor_.capturing() && p2_editor_.bindings()!=live_.options().player2.bindings) { auto next=live_.options(); next.player2.bindings=p2_editor_.bindings(); live_.apply(next); }   // the second player's table is part of the options
    if(second_pick_) { const auto pick=*second_pick_; second_pick_.reset(); if(second_command_) second_command_(pick); }   // the answer to the new-device window, or the disconnect button
}
template<class Content> void SettingsMenu::page(const char* id,Content&& content) {
    const float footer=live_.error() ? ImGui::GetTextLineHeightWithSpacing()*2 : 0;
    ImGui::BeginChild(id,{0,-footer});
    content();
    ImGui::EndChild();
}
// A button was pressed on a new device in a menu: who plays with it? (the same window recipe as the binding error's). Call inside an ImGui window.
void SettingsMenu::new_device_modal(float unit) {
    if(!second_view_.available || !second_view_.asking) return;
    const auto& io=ImGui::GetIO();
    if(!ImGui::IsPopupOpen("###coop-new-device")) ImGui::OpenPopup("###coop-new-device");
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{20*unit,20*unit});
    if(ImGui::BeginPopupModal("###coop-new-device",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)) {
        const char* kinds[]={"","coop_gamepad","coop_wiimote","coop_pointer"};
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+520*unit);
        ImGui::PushFont(fonts_.serif,heading_size_); ImGui::TextWrapped("%s",tr("coop_new_device")); ImGui::PopFont();
        ImGui::TextWrapped("%s",second_view_.asking>=1 && second_view_.asking<=3 ? tr(kinds[second_view_.asking]) : "");
        ImGui::Separator();
        ImGui::TextWrapped("%s",tr("coop_new_device_ask"));
        if(second_view_.replaces) ImGui::TextDisabled("%s",tr("coop_replaces"));
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0,6*unit});
        if(ImGui::Button(tr("coop_answer_first"))) second_pick_=SecondCommand::first;
        ImGui::SameLine();
        if(ImGui::Button(tr("coop_answer_second"))) second_pick_=SecondCommand::second;
        ImGui::SameLine();
        if(ImGui::Button(tr("coop_not_now")) || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) second_pick_=SecondCommand::not_now;
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
void SettingsMenu::build() {
    { std::lock_guard lock(second_mutex_); second_view_=second_state_; }   // one snapshot for the whole frame
    const auto& io=ImGui::GetIO();
    const float font_scale=std::max(.75f,io.DisplaySize.y/1080.f);
    const float unit=font_scale*.75f;
    const float font_size=std::round(22*font_scale);
    launcher::graphite_style(); ImGui::GetStyle().ScaleAllSizes(unit);
    const float previous_frame_height=font_size+2*std::floor(11*font_scale);
    ImGui::GetStyle().FramePadding.y=(previous_frame_height*.75f-font_size)*.5f;
    ImGui::GetStyle().FrameBorderSize=ImGui::GetStyle().PopupBorderSize=1;   // ScaleAllSizes floors thin borders to zero below 1x
    ImGui::GetStyle().WindowPadding={28*unit,22*unit};
    if(second_view_.available && second_view_.asking && second_view_.question_only) {   // the question opened the menu alone: only its window, none of the menu behind it
        heading_size_=std::round(32*font_scale);
        ImGui::PushFont(fonts_.sans,font_size);
        ImGui::SetNextWindowPos({0,0});
        ImGui::Begin("##coop-question",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs);
        new_device_modal(unit);
        ImGui::End();
        ImGui::PopFont();
        return;
    }
    const ImVec2 size{std::min(io.DisplaySize.x-40*font_scale,660*font_scale),std::min(io.DisplaySize.y-40*font_scale,900*font_scale)};
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Once,{.5f,.5f});
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.94f);
    ImGui::PushFont(fonts_.sans,font_size);
    ImGui::Begin("##settings",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    heading_size_=std::round(32*font_scale);
    ImGui::PushFont(fonts_.serif,heading_size_);ImGui::TextWrapped("%s",tr("menu_title"));ImGui::PopFont();
    ImGui::Dummy({0,6*unit});
    if(ImGui::BeginTabBar("tabs",ImGuiTabBarFlags_FittingPolicyScroll)) {   // tabs that do not fit (mods add more) scroll with arrow buttons
        if(ImGui::BeginTabItem(tr("menu_main"))) { main_tab(unit); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem(tr("effects"))) { effects_tab(unit); ImGui::EndTabItem(); }
        if(backend_ui_.dlss5 && shown("dlss5") && current().dlss5_enabled && ImGui::BeginTabItem(tr("dlss5"))) { dlss5_tab(unit); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem(tr("menu_controls"))) { page("controls-page",[&] { controls_tab(unit); }); ImGui::EndTabItem(); }
        if(shown("pad_panel") && ImGui::BeginTabItem(tr("controllers"))) { page("controllers-page",[&] { controllers_tab(unit); }); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem(tr("reticle"))) { reticle_tab(unit); ImGui::EndTabItem(); }
        if(mod_menu_) {   // mods: one manager tab; each mod's settings open in a window from there
            if(ImGui::BeginTabItem((mod_menu_->manager_title()+"###mods").c_str())) { page("mods-page",[&] { mod_menu_->manager_tab(unit); }); ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    if(const auto& error=live_.error()) {
        ImGui::PushStyleColor(ImGuiCol_Text,{1,.52f,.52f,1});
        ImGui::TextWrapped("%s",tr(error->find("changed since loading")!=std::string::npos ? "stale" : "error"));
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",error->c_str());
    }
    new_device_modal(unit);
    ImGui::End();
    ImGui::PopFont();
}
void SettingsMenu::main_tab(float unit) {
    auto options=current();
    const auto* features=features_();
    if(features)disable_unsupported_upscaling(options,*features);
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("graphics",{0,0});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    if(shown("render_scale")) {
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
    }
    if(shown("fps_limit")) {
    ImGui::TextUnformatted(tr("fps_limit"));
    const uint32_t current_limit=options.fps_limit.value_or(options.unlock_60fps ? 60 : 30);
    const auto limit_label=[&](uint32_t value) { return value ? std::to_string(value)+" FPS" : std::string(tr("unlimited")); };
    if(ImGui::BeginCombo("##fps",limit_label(current_limit).c_str(),ImGuiComboFlags_HeightLarge)) {
        for(const uint32_t value:kLimits) {
            if(kMaxFrameLimit && (value==0 || value>kMaxFrameLimit)) continue;   // Linux: nothing above the cap, and no "unlimited"
            if(ImGui::Selectable(limit_label(value).c_str(),current_limit==value)) options.fps_limit=value;
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    }
    const auto supported=[&](UpscalerKind kind,bool generation,std::string& why) {
        if(kind==UpscalerKind::off) return true;
        if(failed_.contains(kind) || (generation && failed_generation_.contains(kind))) { why=tr("launcher_only"); return false; }
        if(!features) { why=tr("checking"); return false; }
        if(!features->effects) { why=tr("effects_unavailable"); return false; }
        if(kind==UpscalerKind::smaa) { if(!features->effects) why=tr("effects_unavailable"); return features->effects; }   // SMAA runs on the effects path and has no generator
        const bool ok=kind==UpscalerKind::dlss ? (generation ? features->dlss_g : features->dlss) : (generation ? features->fsr_fg : features->fsr);
        const auto& reason=kind==UpscalerKind::dlss ? (generation ? features->dlss_g_reason : features->dlss_reason) : (generation ? features->fsr_fg_reason : features->fsr_reason);
        if(!ok) why=reason.empty() ? tr("unsupported") : reason;
        return ok;
    };
    const auto kind_label=[&](UpscalerKind kind) { return kind==UpscalerKind::dlss ? "DLSS" : kind==UpscalerKind::fsr && backend_ui_.fsr ? "FSR" : kind==UpscalerKind::smaa ? "SMAA" : tr("off"); };
    if(shown("upscaling")) {
    ImGui::TextUnformatted(tr("upscaling"));
    if(ImGui::BeginCombo("##upscaling",kind_label(options.upscaler))) {
        for(const auto kind:{UpscalerKind::off,UpscalerKind::smaa,UpscalerKind::dlss,UpscalerKind::fsr}) {
            if(kind==UpscalerKind::fsr && !backend_ui_.fsr) continue;
            std::string why; const bool ok=supported(kind,false,why);
            ImGui::BeginDisabled(!ok);
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
    }
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
    if(shown("upscale_mode")) {
    ImGui::TextUnformatted(tr("quality"));
    ImGui::BeginDisabled(!scaled);
    if(ImGui::BeginCombo("##quality",scaled ? mode_label(options.upscale_mode).c_str() : "—")) {
        for(const auto mode:kModes) if(ImGui::Selectable(mode_label(mode).c_str(),options.upscale_mode==mode)) options.upscale_mode=mode;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Dummy({0,6*unit});
    }
    if(shown("sharpening")) {
    ImGui::BeginDisabled(!upscaling);
    const auto sharpening_label=std::string(tr("sharpening"))+" (RCAS)";
    compact_checkbox("##sharpening",sharpening_label.c_str(),options.sharpening,unit);
    ImGui::BeginDisabled(!options.sharpening);
    float sharpness=float(options.sharpness);
    if(ImGui::SliderFloat("##sharpness",&sharpness,.01f,1.f,"%.2f")) options.sharpness=std::clamp(std::round(double(sharpness)*100)/100,.01,1.);
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::Dummy({0,6*unit});
    }
    if(backend_ui_.frame_generation && shown("frame_generation")) {
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
            ImGui::BeginDisabled(!ok);
            if(ImGui::Selectable(choice_label(value).c_str(),choice==value)) options=with_frame_generation(options,value);
            ImGui::EndDisabled();
            if(!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",why.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    }
    if(shown("hud_position")) {
    ImGui::TextUnformatted(tr("hud_position"));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr("hud_hint"));
    const char* hud_keys[]={"hud_auto","hud_center","hud_edges"};
    if(ImGui::BeginCombo("##hud_position",tr(hud_keys[int(options.hud_position)]))) {
        for(int i=0;i<3;++i) if(ImGui::Selectable(tr(hud_keys[i]),int(options.hud_position)==i)) options.hud_position=HudPosition(i);
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr("hud_hint"));
    }
    ImGui::PopItemWidth();
    if(shown("hdr")) {
    ImGui::Dummy({0,14*unit}); ImGui::SeparatorText(tr("hdr"));
    compact_checkbox("##hdr",tr("hdr"),options.hdr.enabled,unit);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr("hdr_hint"));
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    ImGui::BeginDisabled(!options.hdr.enabled);
    const auto nits_slider=[&](const char* id,const char* title,uint32_t& value,uint32_t low,uint32_t high) {
        ImGui::Dummy({0,6*unit}); ImGui::TextUnformatted(tr(title));
        int nits=int(value);
        if(launcher::slider_int(id,nits,int(low),int(high),"%d nits")) value=uint32_t(std::clamp(nits,int(low),int(high)));
    };
    nits_slider("##hdr_peak","hdr_peak",options.hdr.peak_nits,kHdrPeakMin,kHdrPeakMax);
    nits_slider("##hdr_paper","hdr_paper_white",options.hdr.paper_white_nits,kHdrWhiteMin,kHdrWhiteMax);
    nits_slider("##hdr_ui","hdr_ui",options.hdr.ui_nits,kHdrWhiteMin,kHdrWhiteMax);
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    }
    ImGui::EndChild();
    pending_=options;
}
void SettingsMenu::effects_tab(float unit) {
    auto options=current();
    const auto* features=features_();
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("effects",{0,0});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    if(shown("anisotropy")) {
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
    }
    // Effects that need the screen-space path are greyed out on a GPU without it; a saved "on" stays visible and can be switched off.
    const bool path=!features || features->effects;
    if(shown("per_pixel_lighting")) {
    ImGui::BeginDisabled(!path && !options.effects.per_pixel_lighting);
    compact_checkbox("##ppl",tr("per_pixel_lighting"),options.effects.per_pixel_lighting,unit);
    ImGui::EndDisabled();
    if(!path && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr("effects_unavailable"));
    }
    const auto preset_label=[&](EffectLevel level) { return level==EffectLevel::off ? std::string(tr("off")) : std::string(tr(level==EffectLevel::low ? "level_low" : level==EffectLevel::medium ? "level_medium" : level==EffectLevel::high ? "level_high" : "level_ultra")); };
    const auto rt=backend_ui_.ray_tracing ? rt_ui(options.effects,features!=nullptr,features && features->ray_tracing) : RtUi{false,false,false};
    const auto level_combo=[&](const char* id,const char* title,EffectLevel& value,bool replaced=false) {   // the presets of an effect; greyed out like the other screen-space effects (replaced: ray tracing does it instead)
        ImGui::Dummy({0,6*unit});
        ImGui::TextUnformatted(tr(title));
        ImGui::BeginDisabled((!path && value==EffectLevel::off) || replaced);
        if(ImGui::BeginCombo(id,preset_label(value).c_str())) {
            for(const auto level:{EffectLevel::off,EffectLevel::low,EffectLevel::medium,EffectLevel::high,EffectLevel::ultra}) if(ImGui::Selectable(preset_label(level).c_str(),value==level)) value=level;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if((!path || replaced) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr(replaced ? "replaced_by_rt" : "effects_unavailable"));
    };
    if(shown("ambient_occlusion")) level_combo("##ao","ambient_occlusion",options.effects.ambient_occlusion,rt.raster_replaced);
    if(shown("motion_blur")) level_combo("##blur","motion_blur",options.effects.motion_blur);
    if(shown("contact_shadows")) {
    ImGui::Dummy({0,6*unit});
    ImGui::BeginDisabled((!path && !options.effects.contact_shadows) || rt.raster_replaced);
    compact_checkbox("##contact",tr("contact_shadows"),options.effects.contact_shadows,unit);
    ImGui::EndDisabled();
    if((!path || rt.raster_replaced) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tr(rt.raster_replaced ? "replaced_by_rt" : "effects_unavailable"));
    }
    // Ray tracing (path tracing): the checkbox and its quality; it lights the scene instead of the shadow map, the contact shadows and GTAO above.
    if(backend_ui_.ray_tracing && shown("ray_tracing")) {
    ImGui::Dummy({0,6*unit});
    ImGui::BeginDisabled(!rt.master_enabled);
    compact_checkbox("##rt",tr("ray_tracing"),options.effects.path_tracing,unit);
    ImGui::EndDisabled();
    if(!rt.master_enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const std::string why=std::string(tr("rt_unavailable"))+(features && !features->ray_tracing_reason.empty() ? " ("+features->ray_tracing_reason+")" : "");
        ImGui::SetTooltip("%s",why.c_str());
    }
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("rt_quality"));
    ImGui::BeginDisabled(!rt.quality_enabled);
    if(ImGui::BeginCombo("##rt-quality",preset_label(options.effects.rt_quality).c_str())) {
        for(const auto level:{EffectLevel::low,EffectLevel::medium,EffectLevel::high,EffectLevel::ultra}) if(ImGui::Selectable(preset_label(level).c_str(),options.effects.rt_quality==level)) options.effects.rt_quality=level;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    }
    if(shown("bloom")) {
    ImGui::Dummy({0,6*unit});
    compact_checkbox("##bloom",tr("bloom"),options.bloom,unit);
    ImGui::BeginDisabled(!options.bloom);
    ImGui::Dummy({0,6*unit}); ImGui::TextUnformatted(tr("bloom_strength"));
    int bloom_strength=int(options.bloom_strength);
    if(launcher::slider_int("##bloom_strength",bloom_strength,0,int(kBloomStrengthMax),"%d%%")) options.bloom_strength=uint32_t(std::clamp(bloom_strength,0,int(kBloomStrengthMax)));
    ImGui::EndDisabled();
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    pending_=options;
}
// The DLSS 5 tab: a status line, the switch and one control per field of the settings table (dlss5_settings.h); everything but the switch follows the switch.
void SettingsMenu::dlss5_tab(float unit) {
    auto options=current();
    const auto* features=features_();
    const auto runtime=dlss5_report_ ? dlss5_report_() : Dlss5Report{};
    const auto status=dlss5_status(features!=nullptr,features && features->dlss5,features ? features->dlss5_reason : std::string(),runtime.reason);
    const auto panel=dlss5_ui(options.dlss5,status.state!=Dlss5State::checking,status.state==Dlss5State::available);
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("dlss5",{0,0});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    if(status.state==Dlss5State::unavailable) {
        const std::string text=std::string(tr("dlss5_status_unavailable"))+(status.reason.empty() ? "" : ": "+status.reason);
        ImGui::PushStyleColor(ImGuiCol_Text,{1,.52f,.52f,1}); ImGui::TextWrapped("%s",text.c_str()); ImGui::PopStyleColor();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",text.c_str());
    } else if(status.state==Dlss5State::checking) ImGui::TextDisabled("%s",tr("checking"));
    else ImGui::TextUnformatted(tr(runtime.active ? "dlss5_status_active" : "dlss5_status_available"));
    ImGui::Dummy({0,6*unit});
    ui::Dlss5PanelHooks hooks;
    hooks.tr=[this](std::string_view key) { return tr(key); };
    hooks.open_debug=open_debug_;
    ui::dlss5_panel(options.dlss5,panel,hooks);
    ImGui::PopItemWidth();
    ImGui::EndChild();
    pending_=options;
}
void SettingsMenu::controllers_tab(float unit) {
    auto options=current();
    // Bindings changed from outside (a script, the test server) are shown: the editor follows the saved table when it is not the editor's own pending change.
    if(!pad_editor_.capturing() && options.gamepad.bindings!=pad_editor_.bindings() && options.gamepad.bindings!=pad_seen_) pad_editor_=BindingEditor(options.gamepad.bindings,BindingTable::gamepad);
    pad_seen_=options.gamepad.bindings;
    pad_panel_.heading=fonts_.serif; pad_panel_.heading_size=heading_size_; pad_panel_.control_width=.75f;   // the menu's own title font and control width
    const PadStatus status=pad_status_ ? pad_status_() : PadStatus{};
    if(gamepad_tab(pad_panel_,options.gamepad,options.rumble,pad_editor_,status,unit,[this](std::string_view key) { return tr(key); })) pending_=options;
    if(!wii_editor_.capturing() && options.wiimote.bindings!=wii_editor_.bindings() && options.wiimote.bindings!=wii_seen_) wii_editor_=BindingEditor(options.wiimote.bindings,BindingTable::wiimote);
    wii_seen_=options.wiimote.bindings;
    wii_panel_.heading=fonts_.serif; wii_panel_.heading_size=heading_size_; wii_panel_.control_width=.75f;
    options=current();   // the gamepad tab above may have queued a change: this one builds on it
    if(wiimote_tab(wii_panel_,options.wiimote,wii_editor_,wii_status_ ? wii_status_() : WiimoteStatus{},unit,[this](std::string_view key) { return tr(key); })) pending_=options;
    const auto& second=second_view_;
    if(!second.available) return;   // local two-player play is off here (the Steam Deck, network play, a test run)
    // The second player: who it is, the command that sends the device back, the gamepad window of its own (its own table, sticks and gyro); the Wii Remote has no table of its own.
    options=current();
    if(!p2_pad_editor_.capturing() && options.player2.gamepad.bindings!=p2_pad_editor_.bindings() && options.player2.gamepad.bindings!=p2_pad_seen_) p2_pad_editor_=BindingEditor(options.player2.gamepad.bindings,BindingTable::gamepad);
    p2_pad_seen_=options.player2.gamepad.bindings;
    ImGui::Dummy({0,14*unit});
    ImGui::PushFont(fonts_.serif,heading_size_); ImGui::TextWrapped("%s",tr("coop_title")); ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    if(second.joined) {
        const char* kinds[]={"","coop_gamepad","coop_wiimote","coop_pointer"};
        ImGui::TextWrapped("%s: %s",tr("coop_device"),second.device>=1 && second.device<=3 ? tr(kinds[second.device]) : "");
        if(ImGui::Button(tr("coop_disconnect"))) second_pick_=SecondCommand::disconnect;
    } else ImGui::TextWrapped("%s",tr("coop_waiting"));
    if(!second.problem.empty()) { ImGui::PushStyleColor(ImGuiCol_Text,{1,.52f,.52f,1}); ImGui::TextWrapped("%s",second.problem.c_str()); ImGui::PopStyleColor(); }
    ImGui::TextDisabled("%s",tr("coop_wii_note"));
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0,6*unit});
    ImGui::PushID("player2");   // the same window as the first player's: its own ids
    p2_pad_panel_.heading=fonts_.serif; p2_pad_panel_.heading_size=heading_size_; p2_pad_panel_.control_width=.75f;
    GamepadSettings pad=options.player2.gamepad; uint32_t rumble=options.rumble;   // the vibration strength is one setting for both players: this slider is the same one
    if(gamepad_tab(p2_pad_panel_,pad,rumble,p2_pad_editor_,PadStatus{},unit,[this](std::string_view key) { return tr(key); })) { options.player2.gamepad=pad; options.rumble=rumble; pending_=options; }
    ImGui::PopID();
}
// The Reticle tab: a preview of the reticle with the chosen look (yours and a partner's), the size, what to hide, the players' colours, the dot's size, the wake's
// length and how visible the partner's reticle is.
void SettingsMenu::reticle_tab(float unit) {
    auto options=current();
    auto& reticle=options.reticle;
    ImGui::Dummy({0,8*unit});
    ImGui::BeginChild("reticle",{0,0});
    if(shown("reticle_preview")) {   // the preview
        const float height=180*unit, width=ImGui::GetContentRegionAvail().x;
        const auto origin=ImGui::GetCursorScreenPos(); auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilledMultiColor(origin,{origin.x+width,origin.y+height},IM_COL32(52,46,38,255),IM_COL32(30,36,34,255),IM_COL32(18,18,20,255),IM_COL32(40,34,30,255));
        draw->AddRect(origin,{origin.x+width,origin.y+height},IM_COL32(255,255,255,40));
        const float scale=options.crosshair_scale/100.f, radius=46*unit;
        const ImVec2 box_max{origin.x+width,origin.y+height};
        draw->PushClipRect(origin,box_max,true);
        draw_reticle_sample(*draw,{origin.x+width*.30f,origin.y+height*.5f},radius,reticle,reticle.color_p1 ? reticle.color_p1 : kDefaultReticleColor[0],1,scale,true,unit,origin,box_max);
        draw_reticle_sample(*draw,{origin.x+width*.72f,origin.y+height*.5f},radius,reticle,reticle.color_p2 ? reticle.color_p2 : kDefaultReticleColor[1],reticle.partner_opacity/100.f,scale,false,unit,origin,box_max);
        draw->PopClipRect();
        ImGui::Dummy({width,height});
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr("reticle_preview"));
    }
    ImGui::Dummy({0,8*unit});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    const auto slider=[&](const char* id,const char* title,uint32_t& value,uint32_t low,uint32_t high) {
        ImGui::TextUnformatted(tr(title));
        int shown=int(value);
        if(launcher::slider_int(id,shown,int(low),int(high),"%d%%")) value=uint32_t(std::clamp(shown,int(low),int(high)));
        ImGui::Dummy({0,6*unit});
    };
    if(shown("crosshair_scale")) slider("##crosshair","crosshair_scale",options.crosshair_scale,0,100);
    ImGui::PopItemWidth();
    if(shown("hide_tail")) compact_checkbox("##hide_tail",tr("hide_tail"),reticle.hide_tail,unit);
    if(shown("hide_dot")) compact_checkbox("##hide_dot",tr("hide_dot"),reticle.hide_dot,unit);
    if(shown("hide_marks")) compact_checkbox("##hide_marks",tr("hide_marks"),reticle.hide_marks,unit);
    if(shown("hide_arc")) compact_checkbox("##hide_arc",tr("hide_arc"),reticle.hide_arc,unit);
    ImGui::Dummy({0,6*unit});
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    ImGui::BeginDisabled(reticle.hide_tail);
    if(shown("tail_length")) slider("##tail_length","tail_length",reticle.tail_length,kReticleTailMin,kReticleTailMax);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(reticle.hide_dot);
    if(shown("dot_size")) slider("##dot_size","dot_size",reticle.dot_size,kReticleDotMin,kReticleDotMax);
    ImGui::EndDisabled();
    if(shown("partner_opacity")) slider("##partner_opacity","partner_opacity",reticle.partner_opacity,0,100);
    ImGui::PopItemWidth();
    const auto colour=[&](const char* id,const char* title,uint32_t& value,uint32_t fallback) {
        ImGui::TextUnformatted(tr(title));
        float channels[3]={float((((value ? value : fallback)>>16)&255))/255,float((((value ? value : fallback)>>8)&255))/255,float(((value ? value : fallback)&255))/255};
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.5f);
        if(ImGui::ColorEdit3(id,channels,ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_DisplayHex|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_PickerHueWheel)) {
            const auto byte=[](float v) { return uint32_t(std::clamp(v,0.f,1.f)*255+.5f); };
            value=std::max<uint32_t>(0x000001,byte(channels[0])<<16|byte(channels[1])<<8|byte(channels[2]));   // 0 is "the game's own"
        }
        ImGui::SameLine(0,10*unit);
        ImGui::BeginDisabled(!value);
        if(ImGui::Button((std::string(tr("reticle_default"))+"##"+id).c_str())) value=0;
        ImGui::EndDisabled();
        ImGui::Dummy({0,6*unit});
    };
    if(shown("reticle_color_p1")) colour("##color_p1","reticle_color_p1",reticle.color_p1,kDefaultReticleColor[0]);
    if(shown("reticle_color_p2")) colour("##color_p2","reticle_color_p2",reticle.color_p2,kDefaultReticleColor[1]);
    ImGui::EndChild();
    pending_=options;
}
void SettingsMenu::controls_tab(float unit) {
    auto options=current();
    ImGui::Dummy({0,8*unit});
    if(shown("pointer_device")) {
    ImGui::TextUnformatted(tr("pointer_device"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    const char* devices[]={tr("pointer_mouse"),tr("pointer_lightgun")};
    if(ImGui::BeginCombo("##pointer",devices[size_t(options.pointer)])) {
        for(size_t i=0;i<2;++i) if(ImGui::Selectable(devices[i],size_t(options.pointer)==i)) options.pointer=PointerMode(i);
        ImGui::EndCombo();
    }
    ImGui::Dummy({0,6*unit});
    }
    const bool absolute=options.pointer==PointerMode::absolute;   // the position of the cursor is the aim: the mouse speed means nothing, a jittery gun wants smoothing
    if(shown("mouse_sensitivity")) {
    ImGui::BeginDisabled(absolute);
    ImGui::TextUnformatted(tr("mouse_sensitivity"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    int sensitivity=int(options.mouse_sensitivity);
    if(launcher::slider_int("##sensitivity",sensitivity,int(kMouseSensitivityMin),int(kMouseSensitivityMax),"%d%%")) options.mouse_sensitivity=uint32_t(std::clamp(sensitivity,int(kMouseSensitivityMin),int(kMouseSensitivityMax)));
    ImGui::EndDisabled();
    }
    if(absolute && shown("pointer_smoothing")) {
        ImGui::Dummy({0,6*unit});
        ImGui::TextUnformatted(tr("pointer_smoothing"));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
        int smoothing=int(options.pointer_smoothing);
        if(launcher::slider_int("##pointer-smoothing",smoothing,0,int(kPointerSmoothingMax),"%d%%")) options.pointer_smoothing=uint32_t(std::clamp(smoothing,0,int(kPointerSmoothingMax)));
    }
    if(shown("reload_zone")) {
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("reload_zone"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    const char* zones[]={tr("reload_zone_off"),tr("reload_zone_corner"),tr("reload_zone_edge")};
    if(ImGui::BeginCombo("##reload-zone",zones[size_t(options.reload_zone)])) {
        for(size_t i=0;i<3;++i) if(ImGui::Selectable(zones[i],size_t(options.reload_zone)==i)) options.reload_zone=ReloadZone(i);
        ImGui::EndCombo();
    }
    if(options.reload_zone!=ReloadZone::off && shown("reload_zone_size")) {
        ImGui::TextUnformatted(tr("reload_zone_size"));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
        int size=int(options.reload_zone_size);
        if(launcher::slider_int("##reload-zone-size",size,int(kReloadZoneSizeMin),int(kReloadZoneSizeMax),"%d%%")) options.reload_zone_size=uint32_t(std::clamp(size,int(kReloadZoneSizeMin),int(kReloadZoneSizeMax)));
    }
    }
    if(shown("prompt_style")) {
    ImGui::Dummy({0,6*unit});
    ImGui::TextUnformatted(tr("style"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
    const char* styles[]={tr("gray"),tr("cyan")};
    if(ImGui::BeginCombo("##style",styles[size_t(options.prompt_style)])) {
        for(size_t i=2;i-->0;) if(ImGui::Selectable(styles[i],size_t(options.prompt_style)==i)) options.prompt_style=PromptStyle(i);   // cyan (Style 1) first
        ImGui::EndCombo();
    }
    {   // the three mouse pictures of the chosen style
        const float ph=ImGui::GetFrameHeight(); const auto p=ImGui::GetCursorScreenPos(); auto* draw=ImGui::GetWindowDrawList();
        for(int i=0;i<3;++i) draw_mouse_icon(*draw,{p.x+i*ph,p.y},{ph*.65f,ph},{i==2 ? PhysicalKind::wheel : PhysicalKind::mouse,uint16_t(i==1 ? 3 : 1)},false,options.prompt_style);
        ImGui::Dummy({ph*3,ph});
    }
    }
    pending_=options;
    if(!shown("key_bindings")) return;
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
    const auto& second=second_view_;
    if(!second.available) player_view_=0;
    if(second.available) {   // the second player's table is edited with the same widgets: below, `editor_` is the table of the player this shows
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.75f);
        const char* players[]={tr("player_one"),tr("player_two")};
        if(ImGui::BeginCombo("##bindings-player",players[player_view_])) {
            for(int i=0;i<2;++i) if(ImGui::Selectable(players[i],player_view_==i)) player_view_=i;
            ImGui::EndCombo();
        }
        if(player_view_==1) {
            if(!p2_editor_.capturing() && options.player2.bindings!=p2_editor_.bindings() && options.player2.bindings!=p2_seen_) p2_editor_=BindingEditor(options.player2.bindings);
            p2_seen_=options.player2.bindings;
            ImGui::TextDisabled("%s",tr("coop_mouse_note"));
        }
        ImGui::Dummy({0,4*unit});
    }
    auto& editor_=player_view_==1 ? p2_editor_ : this->editor_;   // shadows the member on purpose: every use below is the shown player's table
    if(ImGui::Button(tr("defaults"))) editor_.restore_defaults();
    if(editor_.capturing()) { ImGui::SameLine(); ImGui::TextWrapped("%s",tr("capture")); }
    if(editor_.error()) { binding_error_=*editor_.error(); editor_.cancel_capture(); ImGui::OpenPopup("###binding-error"); }   // the capture ends: the next click goes to the dialog, not into a new binding
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x/2,ImGui::GetIO().DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{20*unit,20*unit});
    if(ImGui::BeginPopupModal("###binding-error",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+520*unit);
        ImGui::TextWrapped("%s",tr("rejected")); ImGui::Separator();
        ImGui::TextWrapped("%s",binding_error_.c_str());
        ImGui::PopTextWrapPos();
        if(ImGui::Button(tr("close")) || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::BeginDisabled(editor_.capturing().has_value());
    {   // the page scrolls as a whole (see page()), the table with it
        for(size_t i=0;i<size_t(Action::recenter);++i) {   // recenter is a gamepad action: the controllers tab lists it, this page is keyboard and mouse only
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
    ImGui::EndDisabled();
}
}
