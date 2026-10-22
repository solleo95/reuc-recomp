#pragma once
#include "manifest.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

enum class ModState { disabled, enabled, error, incompatible, missing_dependency, dependency_disabled };

struct ModEntry {
    ModManifest manifest;   // for a broken mod only id/name (the folder name) are set
    std::filesystem::path root;
    ModState state = ModState::disabled;
    std::vector<std::string> messages;
    std::string folder_name;
};

struct Conflict {
    std::string path;                  // lower-case, e.g. "files/data/x.bin" or "textures/tex1_...dds"
    std::vector<std::string> mod_ids;  // active mods that provide it, in load order (the last one wins)
};

// Mods found in the mods folder and what the user chose for them. The config "mods" section holds:
// "enabled" (ids the user turned on), "order" (all ids in load order), "allow_native", "native_hash" {id: sha256 hex},
// and one object per mod id (its own settings), which this class leaves untouched.
class ModRegistry {
public:
    static ModRegistry discover(const std::filesystem::path& mods_dir, const nlohmann::ordered_json& mods_section, ApiVersion host_api,
                                std::string_view game_version);

    const std::vector<ModEntry>& entries() const { return entries_; }   // load order
    std::vector<const ModEntry*> active() const;                        // enabled mods, dependencies before dependents
    bool wanted(std::string_view id) const { return wanted_.count(std::string(id)) > 0; }   // the user turned it on (it may still not work: see state)
    bool set_enabled(std::string_view id, bool on);                     // false for an unknown id
    bool move(std::string_view id, int delta);                          // false at either end or for an unknown id
    std::vector<Conflict> conflicts() const;                            // files/ and textures/ paths provided by two or more active mods
    nlohmann::ordered_json to_config() const;

    bool allow_native() const { return allow_native_; }
    void set_allow_native(bool on) { allow_native_ = on; }
    std::string native_hash(std::string_view id) const;                 // confirmed DLL hash, empty when never confirmed
    void set_native_hash(std::string_view id, std::string hash);

private:
    void evaluate();   // recomputes every state from the manifests, the wanted set and the host
    nlohmann::ordered_json section_;
    ApiVersion host_api_;
    std::string game_version_;
    std::vector<ModEntry> entries_;
    std::vector<bool> broken_;   // parse error or duplicate id: never evaluated further
    std::set<std::string> wanted_;
    bool allow_native_ = false;
};

// Dotted numeric versions ("1.10" > "1.2"); parts that are not numbers count as 0. Returns <0, 0, >0.
int compare_versions(std::string_view a, std::string_view b);

}  // namespace uc::mods
