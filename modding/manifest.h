#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

enum class SettingType { boolean, integer, number, choice, text };

struct SettingDecl {
    std::string key;
    SettingType type = SettingType::boolean;
    nlohmann::json default_value;
    std::optional<double> min, max, step;
    std::vector<std::string> choices;
    std::map<std::string, std::string> label, tooltip;   // language code -> text; label has "en"
    bool restart_required = false;
};

struct ModDependency { std::string id; std::string min_version; };
struct ApiVersion { int major = 0, minor = 0; };

// Loop points of a replaced music stream (audio/<stem>.ogg|.wav), in samples of the mod's own file; end is exclusive.
struct AudioLoop { uint32_t loop_start = 0, loop_end = 0; };

struct ModManifest {
    std::string id, name, version, author, description;
    ApiVersion api;   // minimum API version; the same major version is required
    std::vector<std::string> game;
    std::vector<ModDependency> depends;
    bool touches_gameplay = false;
    std::string script, native;
    std::vector<SettingDecl> settings;
    std::map<std::string, std::string> names;   // localized display names
    std::map<std::string, AudioLoop> audio;     // "audio": loop points by stream stem (lower-case)
};

struct ManifestResult {
    std::optional<ModManifest> manifest;   // empty when errors is not
    std::vector<std::string> errors;
};

// Parses and validates mod.json text. Collects every problem; never throws.
ManifestResult parse_manifest(std::string_view json_text);

// Lexical check for a path written in a manifest: relative, '/' separators, no "..", no drive or stream colon.
bool is_safe_relative_path(std::string_view path);

}  // namespace uc::mods
