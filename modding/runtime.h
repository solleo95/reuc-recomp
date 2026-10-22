#pragma once
#include "api_registry.h"
#include "game_api.h"
#include "hook_registry.h"
#include "settings.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

// Widgets a mod may draw in its tab (schema domain "ui"). Implemented over ImGui by the menu; tests use a recording fake.
// Only valid while the menu draws the mod's tab; outside that the calls do nothing and return their input unchanged.
class UiApi {
public:
    virtual ~UiApi() = default;
    virtual void text(const std::string& text) = 0;
    virtual bool button(const std::string& label) = 0;
    virtual bool checkbox(const std::string& label, bool value) = 0;
    virtual int slider_int(const std::string& label, int value, int min, int max) = 0;
    virtual double slider_float(const std::string& label, double value, double min, double max) = 0;
    virtual int combo(const std::string& label, int selected, const std::vector<std::string>& items) = 0;
    virtual std::string input_text(const std::string& label, const std::string& value) = 0;
    virtual void separator() = 0;
    virtual void same_line() = 0;
    virtual bool collapsing_header(const std::string& label) = 0;
    virtual void tooltip(const std::string& text) = 0;
    virtual std::array<float, 4> color_edit(const std::string& label, std::array<float, 4> rgba) = 0;
};

struct Budget { std::chrono::microseconds limit{1000}; };

// The ImGui context and allocators the menu draws with (ImGui::GetCurrentContext, ImGui::GetAllocatorFunctions). Opaque to everything but the menu.
struct ImGuiHandles {
    void* context = nullptr;
    void* (*alloc)(size_t size, void* user) = nullptr;
    void (*free)(void* pointer, void* user) = nullptr;
    void* user = nullptr;
};

struct CallResult {
    bool ok = true;
    std::string error;
    bool budget_exceeded = false;
};

// Where a runtime installs hook callbacks (schema domain "hook"). The owner pointer identifies the runtime: the host removes its hooks
// before the runtime is destroyed or reloaded, and a hook of a runtime that is gone is never called.
class HookHost {
public:
    virtual ~HookHost() = default;
    // The callback gets the guest's argument registers and may change them; it runs on the game thread inside the budget. pre: before the function
    // (changes go into the function's arguments); replace: instead of it (the registers r3 / f1 are the return value); post: after it returned
    // (r3 / f1 are the return value and may be changed). False when `name` is not a hookable function.
    virtual bool add_hook(HookKind kind, const void* owner, const std::string& name, std::function<CallResult(HookRegs&, Budget)> callback) = 0;
    bool add_pre(const void* owner, const std::string& name, std::function<CallResult(HookRegs&, Budget)> callback) { return add_hook(HookKind::pre, owner, name, std::move(callback)); }
    virtual std::vector<std::string> names() const = 0;
};

// What a runtime may touch: the registry domains (uc.*), its mod's settings, a log sink and the menu widgets.
struct ModContext {
    ApiRegistry& api;
    ModSettings& settings;
    std::string mod_id;
    std::function<void(std::string_view line)> log;
    UiApi* ui = nullptr;   // null in headless runs
    GuestMemory* memory = nullptr;   // native mods read and write guest memory through it
    HookHost* hooks = nullptr;
    bool gameplay = true;   // the manifest says touches_gameplay: a DLL may write guest memory only then
};

class ModRuntime {
public:
    virtual ~ModRuntime() = default;
    // Loads the mod's entry file (relative to mod_root) and runs it; handlers registered with on() are kept.
    virtual CallResult load(const std::filesystem::path& mod_root, const std::string& entry_relative) = 0;
    // Runs every handler registered for the event, with the payload, inside the budget. Stops at the first failing handler.
    virtual CallResult fire(std::string_view event, const Value& payload, Budget budget) = 0;
    // Runs the function registered with ui.draw(), if any.
    virtual CallResult draw_ui(Budget budget) = 0;
    virtual bool has_ui() const { return false; }   // a drawing function was registered: the mod gets a tab even without declared settings
    virtual void set_imgui(const ImGuiHandles&) {}   // before draw_ui: the menu's ImGui context, for runtimes that draw with their own copy of ImGui (DLLs)
    virtual std::string name() const = 0;
};

std::unique_ptr<ModRuntime> make_lua_runtime(ModContext& context);
std::unique_ptr<ModRuntime> make_js_runtime(ModContext& context);   // QuickJS-ng; entry files are ES modules

}  // namespace uc::mods
