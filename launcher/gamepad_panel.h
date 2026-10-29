#pragma once
#include "binding_editor.h"
#include "gamepad_config.h"
#include "gamepad_target.h"
#include <functional>
#include <string_view>
struct ImFont;
namespace uc {
using PanelText=std::function<const char*(std::string_view)>;   // a translated launcher string
// What the host sets so the window looks like the rest of its UI: the serif font and size of its page titles, and how wide a combo or slider is (a fraction of the column).
// combo_unit above zero draws the combos in the launcher's look (launcher::combo, the host's UI scale); zero leaves ImGui's own combo.
struct GamepadPanelState { bool modal_open=false; ImFont* heading=nullptr; float heading_size=0; float control_width=.75f; float combo_unit=0; };
// The Controllers tab (the same code in the launcher and in the in-game menu): control type and gamepad model, the connected pad, and the "Gamepad" button that
// opens a window with the sticks, dead zones, gyro and the gamepad bindings (shown as the controller's own button pictures). `editor` is the gamepad table's
// editor (BindingTable::gamepad); the settings' bindings follow it. A pad press for a pending capture comes from outside, through editor.event().
// Returns true when `settings` changed. Call inside an ImGui window, with the host's style variables (frame padding, item spacing) already pushed.
bool gamepad_tab(GamepadPanelState&,GamepadSettings&,BindingEditor& editor,const PadStatus&,float unit,const PanelText&);
// The family whose button names and pictures the tab shows: the chosen model, or the connected pad's (the Xbox look without a pad).
PadFamily gamepad_panel_family(const GamepadSettings&,const PadStatus&);
}
