#pragma once
// What the in-game menu (game/settings_menu) needs from the mod platform. No ImGui types here: the implementation is modding/menu_ui.
#include <string>
#include <vector>

namespace uc::mods {

struct ModTab {
    std::string id, title;
};

class ModMenu {
public:
    virtual ~ModMenu() = default;
    virtual std::string manager_title() = 0;   // the manager tab's label in the menu's language
    // The "Mods" tab: enable/disable, load order, status, native-code switch, conflicts. Called inside the menu's tab bar item.
    virtual void manager_tab(float unit) = 0;
    // One tab per running mod that has settings or a drawing function, in load order.
    virtual std::vector<ModTab> tabs() = 0;
    // The content of that mod's tab: its declared settings, then whatever the mod draws.
    virtual void draw_tab(const std::string& id, float unit) = 0;
    // Main loop, outside rendering: saves what the menu changed and tells the mods about changed settings.
    virtual void commit() = 0;
};

}  // namespace uc::mods
