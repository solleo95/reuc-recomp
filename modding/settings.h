#pragma once
#include "manifest.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

// The current values of one mod's declared settings. Values read from the config are checked against the declarations:
// anything wrong (type, range, unknown choice, over-long text) falls back to the default, or is clamped for a range,
// with one warning line per value, so a hand-edited config never reaches a script as a bad value.
class ModSettings {
public:
    ModSettings(std::vector<SettingDecl> decls, const nlohmann::json& stored);

    const nlohmann::json& get(std::string_view key) const;   // null json for an unknown key
    // Validates like the loader: ints and numbers are clamped to min/max and snapped to step; false (value unchanged) for an
    // unknown key, a wrong type, a choice that is not listed or text over 1024 bytes.
    bool set(std::string_view key, const nlohmann::json& value);
    nlohmann::json to_json() const;   // every declared key with its current value, in declaration order
    const std::vector<std::string>& warnings() const { return warnings_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }
    std::vector<std::string> take_changed();   // keys whose value changed through set() since the last call (in the order they first changed); clears the list
    const std::vector<SettingDecl>& decls() const { return decls_; }

private:
    std::vector<SettingDecl> decls_;
    std::map<std::string, nlohmann::json, std::less<>> values_;
    std::vector<std::string> warnings_;
    std::vector<std::string> changed_;
    bool dirty_ = false;
};

}  // namespace uc::mods
