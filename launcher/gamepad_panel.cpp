#include "gamepad_panel.h"
#include "panel_widgets.h"
#include "input_prompts.h"
#include "pad_glyphs.h"
#include "graphite.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace uc {
namespace {
constexpr std::array<Action,17> kActions{Action::interact,Action::fire,Action::weapon_previous,Action::weapon_next,Action::reload,Action::grenade,Action::knife,Action::pause,
    Action::confirm,Action::back,Action::screenshot,Action::qte_shake,Action::recenter,Action::camera_up,Action::camera_down,Action::camera_left,Action::camera_right};
// Inputs that can hold the gyro on: a trigger or a shoulder or stick button.
constexpr std::array<PhysicalInput,6> kHoldChoices{{{PhysicalKind::pad_axis,9},{PhysicalKind::pad_axis,11},{PhysicalKind::pad_button,9},{PhysicalKind::pad_button,10},{PhysicalKind::pad_button,7},{PhysicalKind::pad_button,8}}};
}
namespace panel_shared {   // the widgets the Wii Remote window uses as well (panel_widgets.h)
// The same rhythm as the host's own pages: a small caption above each control, a gap after it.
void gap(float unit) { ImGui::Dummy({0,8*unit}); }
bool combo(const char* id,const std::string& shown,const std::vector<std::string>& items,int& index,float width,float look_unit) {
    bool changed=false;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*width);
    if(look_unit>0 ? launcher::combo(id,shown.c_str(),look_unit) : ImGui::BeginCombo(id,shown.c_str())) {
        for(int i=0;i<int(items.size());++i) if(ImGui::Selectable(items[size_t(i)].c_str(),i==index)) {index=i;changed=true;}
        ImGui::EndCombo();
    }
    return changed;
}
bool percent_slider(const char* id,float& fraction,float low_percent,float high_percent,float width) {
    float value=fraction*100;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*width);
    const bool changed=ImGui::SliderFloat(id,&value,low_percent,high_percent,"%.0f%%",ImGuiSliderFlags_AlwaysClamp);
    if(changed) fraction=value/100;
    return changed;
}
bool float_slider(const char* id,float& value,float low,float high,const char* format,float width,bool logarithmic) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*width);
    return ImGui::SliderFloat(id,&value,low,high,format,ImGuiSliderFlags_AlwaysClamp|(logarithmic ? ImGuiSliderFlags_Logarithmic : 0));
}
bool toggle(const char* label,bool& value,float unit) {   // the launcher's checkbox look
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{6*unit,6*unit});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,3*unit);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,{12*unit,0});
    const bool changed=ImGui::Checkbox(label,&value);
    ImGui::PopStyleVar(3);
    return changed;
}
std::string input_name(PhysicalInput input,PadFamily family) { return pad_glyph_label(input,family); }
std::string binding_name(const Binding& binding,PadFamily family,const PanelText& tr) {
    if(binding.kind==BindingKind::interact_fire) return tr("interact_fire");
    if(binding.kind==BindingKind::interact_motion) return tr("interact_motion");
    if(binding.kind==BindingKind::qte_motion) return tr("rapid");
    std::string text; for(auto modifier:binding.modifiers) text+=input_name(modifier,family)+" + ";
    return text+input_name(binding.input,family);
}
// One action's bindings as the controller's own pictures: "LT + A / B", the special kinds as words.
struct Piece { std::string text; std::optional<PhysicalInput> input; };
std::vector<Piece> pieces(const std::vector<Binding>& entries,const PanelText& tr) {
    std::vector<Piece> result;
    for(size_t i=0;i<entries.size();++i) {
        const auto& binding=entries[i];
        if(i) result.push_back({"/",{}});
        if(binding.kind==BindingKind::physical) {
            for(auto modifier:binding.modifiers) {result.push_back({{},modifier});result.push_back({"+",{}});}
            result.push_back({{},binding.input});
        } else result.push_back({binding.kind==BindingKind::interact_fire ? tr("interact_fire") : binding.kind==BindingKind::interact_motion ? tr("interact_motion") : tr("rapid"),{}});
    }
    return result;
}
void draw_pieces(ImDrawList& list,ImVec2 low,ImVec2 high,const std::vector<Piece>& parts,PadFamily family,const char* empty) {
    auto* font=ImGui::GetFont();
    const float font_size=ImGui::GetFontSize(),area=high.y-low.y;
    float glyph=area*.78f;
    auto width_of=[&](float height) {
        float total=0;
        for(const auto& part:parts) total+=(part.input ? pad_glyph_aspect(*part.input)*height : font->CalcTextSizeA(font_size,FLT_MAX,0,part.text.c_str()).x)+font_size*.35f;
        return total-font_size*.35f;
    };
    if(parts.empty()) {
        const auto size=font->CalcTextSizeA(font_size,FLT_MAX,0,empty);
        list.AddText({(low.x+high.x-size.x)/2,(low.y+high.y-size.y)/2},ImGui::GetColorU32(ImGuiCol_TextDisabled),empty);
        return;
    }
    const float room=high.x-low.x-8;
    if(width_of(glyph)>room) glyph=std::max(area*.4f,glyph*room/width_of(glyph));   // many bindings: smaller pictures rather than overflow
    float x=(low.x+high.x-width_of(glyph))/2;
    list.PushClipRect(low,high,true);
    for(const auto& part:parts) {
        if(part.input) {
            const float w=pad_glyph_aspect(*part.input)*glyph;
            draw_pad_glyph(list,{x,(low.y+high.y-glyph)/2},{w,glyph},*part.input,family);
            x+=w+font_size*.35f;
        } else {
            const auto size=font->CalcTextSizeA(font_size,FLT_MAX,0,part.text.c_str());
            list.AddText({x,(low.y+high.y-size.y)/2},ImGui::GetColorU32(ImGuiCol_Text),part.text.c_str());
            x+=size.x+font_size*.35f;
        }
    }
    list.PopClipRect();
}
// A role picked for one stick that the other stick already has: the two swap, so the roles never collide.
bool pick_role(StickRole& changed,StickRole& other,StickRole picked) {
    if(picked==changed) return false;
    if(picked!=StickRole::none && other==picked) other=changed;
    changed=picked;
    return true;
}
bool role_combo(const char* id,StickRole& role,StickRole& other,const PanelText& tr,float width,float look_unit) {
    const std::vector<std::string> items{tr("stick_none"),tr("stick_left"),tr("stick_right")};
    int index=int(role);
    if(!combo(id,items[size_t(index)],items,index,width,look_unit)) return false;
    return pick_role(role,other,StickRole(index));
}
void status_line(const PadStatus& status,const PanelText& tr) {
    if(!status.connected) {ImGui::TextDisabled("%s",tr("pad_none"));return;}
    ImGui::TextDisabled("%s (%s)",status.name.c_str(),tr(status.has_gyro ? "pad_gyro_yes" : "pad_gyro_no"));
}
// The buttons stay live while a capture waits: a click on another action switches the capture to it.
void bindings_rows(BindingEditor& editor,std::span<const Action> actions,PadFamily family,float unit,const PanelText& tr) {
    const float spacing=ImGui::GetStyle().ItemSpacing.x;
    const float plus=46*unit,clear=ImGui::CalcTextSize(tr("clear")).x+2*ImGui::GetStyle().FramePadding.x+16*unit;
    for(Action action:actions) {
        const size_t i=size_t(action);
        ImGui::PushID(int(i));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{ImGui::GetStyle().FramePadding.x,ImGui::GetStyle().FramePadding.y+7*unit});   // taller rows: the pictures need the room
        const float row=ImGui::GetCursorPosX(),total=ImGui::GetContentRegionAvail().x,label=total*.34f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr(action_names[i]));
        ImGui::SameLine(row+label);
        const auto& entries=editor.bindings()[i];
        const float width=std::max(60*unit,total-label-plus-clear-2*spacing);
        const bool waiting=editor.capturing()==action;
        if(waiting) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if(ImGui::Button("##bind",{width,0})) editor.begin_capture(action,true);
        if(waiting) ImGui::PopStyleColor();
        const ImVec2 low=ImGui::GetItemRectMin(),high=ImGui::GetItemRectMax();
        draw_pieces(*ImGui::GetWindowDrawList(),low,high,waiting ? std::vector<Piece>{} : pieces(entries,tr),family,waiting ? "…" : tr("unbound"));
        if(ImGui::IsItemHovered()) {
            std::string names; for(const auto& entry:entries) {if(!names.empty())names+=", ";names+=binding_name(entry,family,tr);}
            launcher::tooltip(names.empty() ? tr("unbound") : names.c_str());
        }
        if(ImGui::BeginPopupContextItem("binding-menu")) {
            for(size_t j=0;j<entries.size();++j) {
                ImGui::PushID(int(j));const auto text=std::string(tr("remove"))+": "+binding_name(entries[j],family,tr);
                const bool remove=ImGui::MenuItem(text.c_str());ImGui::PopID();
                if(remove) {editor.remove(action,j);break;}
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("+",{plus,0})) editor.begin_capture(action);
        if(ImGui::IsItemHovered()) launcher::tooltip(tr("add"));
        ImGui::SameLine();
        if(ImGui::Button(tr("clear"),{clear,0})) editor.clear(action);
        ImGui::PopStyleVar();
        ImGui::PopID();
    }
}
}
namespace {
using namespace panel_shared;
bool modal(GamepadPanelState& state,GamepadSettings& s,BindingEditor& editor,const PadStatus& status,float unit,const PanelText& tr) {
    bool changed=false;
    const auto& io=ImGui::GetIO();
    const std::string title=std::string(tr("gamepad"))+"###gamepad-modal";
    const float width=state.control_width;
    if(state.modal_open && !ImGui::IsPopupOpen("###gamepad-modal")) ImGui::OpenPopup("###gamepad-modal");
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f});
    ImGui::SetNextWindowSize({std::min(io.DisplaySize.x*.94f,1100*unit),std::min(io.DisplaySize.y*.94f,1400*unit)},ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28*unit,22*unit});
    bool open=true;
    const bool visible=ImGui::BeginPopupModal(title.c_str(),&open,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if(!visible) {state.modal_open=false;return false;}
    if(!state.modal_open) {ImGui::CloseCurrentPopup();ImGui::EndPopup();return false;}   // the host closed it (the menu was closed)
    if(editor.capturing() && (ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsMouseClicked(ImGuiMouseButton_Left,false))) editor.cancel_capture();   // before the widgets: a click on a button starts a new capture
    else if(!editor.capturing() && ImGui::IsKeyPressed(ImGuiKey_Escape,false)) open=false;
    const PadFamily family=gamepad_panel_family(s,status);
    // The heading and the hint line of the host's own pages.
    if(state.heading) ImGui::PushFont(state.heading,state.heading_size);
    ImGui::TextUnformatted(tr("gamepad"));
    if(state.heading) ImGui::PopFont();
    status_line(status,tr);
    ImGui::Dummy({0,8*unit}); ImGui::Separator(); ImGui::Dummy({0,12*unit});
    // Everything scrolls together in one body; the capture hint, the error and the buttons stay below it.
    const float footer=(ImGui::GetFrameHeightWithSpacing()*(1+(editor.capturing() ? 1 : 0)+(editor.error() ? 1 : 0)))+24*unit;
    ImGui::BeginChild("pad-body",{0,std::max(120*unit,ImGui::GetContentRegionAvail().y-footer)});
    ImGui::TextUnformatted(tr("stick_aim")); changed|=role_combo("##aim-stick",s.aim_stick,s.camera_stick,tr,width,state.combo_unit); gap(unit);
    ImGui::TextUnformatted(tr("stick_camera")); changed|=role_combo("##camera-stick",s.camera_stick,s.aim_stick,tr,width,state.combo_unit); gap(unit);
    ImGui::TextUnformatted(tr("dead_zone")); changed|=percent_slider("##dead-zone",s.aim.dead_zone,0,50,width); gap(unit);
    ImGui::TextUnformatted(tr("aim_speed")); changed|=float_slider("##aim-speed",s.aim.speed,.2f,4,"%.2f",width); gap(unit);
    ImGui::TextUnformatted(tr("camera_dead_zone")); changed|=percent_slider("##camera-dead-zone",s.camera_dead_zone,0,50,width); gap(unit);
    ImGui::Separator(); gap(unit);
    ImGui::TextUnformatted(tr("gyro_mode"));
    {
        const std::vector<std::string> modes{tr("off"),tr("gyro_always"),tr("gyro_hold")};
        int index=int(s.gyro.mode);
        if(combo("##gyro-mode",modes[size_t(index)],modes,index,width,state.combo_unit)) {s.gyro.mode=GyroMode(index);changed=true;}
    }
    gap(unit);
    if(s.gyro.mode==GyroMode::hold) {
        ImGui::TextUnformatted(tr("gyro_hold_button"));
        std::vector<std::string> names; int index=0;
        for(size_t i=0;i<kHoldChoices.size();++i) {names.push_back(input_name(kHoldChoices[i],family)); if(kHoldChoices[i]==s.gyro_hold) index=int(i);}
        if(combo("##gyro-hold",names[size_t(index)],names,index,width,state.combo_unit)) {s.gyro_hold=kHoldChoices[size_t(index)];changed=true;}
        gap(unit);
    }
    if(s.gyro.mode!=GyroMode::off) {
        ImGui::TextUnformatted(tr("gyro_sens_x")); changed|=float_slider("##gyro-x",s.gyro.sensitivity_x,.1f,10,"%.2f",width,true); gap(unit);
        ImGui::TextUnformatted(tr("gyro_sens_y")); changed|=float_slider("##gyro-y",s.gyro.sensitivity_y,.1f,10,"%.2f",width,true); gap(unit);
        changed|=toggle(tr("gyro_invert_x"),s.gyro.invert_x,unit); gap(unit);
        changed|=toggle(tr("gyro_invert_y"),s.gyro.invert_y,unit); gap(unit);
    }
    ImGui::Separator(); gap(unit);
    ImGui::TextUnformatted(tr("bindings")); gap(unit);
    bindings_rows(editor,kActions,family,unit,tr);
    ImGui::EndChild();
    ImGui::Separator();
    if(editor.capturing()) ImGui::TextWrapped("%s",tr("pad_capture"));
    if(editor.error()) ImGui::TextWrapped("%s: %s",tr("rejected"),editor.error()->c_str());
    if(ImGui::Button(tr("defaults"))) {
        const auto type=s.type; const auto model=s.model;   // the control type and model are the player's choices about the device, not tuning
        s=GamepadSettings{}; s.type=type; s.model=model; editor.restore_defaults(); changed=true;
    }
    ImGui::SameLine();
    if(ImGui::Button(tr("close"))) open=false;
    if(!open) {editor.cancel_capture();state.modal_open=false;ImGui::CloseCurrentPopup();}
    ImGui::EndPopup();
    return changed;
}
}
PadFamily gamepad_panel_family(const GamepadSettings& settings,const PadStatus& status) {
    switch(settings.model) {
    case PadModel::xbox: return PadFamily::xbox;
    case PadModel::playstation: return PadFamily::playstation;
    case PadModel::nintendo: return PadFamily::nintendo;
    case PadModel::generic: return PadFamily::generic;
    case PadModel::automatic: break;
    }
    return status.connected ? status.family : PadFamily::xbox;
}
bool gamepad_tab(GamepadPanelState& state,GamepadSettings& s,BindingEditor& editor,const PadStatus& status,float unit,const PanelText& tr) {
    bool changed=false;
    const float width=std::max(state.control_width,.9f);   // these two list long sentences in some languages
    ImGui::TextUnformatted(tr("control_type"));
    {
        const std::vector<std::string> types{tr("control_auto"),tr("control_keyboard_mouse"),tr("gamepad")};
        int index=int(s.type);
        if(combo("##control-type",types[size_t(index)],types,index,width,state.combo_unit)) {s.type=ControlType(index);changed=true;}
    }
    gap(unit);
    if(s.type!=ControlType::keyboard_mouse) {
        ImGui::TextUnformatted(tr("pad_model"));
        const std::vector<std::string> models{tr("model_auto"),"Xbox","PlayStation","Nintendo",tr("model_generic")};
        int index=int(s.model);
        if(combo("##pad-model",models[size_t(index)],models,index,width,state.combo_unit)) {s.model=PadModel(index);changed=true;}
        gap(unit);
    }
    status_line(status,tr);
    gap(unit);
    if(ImGui::Button(tr("gamepad"),{ImGui::GetContentRegionAvail().x*.4f,0})) state.modal_open=true;
    changed|=modal(state,s,editor,status,unit,tr);
    if(!editor.capturing() && s.bindings!=editor.bindings()) {s.bindings=editor.bindings();changed=true;}   // the editor holds the table the player is editing
    return changed;
}
}
