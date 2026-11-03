#include "settings_pages.h"
#include <algorithm>
#include <string_view>
namespace uc::deck {
using namespace settings;
std::vector<Page> tab_pages(Tab tab) {
    const std::vector<Page> wanted = tab == Tab::game ? std::vector<Page>{Page::game}
        : tab == Tab::graphics ? std::vector<Page>{Page::graphics, Page::effects}
        : tab == Tab::controls ? std::vector<Page>{Page::gamepad, Page::buttons}
        : std::vector<Page>{};   // the Mods tab is the mod manager's, not a registry page
    std::vector<Page> result;
    const auto listed = pages_for(deck_launcher);
    for (const auto page : wanted)
        if (std::find(listed.begin(), listed.end(), page) != listed.end()) result.push_back(page);
    return result;
}
namespace {
// The Controls page is one list; its three groups get a title at their first visible row.
const char* section_of(std::string_view id) {
    if (id == "gyro_mode") return "deck_section_gyro";
    if (id == "aim_speed") return "deck_section_sticks";
    if (id == "trackpad_sensitivity") return "deck_section_touch";
    return nullptr;
}
bool follow_editor(Options& options, const BindingEditor& editor) {   // the table follows its editor
    if (!editor.capturing() && editor.bindings() != options.gamepad.bindings) { options.gamepad.bindings = editor.bindings(); return true; }
    return false;
}
bool draw_bindings(Options& options, RowCtx& rows, BindingEditor& editor, PadFamily family) {
    for (const Action action : gamepad_panel_actions()) binding_row(rows, editor, action, family);
    if (button_row(rows, "defaults", rows.text("defaults").c_str())) editor.restore_defaults();
    return follow_editor(options, editor);
}
}
bool draw_tab(Options& options, Tab tab, RowCtx& rows, BindingEditor& pad_editor, PadFamily family) {
    bool changed = false, first = true;
    for (const auto page : tab_pages(tab)) {
        if (!first) caption(rows, rows.text(page_label_key(page)).c_str());   // the first page is what the tab's own title already says
        first = false;
        const char* pending = nullptr;
        for (const Item* item : items_for(deck_launcher, page)) {
            if (const char* section = section_of(item->id)) pending = section;
            const State state = item->state ? item->state(options, rows.env) : State{};
            if (!state.visible) continue;
            if (item->kind == Kind::custom) {
                if (std::string_view(item->id) == "pad_bindings") changed |= draw_bindings(options, rows, pad_editor, family);
                else if (std::string_view(item->id) == "gyro_hold_button") {   // "while a button is held": press the button that holds it
                    if (pending) { caption(rows, rows.text(pending).c_str()); pending = nullptr; }
                    hold_row(rows, rows.text(item->label_key).c_str(), options.gamepad.gyro_hold, family);
                } else if (std::string_view(item->id) == "gyro_recenter") {   // the same row as on the Buttons tab
                    binding_row(rows, pad_editor, Action::recenter, family);
                    changed |= follow_editor(options, pad_editor);
                }
                continue;
            }
            if (pending) { caption(rows, rows.text(pending).c_str()); pending = nullptr; }
            changed |= registry_row(rows, *item, options);
        }
    }
    return changed;
}
}
