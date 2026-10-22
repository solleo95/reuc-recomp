#include "host.h"
#include "data_patch.h"
#include <algorithm>
#include <cctype>

namespace uc::mods {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kQuietCallsToForgive = 1800;   // one minute of game time (about 30 nominal frames a second) without a failure

struct ModHost::Running {
    ModManifest manifest;
    std::filesystem::path root;
    std::unique_ptr<ModSettings> settings;
    std::unique_ptr<ApiRegistry> api;   // this mod's view of the game API (owner attribution, touches_gameplay)
    std::unique_ptr<ModContext> context;
    std::unique_ptr<ModRuntime> script, native;
    std::mutex mutex;   // held around every call into this mod's runtimes: the game thread and the menu thread both call in
    bool started = false;   // runtimes exist and on_load has run (sync() starts and stops mods while the game runs)
    uint32_t failures = 0;
    uint32_t quiet_calls = 0;   // on_step calls since the last failure; a long quiet stretch clears the failure count
    bool disabled = false;   // switched off for the rest of its run (a failed start or too many failures)
    bool load_failed = false;   // it failed while starting: sync() tries again (the manager may have changed what it was missing, e.g. a DLL confirmation)
    std::string last_error;
    std::map<const void*, std::shared_ptr<bool>> hook_flags;   // per runtime: false once its hooks are dropped (a hook call already in flight sees it)
    std::unique_ptr<Hooks> hooks;
    bool live() const { return started && !disabled; }
};

// The hook installer a mod's runtimes talk to. Hook points run on the game thread: each call takes the mod's lock, skips a mod that is
// switched off or whose runtime is gone, passes the argument registers in and out, and counts failures like any other callback.
class ModHost::Hooks : public HookHost {
public:
    Hooks(ModHost& host, Running& mod) : host_(host), mod_(mod) {}
    bool add_pre(const void* owner, const std::string& name, std::function<CallResult(HookRegs&, Budget)> callback) override {
        const auto address = HookRegistry::address_of(name);
        if (!address) return false;
        auto& flag = mod_.hook_flags[owner];   // the caller (a runtime inside load/fire) holds the mod's lock
        if (!flag) flag = std::make_shared<bool>(true);
        ModHost& host = host_;
        Running& mod = mod_;
        return hook_registry().add(*address, owner, [&host, &mod, alive = flag, callback = std::move(callback)](ppc::Ctx& ctx) {
            std::lock_guard lock(mod.mutex);
            if (!*alive || !mod.live()) return;
            HookRegs regs = read_hook_regs(ctx);
            const CallResult result = callback(regs, host.config_.step_budget);
            if (result.ok) write_hook_regs(ctx, regs); else host.record(mod, result);
        });
    }
    std::vector<std::string> names() const override { return HookRegistry::names(); }
private:
    ModHost& host_;
    Running& mod_;
};

ModHost::ModHost(ModRegistry& registry, ApiRegistry& api, ModHostConfig config) : registry_(registry), api_(api), config_(std::move(config)) {}
ModHost::~ModHost() { shutdown(); }

ModHost::Running* ModHost::find(std::string_view id) const {
    for (const auto& mod : mods_) if (mod->manifest.id == id) return mod.get();
    return nullptr;
}

// Every mod that could run this session gets its bookkeeping now (settings, API view, context); its runtimes exist only while it is switched on.
void ModHost::start() {
    if (!mods_.empty()) return;
    for (const ModEntry& entry : registry_.entries()) {
        if (entry.state != ModState::enabled && entry.state != ModState::disabled && entry.state != ModState::dependency_disabled) continue;   // broken, incompatible or missing a dependency
        auto mod = std::make_unique<Running>();
        mod->manifest = entry.manifest;
        mod->root = entry.root;
        const auto stored = config_.stored_settings ? config_.stored_settings(mod->manifest.id) : nlohmann::json();
        mod->settings = std::make_unique<ModSettings>(mod->manifest.settings, stored);
        for (const auto& warning : mod->settings->warnings()) if (config_.log) config_.log(mod->manifest.id, warning);
        mod->hooks = std::make_unique<Hooks>(*this, *mod);
        mod->api = std::make_unique<ApiRegistry>(api_.for_mod(mod->manifest.id, mod->manifest.touches_gameplay));
        mod->context = std::make_unique<ModContext>(ModContext{*mod->api, *mod->settings, mod->manifest.id,
            [this, id = mod->manifest.id](std::string_view line) { if (config_.log) config_.log(id, line); }, config_.ui, config_.memory,
            mod->manifest.touches_gameplay ? mod->hooks.get() : nullptr, mod->manifest.touches_gameplay});   // a hook can change anything: only gameplay mods get them
        mods_.push_back(std::move(mod));
    }
    sync();
}

// Makes the running mods match what the registry says is switched on, in load order. A mod that starts or stops takes every mod after it in load
// order with it (they restart in order), so the state is always what a fresh start would give.
void ModHost::sync() {
    std::lock_guard sync_lock(sync_mutex_);
    std::vector<Running*> desired;
    const bool native_allowed = config_.allow_native_now ? config_.allow_native_now() : config_.native.allow_native;
    for (const ModEntry* entry : registry_.active()) {
        if (config_.include && !config_.include(*entry)) continue;
        if (!entry->manifest.native.empty() && !native_allowed) continue;   // native code is switched off: such a mod does not run (a DLL cannot be half-loaded)
        if (Running* mod = find(entry->manifest.id)) desired.push_back(mod);
    }
    const auto failed_to_start = [&](const std::string& id) {
        Running& mod = *find(id);
        std::lock_guard lock(mod.mutex);
        return mod.load_failed;
    };
    size_t keep = 0;
    while (keep < run_order_.size() && keep < desired.size() && run_order_[keep] == desired[keep]->manifest.id && !failed_to_start(run_order_[keep])) ++keep;
    for (size_t i = run_order_.size(); i-- > keep;) {
        Running& mod = *find(run_order_[i]);
        std::lock_guard lock(mod.mutex);
        stop_mod(mod);
    }
    run_order_.resize(keep);
    for (size_t i = keep; i < desired.size(); ++i) {
        std::lock_guard lock(desired[i]->mutex);
        start_mod(*desired[i]);
        run_order_.push_back(desired[i]->manifest.id);
    }
}

void ModHost::start_mod(Running& mod) {
    mod.failures = 0; mod.quiet_calls = 0; mod.disabled = false; mod.load_failed = false; mod.last_error.clear();
    mod.started = true;
    ++running_count_;
    create_runtimes(mod, true, true);
    if (mod.live()) emit(mod, nullptr, "on_load", Value::object(), config_.ui_budget);
    if (mod.live()) apply_patches(mod);   // after on_load, in load order: the last mod's value stays
}

void ModHost::stop_mod(Running& mod) {
    if (!mod.started) return;
    if (mod.live()) emit(mod, nullptr, "on_unload", Value::object(), config_.ui_budget);
    drop_hooks(mod, mod.script.get());
    drop_hooks(mod, mod.native.get());
    dropped(mod);   // the game takes back what the mod set (switches, weapon values, data patches)
    mod.script.reset();   // (the lock is held: a hook call waiting for it finds the mod stopped)
    mod.native.reset();
    mod.started = false;
    mod.disabled = false;   // "off" is not a failure: switching it on again starts clean
    mod.failures = 0; mod.quiet_calls = 0; mod.last_error.clear();
    --running_count_;
}

void ModHost::apply_patches(Running& mod) {
    const auto patches = apply_data_patches(mod.root, *mod.api);
    for (const auto& error : patches.errors) if (config_.log) config_.log(mod.manifest.id, "data patch: " + error);
}

void ModHost::create_runtimes(Running& mod, bool scripts, bool native) {
    const auto make = [&](RuntimeKind kind) -> std::unique_ptr<ModRuntime> {
        if (config_.runtime_factory) return config_.runtime_factory(kind, *mod.context);
        switch (kind) {
            case RuntimeKind::lua: return make_lua_runtime(*mod.context);
            case RuntimeKind::js: return make_js_runtime(*mod.context);
            case RuntimeKind::native: {
                NativePolicy policy = config_.native;
                if (config_.allow_native_now) policy.allow_native = config_.allow_native_now();   // the switch in the Mods tab, as it is now
                return make_native_runtime(*mod.context, std::move(policy));
            }
        }
        return nullptr;
    };
    const auto start_one = [&](RuntimeKind kind, const std::string& entry, std::unique_ptr<ModRuntime>& slot) {
        slot = make(kind);
        const CallResult result = slot->load(mod.root, entry);
        if (!result.ok) {
            mod.disabled = true;
            mod.load_failed = true;
            mod.last_error = result.error;
            if (config_.log) config_.log(mod.manifest.id, "could not start: " + result.error);
            dropped(mod);
        }
    };
    if (scripts && !mod.manifest.script.empty()) {
        const auto& s = mod.manifest.script;
        std::string ext = s.size() >= 4 ? s.substr(s.size() - 4) : std::string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });   // the manifest accepts any case
        start_one(ext == ".lua" ? RuntimeKind::lua : RuntimeKind::js, s, mod.script);
    }
    if (native && !mod.manifest.native.empty() && !mod.disabled) start_one(RuntimeKind::native, mod.manifest.native, mod.native);
}

