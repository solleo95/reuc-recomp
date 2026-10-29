#include "dlss5_panel.h"
#include "graphite.h"
#include "public_release.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace uc::ui {
namespace {
std::string choice_text(const Dlss5Field& field, size_t index, const Dlss5PanelHooks& hooks) {
    if(field.kind==Dlss5Kind::boolean) return hooks.tr(index ? "dlss5_on" : "dlss5_off");
    const auto& choice=field.choices[index];
    return choice.label.empty() ? std::string(hooks.tr("dlss5_"+std::string(choice.name))) : std::string(choice.label);
}
// A small square button with a circular arrow: reset to default (greyed while the value is the default).
bool reset_button(const std::string& id, bool at_default, const char* tooltip) {
    const float size=ImGui::GetFrameHeight();
    ImGui::BeginDisabled(at_default);
    const bool clicked=ImGui::Button(("##reset-"+id).c_str(),{size,size});
    ImGui::EndDisabled();
    const auto a=ImGui::GetItemRectMin(), b=ImGui::GetItemRectMax();
    const ImVec2 center{(a.x+b.x)/2,(a.y+b.y)/2};
    const float radius=size*.26f, thickness=std::max(1.f,size*.07f);
    const auto color=ImGui::GetColorU32(at_default ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    auto* draw=ImGui::GetWindowDrawList();
    draw->PathArcTo(center,radius,-1.2f,3.6f,18); draw->PathStroke(color,0,thickness);   // almost a full circle, open at the top right
    const ImVec2 tip{center.x+radius*std::cos(-1.2f),center.y+radius*std::sin(-1.2f)};
    draw->AddTriangleFilled(tip,{tip.x+radius*.7f,tip.y-radius*.1f},{tip.x+radius*.05f,tip.y-radius*.75f},color);   // the arrow head
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip) launcher::tooltip(tooltip);
    return clicked;
}
}
bool dlss5_panel(Dlss5Settings& settings, const Dlss5Ui& ui, const Dlss5PanelHooks& hooks) {
    bool changed=false;
    const auto& style=ImGui::GetStyle();
    const float full=ImGui::GetContentRegionAvail().x;
    const float control=std::floor(full*.56f);   // the rest is the reset button and the label, which wraps
    const auto row=[&](const Dlss5Field& field) {
        const std::string key(field.key), id="##d5-"+key;
        const bool master=field.key=="hook";
        ImGui::PushID(key.c_str());
        ImGui::BeginDisabled(master ? !ui.switch_enabled : !ui.settings_enabled);
        const double before=field.get(settings);
        ImGui::AlignTextToFramePadding();
        if(!field.choices.empty()) {
            const size_t current=size_t(before);
            bool bar=field.control==Dlss5Control::bar;
            std::vector<float> wanted;   // a bar: each button as wide as its name plus padding, scaled to fill the control; a selector when the names do not fit
            float need=0;
            if(bar) {
                for(size_t i=0;i<field.choices.size();++i) { wanted.push_back(ImGui::CalcTextSize(choice_text(field,i,hooks).c_str()).x+style.FramePadding.x*.9f); need+=wanted.back(); }
                bar=need<=control;
            }
            if(bar) {
                float used=0;
                for(size_t i=0;i<field.choices.size();++i) {
                    if(i) ImGui::SameLine(0,0);
                    const bool selected=i==current;
                    const float width=i+1==field.choices.size() ? control-used : std::floor(control*wanted[i]/need);
                    used+=width;
                    ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(selected ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);
                    ImGui::PushID(int(i));
                    if(ImGui::Button(choice_text(field,i,hooks).c_str(),{width,0})) field.set(settings,double(i));
                    ImGui::PopID(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
                    if(hooks.track) hooks.track(id+"-"+std::string(field.choices[i].name));
                }
            } else {
                ImGui::SetNextItemWidth(control);
                if(ImGui::BeginCombo(id.c_str(),choice_text(field,current,hooks).c_str())) {
                    for(size_t i=0;i<field.choices.size();++i) {
                        if(ImGui::Selectable(choice_text(field,i,hooks).c_str(),i==current)) field.set(settings,double(i));
                        if(hooks.track) hooks.track(id+"-"+std::string(field.choices[i].name));
                    }
                    ImGui::EndCombo();
                }
                if(hooks.track) hooks.track(id);
            }
        } else {
            ImGui::SetNextItemWidth(control);
            if(field.kind==Dlss5Kind::integer) {
                int value=int(before);
                if(launcher::slider_int(id.c_str(),value,int(field.min),int(field.max))) field.set(settings,double(value));
            } else {
                const double scale=field.format==Dlss5Format::percent ? 100.0 : 1.0;
                float value=float(before*scale);
                const char* format=field.format==Dlss5Format::percent ? "%.0f%%" : field.format==Dlss5Format::multiplier ? "%.2fx" : field.format==Dlss5Format::nits ? "%.0f" : "%.2f";
                if(ImGui::SliderFloat(id.c_str(),&value,float(field.min*scale),float(field.max*scale),format)) {
                    const double snapped=std::clamp(std::round(double(value)/scale/field.step)*field.step,field.min,field.max);
                    field.set(settings,std::round(snapped*1e6)/1e6);
                }
            }
            if(hooks.track) hooks.track(id);
        }
        if(field.resettable) {
            ImGui::SameLine(0,style.ItemInnerSpacing.x*2);
            if(reset_button(key,field.get(settings)==dlss5_field_default(field),hooks.tr("dlss5_reset"))) field.set(settings,dlss5_field_default(field));
            if(hooks.track) hooks.track(id+"-reset");
            ImGui::SameLine(0,style.ItemInnerSpacing.x*2);
        } else ImGui::SameLine(0,style.ItemInnerSpacing.x*2+ImGui::GetFrameHeight()+style.ItemInnerSpacing.x*2);   // the labels line up
        ImGui::TextWrapped("%s",hooks.tr("dlss5_"+key));
        ImGui::EndDisabled();
        if(master) {   // a line under the bar says what the chosen method does
            ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s",hooks.tr("dlss5_hook_desc_"+std::string(field.choices[size_t(field.get(settings))].name)));
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        changed|=field.get(settings)!=before;
        ImGui::Dummy({0,style.ItemSpacing.y*.4f});
    };
    const auto section=[&](Dlss5Section which, const char* title_key) {
        if(which!=Dlss5Section::main) {
            if(!ImGui::CollapsingHeader(hooks.tr(title_key),ImGuiTreeNodeFlags_DefaultOpen)) return;
            ImGui::Dummy({0,style.ItemSpacing.y*.4f});
        }
        for(const auto& field:dlss5_fields()) if(field.section==which && field.visible) row(field);
    };
    section(Dlss5Section::main,nullptr);
    section(Dlss5Section::scaling,"dlss5_section_scaling");
    section(Dlss5Section::advanced,"dlss5_section_advanced");
    if(!public_release() && ImGui::CollapsingHeader(hooks.tr("dlss5_section_debug"),ImGuiTreeNodeFlags_DefaultOpen)) {   // a release client has no debug folder to open
        ImGui::Dummy({0,style.ItemSpacing.y*.4f});
        if(ImGui::Button(hooks.tr("dlss5_open_debug"),{control,0}) && hooks.open_debug) hooks.open_debug();
        if(hooks.track) hooks.track("##d5-open-debug");
        ImGui::Dummy({0,style.ItemSpacing.y});
    }
    return changed;
}
}
