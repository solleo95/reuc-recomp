#pragma once
#include "registry.h"
#include "settings.h"
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace uc::mods {

// The mod API the game offers: what the registry compares each mod's "api" with (the game and the launchers must agree).
inline constexpr ApiVersion kHostModApi{1, 1};

// section[id]["settings"] = settings[id] for every entry (the objects are created when missing). The one place that decides how a mod's settings
// sit in the config's "mods" section, for the in-game menu and the launchers.
nlohmann::ordered_json merge_mod_settings(nlohmann::ordered_json section, const std::map<std::string, nlohmann::ordered_json>& settings);

// The mod manager without a running game, for the launchers: the registry of the mods folder plus the declared settings of each mod, edited
// and written to the config's "mods" section the way the game would. No mod code runs here.
class StandaloneMods {
public:
    // game_id empty: the launcher does not know the disc, so a mod's "made for another game version" check is skipped (the game still makes it).
    StandaloneMods(std::filesystem::path mods_dir, nlohmann::ordered_json section, std::string game_id = {});

    ModRegistry& registry() { return registry_; }
    // The values of a mod's declared settings, built on first use from the stored section; null for an unknown mod or one that declares none.
    ModSettings* settings(std::string_view id);
    bool settings_dirty() const;   // a ModSettings was changed since the last section()
    void mark_dirty() { dirty_ = true; }   // the registry was changed through registry()
    bool take_dirty();                     // true once after mark_dirty()
    // The section to store: the registry's choices plus the settings of every mod that was edited; the stored settings of other mods
    // (including mods that are not installed) are left as they were. Clears the settings' dirty flags.
    nlohmann::ordered_json section();
    std::string dll_hash(std::string_view id);   // sha256 of the mod's native DLL, cached; empty when it has none

private:
    nlohmann::ordered_json stored_;   // the section as given: the stored settings of every mod
    ModRegistry registry_;
    std::map<std::string, ModSettings, std::less<>> settings_;
    std::map<std::string, nlohmann::ordered_json> edited_;   // id -> to_json() of each mod that changed
    std::map<std::string, std::string, std::less<>> hashes_;
    bool dirty_ = false;
};

}  // namespace uc::mods