void ModHost::record(Running& mod, const CallResult& result) {
    if (result.ok) return;
    ++mod.failures;
    mod.quiet_calls = 0;
    mod.last_error = result.error;
    if (config_.log) config_.log(mod.manifest.id, (result.budget_exceeded ? "over its time budget: " : "error: ") + result.error);
    if (mod.failures >= config_.max_failures && !mod.disabled) {
        mod.disabled = true;
        if (config_.log) config_.log(mod.manifest.id, "switched off for this session after " + std::to_string(mod.failures) + " failures");
        dropped(mod);
    }
}

void ModHost::dropped(Running& mod) {
    if (config_.on_mod_dropped) config_.on_mod_dropped(mod.manifest.id);
}

void ModHost::emit(Running& mod, ModRuntime* only, std::string_view event, const Value& payload, Budget budget) {
    for (ModRuntime* runtime : {mod.script.get(), mod.native.get()}) {
        if (!runtime || (only && runtime != only)) continue;
        const CallResult result = runtime->fire(event, payload, budget);
        if (!result.ok) { record(mod, result); break; }
    }
}

void ModHost::drop_hooks(Running& mod, const void* owner) {
    if (!owner) return;
    if (const auto at = mod.hook_flags.find(owner); at != mod.hook_flags.end()) { *at->second = false; mod.hook_flags.erase(at); }
    hook_registry().remove_owner(owner);   // owner is the runtime's address: runtimes register themselves with `this`
}

