#include "runtime_native.h"

// Linux: native (DLL) mods are not supported yet; Lua and JavaScript mods work. A native mod would be a shared library loaded with dlopen behind the same
// policy (allow switch, confirmed hash, API version check) as runtime_native.cpp does for Windows DLLs.
namespace uc::mods {
std::string sha256_file_hex(const std::filesystem::path&) { return {}; }   // "cannot be read": only native mods are hashed

namespace {
class UnsupportedNativeRuntime final : public ModRuntime {
public:
    CallResult load(const std::filesystem::path&, const std::string&) override { return {false, "native mods are not supported on Linux", false}; }
    CallResult fire(std::string_view, const Value&, Budget) override { return {}; }
    CallResult draw_ui(Budget) override { return {}; }
    std::string name() const override { return "native"; }
};
}  // namespace

std::unique_ptr<ModRuntime> make_native_runtime(ModContext&, NativePolicy) { return std::make_unique<UnsupportedNativeRuntime>(); }
}  // namespace uc::mods
