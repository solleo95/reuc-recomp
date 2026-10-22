#pragma once
// What the in-game menu (game/settings_menu) needs from the mod platform. No ImGui types here: the implementation is modding/menu_ui.
#include <string>
#include <vector>

namespace uc::mods {

class ModMenu {
public:
    virtual ~ModMenu() = default;
    virtual std::string manager_title() = 0;   // the manager tab's label in the menu's language
    // The "Mods" tab: enable/disable, load order, status, native-code switch, conflicts, and per mod a gear button that opens its settings in a modal window. Called inside the menu's tab bar item.
    virtual void manager_tab(float unit) = 0;
    // Main loop, outside rendering: saves what the menu changed and tells the mods about changed settings.
    virtual void commit() = 0;
};

}  // namespace uc::mods
