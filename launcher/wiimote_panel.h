#pragma once
#include "binding_editor.h"
#include "gamepad_panel.h"
#include "wiimote_config.h"
#include "wiimote_target.h"
struct ImFont;
namespace uc {
// What the host sets so the window looks like the rest of its UI (the same as the gamepad window's).
struct WiimotePanelState { bool modal_open=false; ImFont* heading=nullptr; float heading_size=0; float control_width=.75f; };
// The Wii Remote part of the Controllers tab (the same code in the launcher and in the in-game menu): the connected remote, and a "Wii Remote" button that opens a window with
// the switch, the pointer smoothing, the swing strength, the nunchuk stick's dead zone and the remote's bindings shown as its own button pictures. `editor` is the remote table's
// editor (BindingTable::wiimote); the settings' bindings follow it. A press on the remote for a pending capture comes from outside, through editor.event().
// Returns true when `settings` changed. Call inside an ImGui window, with the host's style variables already pushed.
bool wiimote_tab(WiimotePanelState&,WiimoteSettings&,BindingEditor& editor,const WiimoteStatus&,float unit,const PanelText&);
}
