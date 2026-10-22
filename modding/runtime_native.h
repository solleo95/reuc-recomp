#pragma once
#include "runtime.h"
#include "uc_mod.h"
#include <filesystem>
#include <functional>
#include <string>

namespace uc::mods {

// SHA-256 of a file as 64 lower-case hex digits; empty when the file cannot be read.
std::string sha256_file_hex(const std::filesystem::path& file);

struct NativePolicy {
    bool allow_native = false;   // the global "Allow native mods" switch
    // Asked with the mod id and the DLL's hash before it is loaded; the manager answers from the stored confirmation.
    std::function<bool(const std::string& mod_id, const std::string& sha256_hex)> confirmed;
    // The ImGui version the DLL must have been built with. The context and allocators are handed over at each draw (ModRuntime::set_imgui).
    const char* imgui_version = UC_MOD_IMGUI_VERSION;
};

// A native mod: `entry` passed to load() is the DLL path relative to the mod folder. Loading is refused unless the policy allows it,
// the hash is confirmed, uc_mod_get_info matches this port's API major/minor and ImGui version, and uc_mod_init returns 0.
// A hardware exception inside the DLL is caught and reported as a failed call; the DLL cannot be made safe beyond that.
std::unique_ptr<ModRuntime> make_native_runtime(ModContext& context, NativePolicy policy);

}  // namespace uc::mods
