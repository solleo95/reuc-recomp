#include "standalone.h"
#include "mod_paths.h"
#include "runtime_native.h"

namespace uc::mods {

nlohmann::ordered_json merge_mod_settings(nlohmann::ordered_json section, const std::map<std::string, nlohmann::ordered_json>& settings) {
    if (!section.is_object()) section = nlohmann::ordered_json::object();
    for (const auto& [id, values] : settings) {
        if (!section.contains(id) || !section[id].is_object()) section[id] = nlohmann::ordered_json::object();
        section[id]["settings"] = values;
    }
    return section;
}

StandaloneMods::StandaloneMods(std::filesystem::path mods_dir, nlohmann::ordered_json section, std::string game_id)
    : stored_(section.is_object() ? std::move(section) : nlohmann::ordered_json::object()),
      registry_(ModRegistry::discover(mods_dir, stored_, kHostModApi, game_id)) {}

ModSettings* StandaloneMods::settings(std::string_view id) {
    if (const auto at = settings_.find(id); at != settings_.end()) return &at->second;
    for (const auto& entry : registry_.entries()) {
        if (entry.manifest.id != id || entry.manifest.settings.empty()) continue;
        const auto key = std::string(id);
        const nlohmann::json stored = stored_.contains(key) && stored_[key].is_object() && stored_[key].contains("settings") ? nlohmann::json(stored_[key]["settings"]) : nlohmann::json::object();
        return &settings_.emplace(key, ModSettings(entry.manifest.settings, stored)).first->second;
    }
    return nullptr;
}

bool StandaloneMods::settings_dirty() const {
    for (const auto& [id, values] : settings_) if (values.dirty()) return true;
    return false;
}

bool StandaloneMods::take_dirty() {
    const bool was = dirty_;
    dirty_ = false;
    return was;
}

nlohmann::ordered_json StandaloneMods::section() {
    for (auto& [id, values] : settings_) {
        if (!values.dirty()) continue;
        edited_[id] = values.to_json();
        values.clear_dirty();
        values.take_changed();
    }
    // Settings written earlier stay in the section (the registry's own copy was taken when it was discovered).
    return merge_mod_settings(registry_.to_config(), edited_);
}

std::string StandaloneMods::dll_hash(std::string_view id) {
    if (const auto at = hashes_.find(id); at != hashes_.end()) return at->second;
    std::string hash;
    for (const auto& entry : registry_.entries()) {
        if (entry.manifest.id != id || entry.manifest.native.empty()) continue;
        if (const auto dll = confine(entry.root, entry.manifest.native)) hash = sha256_file_hex(*dll);
    }
    return hashes_[std::string(id)] = hash;
}

}  // namespace uc::mods
