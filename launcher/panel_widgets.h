#pragma once
#include "binding_editor.h"
#include "gamepad_panel.h"
#include <span>
#include <string>
#include <vector>
namespace uc::panel_shared {
// The small widgets of the controller windows, defined in gamepad_panel.cpp and used by the Wii Remote window too: the host's rhythm of caption and gap, combo, sliders,
// the host's checkbox, and the binding rows (an action's name, its bindings as the controller's own pictures, "+" to add one, Clear).
void gap(float unit);
bool combo(const char* id,const std::string& shown,const std::vector<std::string>& items,int& index,float width);
bool percent_slider(const char* id,float& fraction,float low_percent,float high_percent,float width);
bool float_slider(const char* id,float& value,float low,float high,const char* format,float width,bool logarithmic=false);
bool toggle(const char* label,bool& value,float unit);
void bindings_rows(BindingEditor& editor,std::span<const Action> actions,PadFamily family,float unit,const PanelText& tr);
}