void ModHost::step(uint32_t nominal) {
    if (running_count_.load(std::memory_order_relaxed) == 0) return;   // no mod is switched on: nothing to run, nothing to pay
    for (const auto& tick : config_.ticks) tick();
    std::vector<std::pair<std::string, std::string>> changed;
    {
        std::lock_guard lock(pending_mutex_);
        changed.swap(pending_settings_);
    }
    for (const auto& [id, key] : changed) {
        if (Running* mod = find(id)) {
            std::lock_guard lock(mod->mutex);
            if (mod->live()) emit(*mod, nullptr, "on_settings_changed", Value{{"key", key}}, config_.ui_budget);
        }
    }
    if (nominal == 0) return;
    const auto begin = Clock::now();
    const size_t count = mods_.size();
    for (uint32_t frame = 0; frame < std::min<uint32_t>(nominal, 4); ++frame) {
        for (size_t i = 0; i < count; ++i) {
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin);
            const auto remaining = config_.step_budget.limit - spent;
            if (remaining.count() <= 0) break;   // the step's share is used up; the next step starts with another mod
            Running& mod = *mods_[(rotation_ + i) % count];
            std::lock_guard lock(mod.mutex);
            if (!mod.live()) continue;
            // Everything that is left is the interrupt limit, but a mod is only failed for its own overrun: one that stayed within an equal share of the
            // step did not cause it (a slow mod ahead of it ate the budget), so its call is just cut short.
            const auto started = Clock::now();
            const CallResult result = run_step(mod, remaining);
            if (!result.ok) {
                const auto own = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
                if (!result.budget_exceeded || own > config_.step_budget.limit / int64_t(count)) record(mod, result);
            }
            if (mod.live() && ++mod.quiet_calls >= kQuietCallsToForgive && mod.failures) mod.failures = 0;   // record() resets quiet_calls on a failure
        }
    }
    rotation_ = (rotation_ + 1) % count;
}

