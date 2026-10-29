#include "wiimote_panel.h"
#include "panel_widgets.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <string>
namespace uc {
namespace {
using namespace panel_shared;
// What a remote can do in the game: the actions it has buttons for (the camera is the nunchuk stick, the swing and the shake are its motion).
constexpr std::array<Action,10> kActions{Action::interact,Action::fire,Action::weapon_previous,Action::weapon_next,Action::reload,Action::grenade,Action::knife,Action::pause,
    Action::confirm,Action::back};
void status_line(const WiimoteStatus& status,const PanelText& tr) {
    if(!status.error.empty()) {ImGui::TextWrapped("%s%s",tr("wii_unusable"),status.error.c_str());return;}
    if(!status.connected) {ImGui::TextWrapped("%s",tr("wii_none"));return;}
    ImGui::TextDisabled("%s",tr(status.has_nunchuk ? "wii_connected_nunchuk" : "wii_connected"));
}
bool modal(WiimotePanelState& state,WiimoteSettings& s,BindingEditor& editor,const WiimoteStatus& status,float unit,const PanelText& tr) {
    bool changed=false;
    const auto& io=ImGui::GetIO();
    const std::string title=std::string(tr("wiimote"))+"###wiimote-modal";
    const float width=state.control_width;
    if(state.modal_open && !ImGui::IsPopupOpen("###wiimote-modal")) ImGui::OpenPopup("###wiimote-modal");
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f});
    ImGui::SetNextWindowSize({std::min(io.DisplaySize.x*.94f,1100*unit),std::min(io.DisplaySize.y*.94f,1200*unit)},ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28*unit,22*unit});
    bool open=true;
    const bool visible=ImGui::BeginPopupModal(title.c_str(),&open,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if(!visible) {state.modal_open=false;return false;}
    if(!state.modal_open) {ImGui::CloseCurrentPopup();ImGui::EndPopup();return false;}   // the host closed it (the menu was closed)
    if(editor.capturing() && (ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsMouseClicked(ImGuiMouseButton_Left,false))) editor.cancel_capture();   // before the widgets: a click on a button starts a new capture
    else if(!editor.capturing() && ImGui::IsKeyPressed(ImGuiKey_Escape,false)) open=false;
    if(state.heading) ImGui::PushFont(state.heading,state.heading_size);
    ImGui::TextUnformatted(tr("wiimote"));
    if(state.heading) ImGui::PopFont();
    status_line(status,tr);
    ImGui::Dummy({0,8*unit}); ImGui::Separator(); ImGui::Dummy({0,12*unit});
    const float footer=(ImGui::GetFrameHeightWithSpacing()*(1+(editor.capturing() ? 1 : 0)+(editor.error() ? 1 : 0)))+24*unit;
    ImGui::BeginChild("wii-body",{0,std::max(120*unit,ImGui::GetContentRegionAvail().y-footer)});
    changed|=toggle(tr("wii_enabled"),s.enabled,unit); gap(unit);
    ImGui::TextUnformatted(tr("wii_smoothing")); changed|=percent_slider("##wii-smoothing",s.smoothing,0,90,width); gap(unit);
    ImGui::TextUnformatted(tr("wii_swing")); changed|=float_slider("##wii-swing",s.swing_threshold,.5f,4,"%.1f",width); gap(unit);
    ImGui::TextUnformatted(tr("wii_stick_dead_zone")); changed|=percent_slider("##wii-dead-zone",s.stick_dead_zone,0,50,width); gap(unit);
    ImGui::Separator(); gap(unit);
    ImGui::TextUnformatted(tr("bindings")); gap(unit);
    bindings_rows(editor,kActions,PadFamily::generic,unit,tr);
    ImGui::EndChild();
    ImGui::Separator();
    if(editor.capturing()) ImGui::TextWrapped("%s",tr("wii_capture"));
    if(editor.error()) ImGui::TextWrapped("%s: %s",tr("rejected"),editor.error()->c_str());
    if(ImGui::Button(tr("defaults"))) {s=WiimoteSettings{};editor.restore_defaults();changed=true;}
    ImGui::SameLine();
    if(ImGui::Button(tr("close"))) open=false;
    if(!open) {editor.cancel_capture();state.modal_open=false;ImGui::CloseCurrentPopup();}
    ImGui::EndPopup();
    return changed;
}
}
bool wiimote_tab(WiimotePanelState& state,WiimoteSettings& s,BindingEditor& editor,const WiimoteStatus& status,float unit,const PanelText& tr) {
    bool changed=false;
    ImGui::Separator(); gap(unit);
    status_line(status,tr);
    gap(unit);
    if(ImGui::Button(tr("wiimote"),{ImGui::GetContentRegionAvail().x*.4f,0})) state.modal_open=true;
    changed|=modal(state,s,editor,status,unit,tr);
    if(!editor.capturing() && s.bindings!=editor.bindings()) {s.bindings=editor.bindings();changed=true;}   // the editor holds the table the player is editing
    return changed;
}
}
