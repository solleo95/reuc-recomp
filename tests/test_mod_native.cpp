#include "mod_test_rig.h"
#include "guest_mem.h"
#include "mod_paths.h"
#include "runtime_native.h"
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;

namespace {
struct NativeRig {
    ApiRegistry api;
    GuestMemory memory;
    ModSettings settings;
    std::vector<std::string> log;
    std::unique_ptr<ModContext> context;
    std::unique_ptr<ModRuntime> runtime;
    std::string asked_id, asked_hash;
    rig::FakeHooks hooks;

    NativeRig(bool allow, bool confirm)
        : memory((uc::guest_mem_init(), uc::guest_mem_base())),
          settings(parse_manifest(R"({"id":"m","name":"m","version":"1","api":"1.0","settings":[{"key":"scale","type":"number","default":2.5,"label":{"en":"S"}}]})").manifest->settings, json::object()) {
        register_low_level_api(api, memory, SymbolTable{[](std::string_view) { return std::optional<uint32_t>(); }, [](uint32_t) { return std::string(); }});
        context = std::make_unique<ModContext>(ModContext{api, settings, "nativemod", [this](std::string_view line) { log.emplace_back(line); }, nullptr, &memory, &hooks});
        NativePolicy policy;
        policy.allow_native = allow;
        policy.confirmed = [this, confirm](const std::string& id, const std::string& hash) { asked_id = id; asked_hash = hash; return confirm; };
        runtime = make_native_runtime(*context, policy);
    }
    uint32_t word(uint32_t address) { return memory.read_u32(address).value(); }
    void set(uint32_t address, uint32_t v) { memory.write_u32(address, v); }
};

fs::path fresh(const std::u8string& name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

// Copies a built test DLL into a fresh mod folder as native/mod.dll and returns the folder.
fs::path mod_with(const char* dll, const std::u8string& folder) {
    const auto root = fresh(folder);
    fs::create_directories(root / "native");
    fs::copy_file(dll, root / "native" / "mod.dll");
    return root;
}
const Budget kBudget{std::chrono::milliseconds(200)};
}  // namespace

static void sha256_known() {
    const auto dir = fresh(u8"native_sha");
    std::ofstream(dir / "empty") << "";
    std::ofstream(dir / "abc", std::ios::binary) << "abc";
    CHECK(sha256_file_hex(dir / "empty") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_file_hex(dir / "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_file_hex(dir / "missing").empty());
}

static void refuses_when_not_allowed() {
    NativeRig rig(false, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_off"), "native/mod.dll");
    CHECK(!r.ok && r.error.find("native") != std::string::npos && rig.asked_id.empty());   // not even asked
}

static void refuses_when_unconfirmed() {
    NativeRig rig(true, false);
    const auto root = mod_with(TEST_DLL_GOOD, u8"native_unconfirmed");
    const auto r = rig.runtime->load(root, "native/mod.dll");
    CHECK(!r.ok && r.error.find("confirm") != std::string::npos);
    CHECK(rig.asked_id == "nativemod" && rig.asked_hash == sha256_file_hex(root / "native" / "mod.dll") && rig.asked_hash.size() == 64);
    CHECK(rig.log.empty());   // nothing of the DLL ran
}

static void loads_test_dll() {
    NativeRig rig(true, true);
    rig.set(0x80000300, 0); rig.set(0x80000304, 0); rig.set(0x80000308, 0); rig.set(0x8000030C, 0);
    rig.set(0x80000000, 77);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_good"), "native/mod.dll");
    CHECK(r.ok);
    CHECK((rig.log == std::vector<std::string>{"nativemod", "77", "2.5"}));
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok && rig.runtime->fire("on_step", json::object(), kBudget).ok);
    CHECK(rig.word(0x80000300) == 2);
    CHECK(rig.runtime->fire("on_load", json{{"hello", 1}}, kBudget).ok && rig.log.back() == "{\"hello\":1}");
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.word(0x8000030C) == 1);
    CHECK(rig.runtime->fire("on_unknown", json::object(), kBudget).ok);
    CHECK(rig.word(0x80000304) == 0);
    rig.runtime.reset();
    CHECK(rig.word(0x80000304) == 1);   // uc_mod_shutdown ran exactly once
}

// The menu's ImGui context is handed over for every draw (it is not known when the DLL loads, and a DLL has its own copy of ImGui's globals).
static void imgui_handles_reach_the_dll_at_each_draw() {
    NativeRig rig(true, true);
    CHECK(rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_imgui_ctx"), "native/mod.dll").ok);
    rig.set(0x80000310, 0);
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.word(0x80000310) == 0);   // nothing was handed over yet
    ImGuiHandles handles{reinterpret_cast<void*>(0x1234), nullptr, nullptr, reinterpret_cast<void*>(0x5678)};
    rig.runtime->set_imgui(handles);
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.word(0x80000310) == 0x1234u && rig.word(0x80000314) == 0x5678u);
    handles.context = reinterpret_cast<void*>(0x4321);   // a new context (the menu was recreated)
    rig.runtime->set_imgui(handles);
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.word(0x80000310) == 0x4321u);
}

