#include "host.h"
#include "data_patch.h"
#include <algorithm>
#include <cctype>

namespace uc::mods {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kQuietCallsToForgive = 3600;   // one minute of nominal 60 Hz steps without a failure

struct ModHost::Running {
    ModManifest manifest;
    std::filesystem::path root;
    std::unique_ptr<ModSettings> settings;
    std::unique_ptr<ApiRegistry> api;   // this mod's view of the game API (owner attribution, touches_gameplay)
    std::unique_ptr<ModContext> context;
    std::unique_ptr<ModRuntime> script, native;
    std::mutex mutex;   // held around every call into this mod's runtimes: the game thread and the menu thread both call in
    uint32_t failures = 0;
    uint32_t quiet_calls = 0;   // on_step calls since the last failure; a long quiet stretch clears the failure count
    bool disabled = false;
    std::string last_error;
    std::map<const void*, std::shared_ptr<bool>> hook_flags;   // per runtime: false once its hooks are dropped (a hook call already in flight sees it)
    std::unique_ptr<Hooks> hooks;
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
            if (!*alive || mod.disabled) return;
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

void ModHost::start() {
    if (!mods_.empty()) return;
    for (const ModEntry* entry : registry_.active()) {
        if (config_.include && !config_.include(*entry)) continue;
        auto mod = std::make_unique<Running>();
        mod->manifest = entry->manifest;
        mod->root = entry->root;
        const auto stored = config_.stored_settings ? config_.stored_settings(mod->manifest.id) : nlohmann::json();
        mod->settings = std::make_unique<ModSettings>(mod->manifest.settings, stored);
        for (const auto& warning : mod->settings->warnings()) if (config_.log) config_.log(mod->manifest.id, warning);
        mod->hooks = std::make_unique<Hooks>(*this, *mod);
        mod->api = std::make_unique<ApiRegistry>(api_.for_mod(mod->manifest.id, mod->manifest.touches_gameplay));
        mod->context = std::make_unique<ModContext>(ModContext{*mod->api, *mod->settings, mod->manifest.id,
            [this, id = mod->manifest.id](std::string_view line) { if (config_.log) config_.log(id, line); }, config_.ui, config_.memory,
            mod->manifest.touches_gameplay ? mod->hooks.get() : nullptr});   // a hook can change anything: only gameplay mods get them
        {
            std::lock_guard lock(mod->mutex);
            create_runtimes(*mod, true, true);
            if (!mod->disabled) emit(*mod, nullptr, "on_load", Value::object(), config_.ui_budget);
            if (!mod->disabled) {   // a mod that did not start does not change the game, by script or by data
                const auto patches = apply_data_patches(mod->root, *mod->api);   // after on_load, in load order: the last mod's value stays
                for (const auto& error : patches.errors) if (config_.log) config_.log(mod->manifest.id, "data patch: " + error);
            }
        }
        mods_.push_back(std::move(mod));
    }
}

void ModHost::create_runtimes(Running& mod, bool scripts, bool native) {
    const auto make = [&](RuntimeKind kind) -> std::unique_ptr<ModRuntime> {
        if (config_.runtime_factory) return config_.runtime_factory(kind, *mod.context);
        switch (kind) {
            case RuntimeKind::lua: return make_lua_runtime(*mod.context);
            case RuntimeKind::js: return make_js_runtime(*mod.context);
            case RuntimeKind::native: return make_native_runtime(*mod.context, config_.native);
        }
        return nullptr;
    };
    const auto start_one = [&](RuntimeKind kind, const std::string& entry, std::unique_ptr<ModRuntime>& slot) {
        slot = make(kind);
        const CallResult result = slot->load(mod.root, entry);
        if (!result.ok) {
            mod.disabled = true;
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
    if (mods_.empty()) return;
    for (const auto& tick : config_.ticks) tick();
    std::vector<std::pair<std::string, std::string>> changed;
    {
        std::lock_guard lock(pending_mutex_);
        changed.swap(pending_settings_);
    }
    for (const auto& [id, key] : changed) {
        for (auto& mod : mods_) {
            if (mod->manifest.id != id) continue;
            std::lock_guard lock(mod->mutex);
            if (!mod->disabled) emit(*mod, nullptr, "on_settings_changed", Value{{"key", key}}, config_.ui_budget);
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
            if (mod.disabled) continue;
            emit(mod, nullptr, "on_step", Value::object(), Budget{remaining / int64_t(count - i)});   // a fair share of what is left: an idle mod's share goes to the next
            if (!mod.disabled && ++mod.quiet_calls >= kQuietCallsToForgive && mod.failures) mod.failures = 0;   // record() resets quiet_calls on a failure
        }
    }
    rotation_ = (rotation_ + 1) % count;
}

void ModHost::fire_game_event(std::string_view event, const Value& payload) {
    for (auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        if (!mod->disabled) emit(*mod, nullptr, event, payload, config_.step_budget);
    }
}

void ModHost::settings_changed(std::string_view mod_id, std::string_view key) {
    std::lock_guard lock(pending_mutex_);
    pending_settings_.emplace_back(std::string(mod_id), std::string(key));
}

CallResult ModHost::draw_ui(std::string_view mod_id, const ImGuiHandles* imgui) {
    for (auto& mod : mods_) {
        if (mod->manifest.id != mod_id) continue;
        std::lock_guard lock(mod->mutex);
        if (mod->disabled) return {};
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
    for (auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        if (!mod->script) continue;
        emit(*mod, mod->script.get(), "on_unload", Value::object(), config_.ui_budget);
        drop_hooks(*mod, mod->script.get());
        dropped(*mod);
        mod->script.reset();
        if (!mod->native) { mod->disabled = false; mod->failures = 0; mod->last_error.clear(); }   // a native part keeps its failure state
        create_runtimes(*mod, true, false);
        if (mod->script && !mod->disabled) emit(*mod, mod->script.get(), "on_load", Value::object(), config_.ui_budget);
    }
}

void ModHost::shutdown() {
    for (auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        if (!mod->disabled) emit(*mod, nullptr, "on_unload", Value::object(), config_.ui_budget);
        drop_hooks(*mod, mod->script.get());
        drop_hooks(*mod, mod->native.get());
        dropped(*mod);
        mod->disabled = true;   // a hook call that is still in flight must not reach the runtimes below
        mod->script.reset();
        mod->native.reset();
    }
    mods_.clear();
}

std::vector<ModHost::ModStatus> ModHost::status() const {
    std::vector<ModStatus> result;
    for (const auto& mod : mods_) {
        std::lock_guard lock(mod->mutex);
        const bool ui = (mod->script && mod->script->has_ui()) || (mod->native && mod->native->has_ui());
        result.push_back({mod->manifest.id, mod->script || mod->native, mod->disabled, !mod->manifest.native.empty(), ui, !mod->manifest.settings.empty(), mod->last_error, mod->failures});
    }
    return result;
}

ModSettings* ModHost::settings_of(std::string_view mod_id) {
    for (auto& mod : mods_) if (mod->manifest.id == mod_id) return mod->settings.get();
    return nullptr;
}

std::vector<std::string> ModHost::ids() const {
    std::vector<std::string> result;
    for (const auto& mod : mods_) result.push_back(mod->manifest.id);
    return result;
}

bool ModHost::try_edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit) {
    for (auto& mod : mods_) {
        if (mod->manifest.id != mod_id) continue;
        std::unique_lock lock(mod->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        edit(*mod->settings);
        return true;
    }
    return false;
}

bool ModHost::edit_settings(std::string_view mod_id, const std::function<void(ModSettings&)>& edit) {
    for (auto& mod : mods_) {
        if (mod->manifest.id != mod_id) continue;
        std::lock_guard lock(mod->mutex);
        edit(*mod->settings);
        return true;
    }
    return false;
}

}  // namespace uc::mods
