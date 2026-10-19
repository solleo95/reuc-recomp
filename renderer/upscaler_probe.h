#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace uc {
// What this machine supports, checked with the same runtime files the game loads (uc.exe --features for the launcher).
struct UpscalerSupport {
    bool dlss=false, dlss_g=false, fsr=false, fsr_fg=false;
    uint32_t dlss_g_max_multiplier=0;   // presented frames per rendered frame DLSS-G allows
    std::string dlss_reason, dlss_g_reason, fsr_reason, fsr_fg_reason;
    bool effects=false;          // unused attachments + independent blend: the path of the screen-space effects
    std::string effects_reason;
    uint32_t anisotropy_max=1;   // 1 when samplerAnisotropy is missing
};
UpscalerSupport probe_upscalers(const std::filesystem::path& directory);
}
