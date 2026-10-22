#include "settings.h"
#include <algorithm>
#include <cmath>

namespace uc::mods {
namespace {
constexpr size_t kMaxText = 1024;
const nlohmann::json kNull;

// The value in its final form (clamped, snapped), or nullopt when the type is wrong. `note` says what was corrected.
std::optional<nlohmann::json> normalize(const SettingDecl& decl, const nlohmann::json& value, std::string& note) {
    switch (decl.type) {
        case SettingType::boolean:
            if (value.is_boolean()) return value;
            return std::nullopt;
        case SettingType::text:
            if (value.is_string() && value.get_ref<const std::string&>().size() <= kMaxText) return value;
            return std::nullopt;
        case SettingType::choice:
            if (value.is_string() && std::find(decl.choices.begin(), decl.choices.end(), value.get<std::string>()) != decl.choices.end()) return value;
            return std::nullopt;
        case SettingType::integer:
        case SettingType::number: {
            if (!value.is_number()) return std::nullopt;
            double v = value.get<double>();
            if (!std::isfinite(v)) return std::nullopt;
            if (decl.type == SettingType::integer && v != std::floor(v)) v = std::round(v);
            const double before = v;
            if (decl.step) { const double origin = decl.min.value_or(0); v = origin + std::round((v - origin) / *decl.step) * *decl.step; }
            if (decl.min) v = std::max(v, *decl.min);
            if (decl.max) v = std::min(v, *decl.max);
            if (v != before) note = "adjusted";
            if (decl.type == SettingType::integer) {
                if (std::fabs(v) >= 9.0e15) return std::nullopt;   // no range declared and too big for a 64-bit integer: llround would be undefined
                return nlohmann::json(static_cast<long long>(std::llround(v)));
            }
            return nlohmann::json(v);
        }
    }
    return std::nullopt;
}
}  // namespace

ModSettings::ModSettings(std::vector<SettingDecl> decls, const nlohmann::json& stored) : decls_(std::move(decls)) {
    for (const auto& decl : decls_) {
        values_[decl.key] = decl.default_value;
        if (!stored.is_object() || !stored.contains(decl.key)) continue;
        std::string note;
        if (const auto value = normalize(decl, stored[decl.key], note)) {
            values_[decl.key] = *value;
            if (!note.empty()) warnings_.push_back("setting '" + decl.key + "' was outside its range and has been adjusted");
        } else warnings_.push_back("setting '" + decl.key + "' in the config has the wrong type or value; the default is used");
    }
}

const nlohmann::json& ModSettings::get(std::string_view key) const {
    const auto at = values_.find(key);
    return at == values_.end() ? kNull : at->second;
}

bool ModSettings::set(std::string_view key, const nlohmann::json& value) {
    const auto decl = std::find_if(decls_.begin(), decls_.end(), [&](const SettingDecl& d) { return d.key == key; });
    if (decl == decls_.end()) return false;
    std::string note;
    const auto normalized = normalize(*decl, value, note);
    if (!normalized) return false;
    auto& current = values_.find(key)->second;
    if (current != *normalized) {
        current = *normalized;
        dirty_ = true;
        if (std::find(changed_.begin(), changed_.end(), std::string(key)) == changed_.end()) changed_.emplace_back(key);
    }
    return true;
}

std::vector<std::string> ModSettings::take_changed() {
    std::vector<std::string> keys;
    keys.swap(changed_);
    return keys;
}

nlohmann::json ModSettings::to_json() const {
    auto out = nlohmann::ordered_json::object();
    for (const auto& decl : decls_) out[decl.key] = values_.find(decl.key)->second;
    return out;
}

}  // namespace uc::mods
