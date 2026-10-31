#pragma once
#include "binding_editor.h"
#include "focus.h"
#include "gamepad_panel.h"
#include "imgui.h"
#include "settings_registry.h"
#include <functional>
#include <string>
namespace uc::deck {
// A finger on a row is a tap or the drag of the list: the press position and whether it turned into a drag.
struct TouchDrag { float press_y = 0; bool dragged = false; };
// Everything the rows of one screen share. Rows are drawn like the in-game menu's (wide, rounded, the focused one lit with the accent colour) and registered in the
// FocusList in drawing order, so the D-pad drives them directly: left/right change the focused row's value, A acts on it. Taps and finger drags do the same.
struct RowCtx {
    FocusList* focus = nullptr;
    float unit = 1;
    ImFont* serif = nullptr;
    settings::Text text;                                       // a translated string by key
    PanelText panel_text;                                      // the same for the shared controller widgets
    settings::Env env;
    std::function<void(const settings::Item&)> open_list;      // A or a tap on a choice: the host shows its list
    TouchDrag* touch = nullptr;
    bool* hold_capture = nullptr;                              // the gyro's hold-button row waits for a pad press (the host's capture sink answers it)
    int* sub = nullptr;                                        // which part of a binding row the D-pad has selected: 0 the row, 1 the plus, 2 the cross
};
// A registry item as a row: a switch, a choice (value and chevron), a slider (bar and value), a colour. Returns true when `options` changed.
bool registry_row(RowCtx&, const settings::Item&, Options&);
// A full-width button. Returns true when activated (A, or a tap). `primary`: the accent colour (Play).
bool button_row(RowCtx&, const char* id, const char* label, bool enabled = true, bool primary = false);
// A choice list's or the file browser's entry. Returns true when chosen. `mark`: the current value (a dot) or a folder (a chevron).
enum class EntryMark { none, current, folder };
bool entry_row(RowCtx&, const char* id, const std::string& label, EntryMark mark = EntryMark::none);
// One action of the gamepad table: its buttons as pictures. A replaces them (the next pad button is bound), X adds one, Y clears; taps do the same.
void binding_row(RowCtx&, BindingEditor&, Action, PadFamily);
// The button that holds the gyro on ("while a button is held"): its picture, or the prompt while the row waits for a press. A or a tap starts the wait.
void hold_row(RowCtx&, const char* label, PhysicalInput current, PadFamily);
// One mod of the Mods tab: a switch, the name with its state, description and problems, and a gear, "load earlier" and "load later" button. Like the binding row it has
// parts for the D-pad (the row itself, then the gear when the mod has settings, up, down): left/right move between them, A acts on the selected one, a tap on the row's
// body switches the mod. The result says which part was activated; the host changes the mod.
struct ModRowView {
    std::string title, status, description, problems, native;   // each shown only when not empty, wrapped
    bool on = false, can_toggle = true, has_settings = false;
};
struct ModRowResult { bool toggle = false, settings = false, up = false, down = false; };
ModRowResult mod_row(RowCtx&, const char* id, const ModRowView&);
// A value row of a mod's declared setting: a switch, a slider (bar and value) or a text value with a chevron. The result: left/right presses (delta), A or a tap
// (activated), and where a finger or the pointer set the bar (set_t, 0..1; -1 when it did not).
struct ValueView { enum class Kind { toggle, slider, text } kind = Kind::toggle; bool on = false; float t = 0; std::string shown; };
struct ValueResult { int delta = 0; bool activated = false; float set_t = -1; };
ValueResult value_row(RowCtx&, const char* id, const std::string& label, const ValueView&);
void caption(RowCtx&, const char* label);                   // a section title (not focusable)
void note(RowCtx&, const char* text, ImU32 color);          // a wrapped line of text (not focusable)
}
