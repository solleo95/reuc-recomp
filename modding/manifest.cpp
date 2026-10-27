#include "manifest.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace uc::mods {
namespace {
using json = nlohmann::json;

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 48) return false;
    const auto lower_digit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!lower_digit(id[0])) return false;
    return std::all_of(id.begin() + 1, id.end(), [&](char c) { return lower_digit(c) || c == '_' || c == '-'; });
}

// Ids are compared and stored (config keys, cache folders, depends) in lower case, so "FFRU" and "ffru" are one mod.
std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
    return text;
}

bool reserved_id(std::string_view id) { return id == "enabled" || id == "order" || id == "allow_native" || id == "native_hash"; }   // keys of the config "mods" section

std::string extension(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) return {};
    std::string ext(path.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

// Reads an optional string member; a member of another type is an error.
std::string optional_string(const json& object, const char* key, std::vector<std::string>& errors) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (!it->is_string()) { errors.push_back(std::string(key) + " must be a string"); return {}; }
    return it->get<std::string>();
}

std::string required_string(const json& object, const char* key, std::vector<std::string>& errors) {
    if (!object.contains(key)) { errors.push_back(std::string(key) + " is required"); return {}; }
    auto value = optional_string(object, key, errors);
    if (value.empty() && object[key].is_string()) errors.push_back(std::string(key) + " must not be empty");
    return value;
}

std::map<std::string, std::string> string_map(const json& value, const std::string& what, std::vector<std::string>& errors) {
    std::map<std::string, std::string> result;
    if (!value.is_object()) { errors.push_back(what + " must be an object of language code to text"); return result; }
    for (const auto& [lang, text] : value.items()) {
        if (!text.is_string()) errors.push_back(what + "." + lang + " must be a string");
        else result[lang] = text.get<std::string>();
    }
    return result;
}

bool parse_api(std::string_view text, ApiVersion& out) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) return false;
    const auto digits = [](std::string_view s) { return !s.empty() && s.size() <= 6 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    const auto major = text.substr(0, dot), minor = text.substr(dot + 1);
    if (!digits(major) || !digits(minor)) return false;
    out.major = std::stoi(std::string(major));
    out.minor = std::stoi(std::string(minor));
    return true;
}