// The menu thread and the game thread both call into a DLL (under the host's lock); the C API must find the right runtime on either.
static void callbacks_from_another_thread() {
    NativeRig rig(true, true);
    rig.set(0x80000300, 0); rig.set(0x80000308, 0);
    CHECK(rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_threads"), "native/mod.dll").ok);
    CallResult step, draw;
    std::thread other([&] { step = rig.runtime->fire("on_step", json::object(), kBudget); draw = rig.runtime->draw_ui(kBudget); });
    other.join();
    CHECK(step.ok && draw.ok && rig.word(0x80000300) == 1);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok && rig.word(0x80000300) == 2);
}

static void native_hook_pre() {
    NativeRig rig(true, true);
    CHECK(rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_hook"), "native/mod.dll").ok);
    CHECK(rig.hooks.callbacks.size() == 1 && rig.hooks.owners[0] != nullptr);
    HookRegs regs;
    regs.r[0] = 7; regs.f[0] = 1.5;
    CHECK(rig.hooks.callbacks[0](regs, kBudget).ok);
    CHECK(regs.r[1] == 8 && regs.f[1] == 3.0);
}

static void api_major_mismatch_refused() {
    NativeRig rig(true, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_MAJOR2, u8"native_major"), "native/mod.dll");
    CHECK(!r.ok && r.error.find("API") != std::string::npos);
}

static void imgui_version_mismatch_refused() {
    NativeRig rig(true, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_OLD_IMGUI, u8"native_imgui"), "native/mod.dll");
    CHECK(!r.ok && r.error.find("ImGui") != std::string::npos);
}

static void missing_exports_reported() {
    NativeRig rig(true, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_EMPTY, u8"native_empty"), "native/mod.dll");
    CHECK(!r.ok && r.error.find("uc_mod_get_info") != std::string::npos);
    const auto not_a_dll = fresh(u8"native_notdll");
    fs::create_directories(not_a_dll / "native");
    std::ofstream(not_a_dll / "native" / "mod.dll") << "this is text";
    NativeRig other(true, true);
    const auto text_file = other.runtime->load(not_a_dll, "native/mod.dll");
    CHECK(!text_file.ok && text_file.error.find("not a Windows DLL") != std::string::npos);   // refused before Windows can show its own error box
    const auto truncated = mod_with(TEST_DLL_GOOD, u8"native_truncated");   // a real header with the rest cut off: LoadLibrary fails, and must do so silently
    fs::resize_file(truncated / "native" / "mod.dll", 1024);
    const auto cut = other.runtime->load(truncated, "native/mod.dll");
    CHECK(!cut.ok && cut.error.find("Windows could not load") != std::string::npos);
    CHECK(!other.runtime->load(not_a_dll, "native/absent.dll").ok);
    CHECK(!other.runtime->load(not_a_dll, "../escape.dll").ok);
}

static void init_failure_reported() {
    NativeRig rig(true, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_INIT_FAILS, u8"native_initfail"), "native/mod.dll");
    CHECK(!r.ok && r.error.find("uc_mod_init") != std::string::npos);
    rig.set(0x80000304, 0);
    rig.runtime.reset();
    CHECK(rig.word(0x80000304) == 0);   // a mod that failed to start is not shut down
}

static void slow_and_crashing_callbacks() {
    NativeRig rig(true, true);
    CHECK(rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_bad"), "native/mod.dll").ok);
    rig.set(0x80000308, 1);   // the DLL sleeps 30 ms
    const auto slow = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    CHECK(!slow.ok && slow.budget_exceeded);
    rig.set(0x80000308, 2);   // the DLL dereferences null
    const auto crash = rig.runtime->fire("on_step", json::object(), kBudget);
    CHECK(!crash.ok && crash.error.find("C0000005") != std::string::npos);
    rig.set(0x80000308, 0);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok);   // the port is still standing
}

static void non_ascii_dll_path() {
    NativeRig rig(true, true);
    const auto r = rig.runtime->load(mod_with(TEST_DLL_GOOD, u8"native_моды"), "native/mod.dll");
    CHECK(r.ok);
}

int main() {
    callbacks_from_another_thread(); imgui_handles_reach_the_dll_at_each_draw(); sha256_known(); refuses_when_not_allowed(); refuses_when_unconfirmed(); loads_test_dll(); native_hook_pre(); api_major_mismatch_refused(); imgui_version_mismatch_refused();
    missing_exports_reported(); init_failure_reported(); slow_and_crashing_callbacks(); non_ascii_dll_path();
    std::puts("ok");
}
