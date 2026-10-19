#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace uc {
// What this machine supports, as `uc.exe --features` prints it for the launcher. An unreadable report offers nothing.
struct FeatureReport {
    bool dlss=false, dlss_g=false, fsr=false, fsr_fg=false;
    uint32_t dlss_g_max_multiplier=0;   // presented frames per rendered frame DLSS-G allows (2 on RTX 40, up to 6 on RTX 50)
    std::string dlss_reason, dlss_g_reason, fsr_reason, fsr_fg_reason;   // why a feature is unavailable, shown in the launcher
    bool effects=false;            // the GPU can run the screen-space effects (motion attachment path: unused attachments + independent blend)
    std::string effects_reason;
    uint32_t anisotropy_max=1;     // highest anisotropic filtering level (1 = unsupported), a power of two up to 16
};
inline std::string to_json(const FeatureReport& r) {
    return nlohmann::json{{"dlss",r.dlss},{"dlss_g",r.dlss_g},{"fsr",r.fsr},{"fsr_fg",r.fsr_fg},{"dlss_g_max_multiplier",r.dlss_g_max_multiplier},
        {"dlss_reason",r.dlss_reason},{"dlss_g_reason",r.dlss_g_reason},{"fsr_reason",r.fsr_reason},{"fsr_fg_reason",r.fsr_fg_reason},
        {"effects",r.effects},{"effects_reason",r.effects_reason},{"anisotropy_max",r.anisotropy_max}}.dump();
}
inline FeatureReport feature_report_from_json(std::string_view text) {
    FeatureReport r;
    const auto json=nlohmann::json::parse(text,nullptr,false);
    if(json.is_discarded() || !json.is_object()) return r;
    try {
        const auto flag=[&](const char* key) { return json.contains(key) && json[key].get<bool>(); };
        const auto words=[&](const char* key) { return json.contains(key) ? json[key].get<std::string>() : std::string{}; };
        r.dlss=flag("dlss"); r.dlss_g=flag("dlss_g"); r.fsr=flag("fsr"); r.fsr_fg=flag("fsr_fg");
        if(json.contains("dlss_g_max_multiplier")) r.dlss_g_max_multiplier=json["dlss_g_max_multiplier"].get<uint32_t>();
        r.dlss_reason=words("dlss_reason"); r.dlss_g_reason=words("dlss_g_reason"); r.fsr_reason=words("fsr_reason"); r.fsr_fg_reason=words("fsr_fg_reason");
        r.effects=flag("effects"); r.effects_reason=words("effects_reason");
        if(json.contains("anisotropy_max")) r.anisotropy_max=json["anisotropy_max"].get<uint32_t>();
    } catch(const nlohmann::json::exception&) { return {}; }   // wrong types: offer nothing
    return r;
}
}