void parse_setting(const json& item, size_t index, ModManifest& manifest, std::set<std::string>& seen, std::vector<std::string>& errors) {
    const std::string where = "settings[" + std::to_string(index) + "]";
    if (!item.is_object()) { errors.push_back(where + " must be an object"); return; }
    SettingDecl decl;
    std::vector<std::string> local;
    decl.key = required_string(item, "key", local);
    const std::string name = decl.key.empty() ? where : "setting '" + decl.key + "'";
    if (!decl.key.empty() && !seen.insert(decl.key).second) local.push_back("duplicate key");
    const auto type = optional_string(item, "type", local);
    if (type == "bool") decl.type = SettingType::boolean;
    else if (type == "int") decl.type = SettingType::integer;
    else if (type == "number") decl.type = SettingType::number;
    else if (type == "choice") decl.type = SettingType::choice;
    else if (type == "text") decl.type = SettingType::text;
    else local.push_back("type must be one of bool, int, number, choice, text");
    if (!item.contains("default")) local.push_back("default is required");
    else decl.default_value = item["default"];
    if (item.contains("label")) {
        decl.label = string_map(item["label"], "label", local);
        if (!decl.label.count("en")) local.push_back("label.en is required");
    } else local.push_back("label.en is required");
    if (item.contains("tooltip")) decl.tooltip = string_map(item["tooltip"], "tooltip", local);
    if (item.contains("restart_required")) {
        if (!item["restart_required"].is_boolean()) local.push_back("restart_required must be true or false");
        else decl.restart_required = item["restart_required"].get<bool>();
    }
    const bool numeric = decl.type == SettingType::integer || decl.type == SettingType::number;
    for (const char* limit : {"min", "max", "step"}) {
        if (!item.contains(limit)) continue;
        if (!numeric || !item[limit].is_number()) { local.push_back(std::string(limit) + " is only valid as a number on int/number settings"); continue; }
        const double v = item[limit].get<double>();
        (std::string_view(limit) == "min" ? decl.min : std::string_view(limit) == "max" ? decl.max : decl.step) = v;
    }
    if (decl.min && decl.max && *decl.min > *decl.max) local.push_back("min is greater than max");
    if (decl.step && *decl.step <= 0) local.push_back("step must be positive");
    if (decl.type == SettingType::choice) {
        if (!item.contains("choices") || !item["choices"].is_array() || item["choices"].empty()) local.push_back("choices must be a non-empty array of strings");
        else for (const auto& c : item["choices"]) {
            if (!c.is_string()) { local.push_back("choices must be strings"); break; }
            decl.choices.push_back(c.get<std::string>());
        }
    }
    if (item.contains("default")) {
        const auto& d = decl.default_value;
        bool good = false;
        switch (decl.type) {
            case SettingType::boolean: good = d.is_boolean(); break;
            case SettingType::integer: good = d.is_number_integer(); break;
            case SettingType::number: good = d.is_number(); break;
            case SettingType::choice: good = d.is_string() && std::find(decl.choices.begin(), decl.choices.end(), d.get<std::string>()) != decl.choices.end(); break;
            case SettingType::text: good = d.is_string(); break;
        }
        if (!good) local.push_back("default has the wrong type or is not one of the choices");
        else if (numeric && ((decl.min && d.get<double>() < *decl.min) || (decl.max && d.get<double>() > *decl.max))) local.push_back("default is outside min/max");
    }
    for (const auto& message : local) errors.push_back(name + ": " + message);
    if (local.empty()) manifest.settings.push_back(std::move(decl));
}
bool valid_stem(std::string_view stem) {
    return !stem.empty() && stem.size() <= 64 && std::all_of(stem.begin(), stem.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

void parse_audio(const json& value, ModManifest& manifest, std::vector<std::string>& errors) {
    constexpr uint64_t kLimit = 2147483648ull;
    if (!value.is_object()) { errors.push_back("audio must be an object of stream name to loop points"); return; }
    for (const auto& [stem, item] : value.items()) {
        const std::string where = "audio." + stem;
        if (!valid_stem(stem)) { errors.push_back(where + ": the stream name may hold letters, digits, '_' and '-' only (no extension, no folders)"); continue; }
        if (!item.is_object()) { errors.push_back(where + " must be an object with loop_start and loop_end"); continue; }
        bool good = true;
        uint64_t numbers[2] = {};
        for (int i = 0; i < 2; ++i) {
            const char* key = i == 0 ? "loop_start" : "loop_end";
            if (!item.contains(key) || !item[key].is_number_unsigned() || item[key].get<uint64_t>() > kLimit) { errors.push_back(where + "." + key + " must be a whole number of samples from 0 to 2147483648"); good = false; }
            else numbers[i] = item[key].get<uint64_t>();
        }
        if (good && numbers[0] >= numbers[1]) { errors.push_back(where + ": loop_start must be before loop_end"); continue; }
        if (!good) continue;
        std::string lower(stem);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        manifest.audio[lower] = AudioLoop{uint32_t(numbers[0]), uint32_t(std::min<uint64_t>(numbers[1], 0xFFFFFFFFull))};
    }
}
}  // namespace

bool is_safe_relative_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

ManifestResult parse_manifest(std::string_view text) {
    ManifestResult result;
    const json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded()) { result.errors.push_back("mod.json is not valid JSON"); return result; }
    if (!root.is_object()) { result.errors.push_back("mod.json must contain a JSON object"); return result; }
    auto& errors = result.errors;
    ModManifest m;
    m.id = lower_ascii(required_string(root, "id", errors));
    if (!m.id.empty() && (!valid_id(m.id) || reserved_id(m.id))) errors.push_back("id '" + m.id + "' is not allowed (letters, digits, '_' and '-', 1-48 characters, not a reserved name)");
    m.name = required_string(root, "name", errors);
    m.version = required_string(root, "version", errors);
    m.author = optional_string(root, "author", errors);
    m.description = optional_string(root, "description", errors);
    const auto api = required_string(root, "api", errors);
    if (!api.empty() && !parse_api(api, m.api)) errors.push_back("api must look like \"1.0\" (major.minor)");
    if (root.contains("game")) {
        if (!root["game"].is_array()) errors.push_back("game must be an array of strings");
        else for (const auto& g : root["game"]) { if (g.is_string()) m.game.push_back(g.get<std::string>()); else errors.push_back("game must be an array of strings"); }
    }
    if (root.contains("depends")) {
        if (!root["depends"].is_array()) errors.push_back("depends must be an array");
        else for (const auto& d : root["depends"]) {
            if (!d.is_object()) { errors.push_back("depends entries must be objects"); continue; }
            ModDependency dep;
            dep.id = lower_ascii(required_string(d, "id", errors));
            if (!dep.id.empty() && !valid_id(dep.id)) errors.push_back("depends id '" + dep.id + "' is not a valid mod id");
            dep.min_version = optional_string(d, "min_version", errors);
            m.depends.push_back(std::move(dep));
        }
    }
    if (root.contains("touches_gameplay")) {
        if (!root["touches_gameplay"].is_boolean()) errors.push_back("touches_gameplay must be true or false");
        else m.touches_gameplay = root["touches_gameplay"].get<bool>();
    }
    m.script = optional_string(root, "script", errors);
    if (root.contains("script")) {
        const auto ext = extension(m.script);
        if (!is_safe_relative_path(m.script)) errors.push_back("script must be a relative path inside the mod folder");
        else if (ext != "lua" && ext != "js" && ext != "mjs") errors.push_back("script must end in .lua, .js or .mjs");
    }
    m.native = optional_string(root, "native", errors);
    if (root.contains("native")) {
        if (!is_safe_relative_path(m.native)) errors.push_back("native must be a relative path inside the mod folder");
        else if (extension(m.native) != "dll") errors.push_back("native must end in .dll");
    }
    if (root.contains("audio")) parse_audio(root["audio"], m, errors);
    if (root.contains("names")) m.names = string_map(root["names"], "names", errors);
    if (root.contains("settings")) {
        if (!root["settings"].is_array()) errors.push_back("settings must be an array");
        else { std::set<std::string> seen; size_t i = 0; for (const auto& s : root["settings"]) parse_setting(s, i++, m, seen, errors); }
    }
    if (errors.empty()) result.manifest = std::move(m);
    return result;
}

}  // namespace uc::mods
