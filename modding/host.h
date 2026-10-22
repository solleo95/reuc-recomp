#pragma once
#include "registry.h"
#include "runtime.h"
#include "runtime_native.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uc::mods {

enum class RuntimeKind { lua, js, native };

struct ModHostConfig {
    Budget step_budget{std::chrono::milliseconds(1)};   // all on_step callbacks of all mods together, per game step
    Budget ui_budget{std::chrono::milliseconds(4)};     // one mod's menu callbacks (draw, settings changed)
    uint32_t max_failures = 3;                          // errors or overruns after which a mod is switched off for the session
    NativePolicy native;
    std::function<bool(const ModEntry&)> include;   // which active mods to start (null: all)
    UiApi* ui = nullptr;
    GuestMemory* memory = nullptr;
    std::function<nlohmann::json(const std::string& mod_id)> stored_settings;       // the config's mods.<id>.settings (null/empty: defaults)
    std::function<void(std::string_view mod_id, std::string_view line)> log;
    std::function<void(const std::string& mod_id)> on_mod_dropped;                  // a mod was switched off, its scripts reloaded or the host shut down: take back what it set in the game
    std::vector<std::function<void()>> ticks;                                     // run at the start of every step, before any mod callback (game API per-step logic)
    // Test seam: replaces the real runtimes.
    std::function<std::unique_ptr<ModRuntime>(RuntimeKind, ModContext&)> runtime_factory;
};

// Runs the active mods: one runtime per script/DLL, every callback behind a per-mod lock (the game thread and the menu thread both call in),
// time budgets, and the failure policy (see ModHostConfig). Nothing is created for a registry without active mods.
class ModHost {
public:
    ModHost(ModRegistry& registry, ApiRegistry& api, ModHostConfig config);
    ~ModHost();

    void start();                                               // loads every active mod, then fires on_load to it
    // Once per game step, game thread. `nominal` is how many nominal (60 Hz) frames the step covers: ticks run every step, the mods' on_step
    // handlers `nominal` times (0 for a step shorter than a nominal frame; at most 4).
    void step(uint32_t nominal = 1);
    void fire_game_event(std::string_view event, const Value& payload);
    void settings_changed(std::string_view mod_id, std::string_view key);   // any thread; on_settings_changed is delivered on the game thread at the start of the next step()
    CallResult draw_ui(std::string_view mod_id, const ImGuiHandles* imgui = nullptr);   // menu thread: the mod's free-drawing callback; imgui goes to runtimes that need it
    void reload_scripts();                                      // restarts script runtimes only (overlay, textures and DLLs need a restart)
    void shutdown();                                            // on_unload, then the runtimes are destroyed

    struct ModStatus {
        std::string id;
        bool running = false;
        bool disabled_for_session = false;
        bool has_native = false;
        bool has_ui = false;          // a runtime registered a drawing function
        bool has_settings = false;    // mod.json declares settings
        std::string last_error;
        uint32_t failures = 0;
    };
    std::vector<ModStatus> status() const;
    ModSettings* settings_of(std::string_view mod_id);           // the live values; null for a mod that is not running. Not synchronised: use edit_settings from another thread
    bool edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit);   // runs edit under the mod's lock; false when the mod is not running
    // Like edit_settings, but gives up (false) when the mod is busy or not running, instead of waiting: for the main loop, which must not stall behind a slow mod.
    bool try_edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit);
    std::vector<std::string> ids() const;                         // the running mods in load order; takes no lock
    bool empty() const { return mods_.empty(); }

private:
    struct Running;
    class Hooks;
    void create_runtimes(Running& mod, bool scripts, bool native);
    void drop_hooks(Running& mod, const void* owner);          // before a runtime is destroyed: its hooks stop being called; caller holds the lock
    void record(Running& mod, const CallResult& result);        // counts a failure; caller holds the mod's lock
    // Fires the event at one runtime (only != nullptr) or at all of the mod's runtimes; stops at the first failure. Caller holds the lock.
    void emit(Running& mod, ModRuntime* only, std::string_view event, const Value& payload, Budget budget);

    ModRegistry& registry_;
    ApiRegistry& api_;
    ModHostConfig config_;
    void dropped(Running& mod);                                 // tells config_.on_mod_dropped; caller holds the mod's lock

    std::vector<std::unique_ptr<Running>> mods_;
    size_t rotation_ = 0;
    std::mutex pending_mutex_;
    std::vector<std::pair<std::string, std::string>> pending_settings_;   // (mod id, key) waiting for the next step
};

}  // namespace uc::mods
