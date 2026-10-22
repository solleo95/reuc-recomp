#pragma once
#include "registry.h"
#include "runtime.h"
#include "runtime_native.h"
#include <atomic>
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
    uint32_t max_failures = 3;                          // errors or overruns after which a mod is switched off until it is switched off and on again
    NativePolicy native;
    std::function<bool()> allow_native_now;         // the "Allow native mods" switch as it is when a DLL is about to load (null: native.allow_native)
    std::function<bool(const ModEntry&)> include;   // which enabled mods may run (null: all)
    UiApi* ui = nullptr;
    GuestMemory* memory = nullptr;
    std::function<nlohmann::json(const std::string& mod_id)> stored_settings;       // the config's mods.<id>.settings (null/empty: defaults)
    std::function<void(std::string_view mod_id, std::string_view line)> log;
    std::function<void(const std::string& mod_id)> on_mod_dropped;                  // a mod was switched off, its scripts reloaded or the host shut down: take back what it set in the game
    std::vector<std::function<void()>> ticks;                                     // run at the start of every step, before any mod callback (game API per-step logic)
    // Test seam: replaces the real runtimes.
    std::function<std::unique_ptr<ModRuntime>(RuntimeKind, ModContext&)> runtime_factory;
};

// Runs the enabled mods: one runtime per script/DLL, every callback behind a per-mod lock (the game thread and the menu thread both call in),
// time budgets, and the failure policy (see ModHostConfig). Mods are switched on and off while the game runs (sync()); with none on, step() costs one load.
class ModHost {
public:
    ModHost(ModRegistry& registry, ApiRegistry& api, ModHostConfig config);
    ~ModHost();

    void start();                                               // sets up every mod that could run, then sync()
    // Makes the running mods match the registry (enabled, dependencies, order) and config.include: mods that were switched off get on_unload and
    // everything they set in the game taken back, mods that were switched on get on_load. A change takes every later mod in load order along (they restart
    // in order). Menu/main thread.
    void sync();
    // Once per game step, game thread. `nominal` is how many nominal frames (the game's own 30 Hz step, about 30 per second of game time) the step covers: ticks run every step, the mods' on_step
    // handlers `nominal` times (0 for a step shorter than a nominal frame; at most 4).
    void step(uint32_t nominal = 1);
    void fire_game_event(std::string_view event, const Value& payload);
    void settings_changed(std::string_view mod_id, std::string_view key);   // any thread; on_settings_changed is delivered on the game thread at the start of the next step()
    CallResult draw_ui(std::string_view mod_id, const ImGuiHandles* imgui = nullptr);   // menu thread: the mod's free-drawing callback; imgui goes to runtimes that need it
    void reload_scripts();                                      // restarts the script runtimes of running mods (DLLs stay)
    void shutdown();                                            // on_unload, then the runtimes are destroyed

    struct ModStatus {
        std::string id;
        bool running = false;         // switched on and started (it may still be disabled_for_session)
        bool disabled_for_session = false;
        bool has_native = false;
        bool has_ui = false;          // a runtime registered a drawing function
        bool has_settings = false;    // mod.json declares settings
        std::string last_error;
        uint32_t failures = 0;
    };
    std::vector<ModStatus> status() const;                       // every mod that could run this session, switched on or not
    ModSettings* settings_of(std::string_view mod_id);           // the live values; null for an unknown mod. Not synchronised: use edit_settings from another thread
    bool edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit);   // runs edit under the mod's lock; false for an unknown mod
    // Like edit_settings, but gives up (false) when the mod is busy, instead of waiting: for the main loop, which must not stall behind a slow mod.
    bool try_edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit);
    std::vector<std::string> ids() const;                         // every mod that could run this session, in load order at start; takes no lock
    int running_count() const { return running_count_.load(std::memory_order_relaxed); }
    bool empty() const { return mods_.empty(); }

private:
    struct Running;
    class Hooks;
    Running* find(std::string_view id) const;
    void start_mod(Running& mod);                               // caller holds the mod's lock
    void stop_mod(Running& mod);                                // caller holds the mod's lock
    CallResult run_step(Running& mod, std::chrono::microseconds limit);   // on_step at every runtime of the mod; caller holds the lock
    void apply_patches(Running& mod);                           // caller holds the mod's lock
    void create_runtimes(Running& mod, bool scripts, bool native);
    void drop_hooks(Running& mod, const void* owner);          // before a runtime is destroyed: its hooks stop being called; caller holds the lock
    void record(Running& mod, const CallResult& result);        // counts a failure; caller holds the mod's lock
    // Fires the event at one runtime (only != nullptr) or at all of the mod's runtimes; stops at the first failure. Caller holds the lock.
    void emit(Running& mod, ModRuntime* only, std::string_view event, const Value& payload, Budget budget);
    void dropped(Running& mod);                                 // tells config_.on_mod_dropped; caller holds the mod's lock

    ModRegistry& registry_;
    ApiRegistry& api_;
    ModHostConfig config_;
    std::vector<std::unique_ptr<Running>> mods_;                // fixed after start(): sync() only starts and stops the runtimes inside
    std::vector<std::string> run_order_;                        // ids in the order they were started (sync_mutex_)
    std::mutex sync_mutex_;
    std::atomic<int> running_count_{0};
    size_t rotation_ = 0;
    std::mutex pending_mutex_;
    std::vector<std::pair<std::string, std::string>> pending_settings_;   // (mod id, key) waiting for the next step
};

}  // namespace uc::mods