CallResult ModHost::run_step(Running& mod, std::chrono::microseconds limit) {
    for (ModRuntime* runtime : {mod.script.get(), mod.native.get()}) {
        if (!runtime) continue;
        const CallResult result = runtime->fire("on_step", Value::object(), Budget{limit});
        if (!result.ok) return result;
    }
    return {};
}

void ModHost::fire_game_event(std::string_view event, const Value& payload) {
    for (auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        if (mod->live()) emit(*mod, nullptr, event, payload, config_.step_budget);
    }
}

void ModHost::settings_changed(std::string_view mod_id, std::string_view key) {
    std::lock_guard lock(pending_mutex_);
    const std::pair<std::string, std::string> item{std::string(mod_id), std::string(key)};
    if (std::find(pending_settings_.begin(), pending_settings_.end(), item) == pending_settings_.end()) pending_settings_.push_back(item);   // a paused game must not pile them up
}

CallResult ModHost::draw_ui(std::string_view mod_id, const ImGuiHandles* imgui) {
    if (Running* mod = find(mod_id)) {
        std::lock_guard lock(mod->mutex);
        if (!mod->live()) return {};
        for (ModRuntime* runtime : {mod->script.get(), mod->native.get()}) {
            if (!runtime) continue;
            if (imgui) runtime->set_imgui(*imgui);
            const CallResult result = runtime->draw_ui(config_.ui_budget);
            if (!result.ok) { record(*mod, result); return result; }
        }
    }
    return {};
}

void ModHost::reload_scripts() {
    std::lock_guard sync_lock(sync_mutex_);
    for (const auto& id : run_order_) {   // in start order: overlapping data patches resolve as after a fresh start
        Running* mod = find(id);
        if (!mod) continue;
        std::lock_guard lock(mod->mutex);
        if (!mod->started || !mod->script) continue;
        emit(*mod, mod->script.get(), "on_unload", Value::object(), config_.ui_budget);
        drop_hooks(*mod, mod->script.get());
        dropped(*mod);   // everything the mod set is taken back, its data patches included: they are applied again below
        mod->script.reset();
        if (!mod->native) { mod->disabled = false; mod->load_failed = false; mod->failures = 0; mod->last_error.clear(); }   // a native part keeps its failure state
        create_runtimes(*mod, true, false);
        if (mod->script && mod->live()) {
            emit(*mod, mod->script.get(), "on_load", Value::object(), config_.ui_budget);
            if (mod->live()) apply_patches(*mod);
        }
    }
}

void ModHost::shutdown() {
    std::lock_guard sync_lock(sync_mutex_);
    for (size_t i = run_order_.size(); i-- > 0;) {
        if (Running* mod = find(run_order_[i])) {
            std::lock_guard lock(mod->mutex);
            stop_mod(*mod);
        }
    }
    run_order_.clear();
    mods_.clear();
}

std::vector<ModHost::ModStatus> ModHost::status() const {
    std::vector<ModStatus> result;
    for (const auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        const bool ui = (mod->script && mod->script->has_ui()) || (mod->native && mod->native->has_ui());
        result.push_back({mod->manifest.id, mod->started, mod->disabled, !mod->manifest.native.empty(), ui, !mod->manifest.settings.empty(), mod->last_error, mod->failures});
    }
    return result;
}

ModSettings* ModHost::settings_of(std::string_view mod_id) {
    const Running* mod = find(mod_id);
    return mod ? mod->settings.get() : nullptr;
}

std::vector<std::string> ModHost::ids() const {
    std::vector<std::string> result;
    for (const auto& mod : mods_) result.push_back(mod->manifest.id);
    return result;
}

bool ModHost::try_edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit) {
    Running* mod = find(mod_id);
    if (!mod) return false;
    std::unique_lock lock(mod->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    edit(*mod->settings);
    return true;
}

bool ModHost::edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit) {
    Running* mod = find(mod_id);
    if (!mod) return false;
    std::lock_guard lock(mod->mutex);
    edit(*mod->settings);
    return true;
}

}  // namespace uc::mods
