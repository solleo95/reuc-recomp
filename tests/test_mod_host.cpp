#include "check.h"
#include "host.h"
#include "ppc_rt.h"
#include <atomic>
#include <fstream>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct Behavior {
    std::mutex m;
    std::vector<std::string> events;   // "load", "fire:<event>", "draw"
    int loads = 0, steps = 0;
    double spin_ms = 0;                // busy time per callback
    bool fail_load = false, fail_fire = false, fail_draw = false;
    bool install_hook = false, fail_hook = false;   // load() hooks GameWeaponUpdate; the hook copies r3+1 into r4
    int hook_calls = 0;
    ModContext* context = nullptr;               // the context the host gave the runtime
    std::thread::id settings_thread;             // the thread that delivered on_settings_changed
    std::chrono::microseconds last_step_budget{0};   // the budget the last on_step call was given
    std::atomic<int> inside{0}, overlaps{0};
    void note(std::string e) { std::lock_guard lock(m); events.push_back(std::move(e)); }
    int count(const std::string& e) { std::lock_guard lock(m); return int(std::count(events.begin(), events.end(), e)); }
};

void spin(double ms) {
    const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
    while (Clock::now() < until) {}
}

class FakeRuntime : public ModRuntime {
public:
    FakeRuntime(std::shared_ptr<Behavior> b, ModContext& context) : b_(std::move(b)), ctx_(context) { b_->context = &context; }
    std::string name() const override { return "fake"; }
    CallResult load(const fs::path&, const std::string&) override {
        ++b_->loads;
        b_->note("load");
        if (b_->install_hook) {
            auto b = b_;
            CHECK(ctx_.hooks->add_pre(this, "GameWeaponUpdate", [b](HookRegs& regs, Budget) -> CallResult {
                ++b->hook_calls;
                if (b->fail_hook) return {false, "hook boom", false};
                regs.r[1] = regs.r[0] + 1;
                return {};
            }));
        }
        return b_->fail_load ? CallResult{false, "cannot start", false} : CallResult{};
    }
    CallResult fire(std::string_view event, const Value& payload, Budget budget) override {
        Guard guard(*b_);
        b_->note("fire:" + std::string(event) + (event == "on_settings_changed" ? ":" + payload["key"].get<std::string>() : ""));
        if (event == "on_step") { ++b_->steps; b_->last_step_budget = budget.limit; }
        if (event == "on_settings_changed") b_->settings_thread = std::this_thread::get_id();
        const auto begin = Clock::now();
        spin(b_->spin_ms);
        if (b_->fail_fire) return {false, "boom", false};
        if (Clock::now() - begin > budget.limit) return {false, "over budget", true};
        return {};
    }
    CallResult draw_ui(Budget) override { Guard guard(*b_); b_->note("draw"); spin(b_->spin_ms); return b_->fail_draw ? CallResult{false, "draw failed", false} : CallResult{}; }
private:
    struct Guard {
        Behavior& b;
        explicit Guard(Behavior& bb) : b(bb) { if (b.inside.fetch_add(1) != 0) ++b.overlaps; }
        ~Guard() { b.inside.fetch_sub(1); }
    };
    std::shared_ptr<Behavior> b_;
    ModContext& ctx_;
};

fs::path fresh(const char* name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

void add_mod(const fs::path& dir, const std::string& id, const std::string& extra) {
    fs::create_directories(dir / id);
    std::ofstream(dir / id / "mod.json") << R"({"id":")" << id << R"(","name":")" << id << R"(","version":"1","api":"1.0",)" << extra << "}";
}

struct Setup {
    ModRegistry registry;
    ApiRegistry api;
    std::map<std::string, std::shared_ptr<Behavior>> behaviors;   // "<id>/<kind>"
    ModHostConfig config;
    std::unique_ptr<ModHost> host;
    int created = 0;
    std::vector<RuntimeKind> kinds;   // in creation order

    Setup(const fs::path& mods_dir, std::initializer_list<const char*> enabled) : registry(make_registry(mods_dir, enabled)) {
        config.step_budget = {std::chrono::milliseconds(1)};
        config.native.allow_native = true;
        config.native.confirmed = [](const std::string&, const std::string&) { return true; };
        config.runtime_factory = [this](RuntimeKind kind, ModContext& context) {
            ++created;
            kinds.push_back(kind);
            return std::unique_ptr<ModRuntime>(new FakeRuntime(behavior(context.mod_id, kind), context));
        };
    }
    void make_host() { host = std::make_unique<ModHost>(registry, api, config); }
    std::shared_ptr<Behavior> behavior(const std::string& id, RuntimeKind kind) {
        auto& slot = behaviors[id + "/" + std::to_string(int(kind))];
        if (!slot) slot = std::make_shared<Behavior>();
        return slot;
    }
    Behavior& script(const std::string& id) { return *behavior(id, RuntimeKind::lua); }
    static ModRegistry make_registry(const fs::path& dir, std::initializer_list<const char*> enabled) {
        json section = {{"enabled", json::array()}};
        for (const char* id : enabled) section["enabled"].push_back(id);
        return ModRegistry::discover(dir, section, {1, 0}, "RBUE08");
    }
    const ModHost::ModStatus* status(const std::string& id) {
        static std::vector<ModHost::ModStatus> keep;
        keep = host->status();
        for (const auto& s : keep) if (s.id == id) return &s;
        return nullptr;
    }
};
}  // namespace

static void start_creates_runtime_per_mod_and_fires_on_load() {
    const auto dir = fresh("host_start");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    add_mod(dir, "b", R"("script":"scripts/main.js","native":"native/b.dll")");
    add_mod(dir, "c", R"("script":"scripts/main.lua")");   // not enabled
    Setup s(dir, {"a", "b"});
    s.make_host();
    s.host->start();
    CHECK(s.created == 3);   // a: lua; b: js + native
    CHECK((s.script("a").events == std::vector<std::string>{"load", "fire:on_load"}));
    CHECK(s.behavior("b", RuntimeKind::js)->events.size() == 2 && s.behavior("b", RuntimeKind::native)->events.size() == 2);
    CHECK(s.status("c") && !s.status("c")->running && s.status("a")->running && s.status("b")->has_native);   // c is known but switched off
}

static void step_fires_each_mod_once() {
    const auto dir = fresh("host_step");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    add_mod(dir, "b", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a", "b"});
    s.make_host();
    s.host->start();
    s.host->step(); s.host->step();
    CHECK(s.script("a").count("fire:on_step") == 2 && s.script("b").count("fire:on_step") == 2);
}

// on_step is the game's nominal rate: a step that is shorter than a nominal frame runs the per-step logic (ticks) but not the handlers, a longer one
// runs the handlers once per nominal frame it covers (at most 4).
static void on_step_follows_nominal_frames() {
    const auto dir = fresh("host_nominal");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    int ticks = 0;
    s.config.ticks.push_back([&] { ++ticks; });
    s.make_host();
    s.host->start();
    s.host->step(0);
    CHECK(ticks == 1 && s.script("a").steps == 0);
    s.host->step(1);
    s.host->step(3);
    CHECK(ticks == 3 && s.script("a").steps == 4);
    s.host->step(100);
    CHECK(s.script("a").steps == 8);
}

// The 1 ms is for all mods together: mods that stay inside it as a group are never failed, however many there are and wherever they sit in the rotation.
static void mods_within_the_total_budget_are_never_failed() {
    const auto dir = fresh("host_total_ok");
    for (const char* id : {"a", "b", "c", "d"}) add_mod(dir, id, R"("script":"scripts/main.lua")");
    Setup s(dir, {"a", "b", "c", "d"});
    s.config.step_budget = {std::chrono::milliseconds(4)};   // slack for timer noise: the four mods use 0.2 ms each, 0.8 ms of the 4
    s.make_host();
    s.host->start();
    for (const char* id : {"a", "b", "c", "d"}) s.script(id).spin_ms = 0.2;
    for (int i = 0; i < 40; ++i) s.host->step();
    for (const char* id : {"a", "b", "c", "d"}) CHECK(s.status(id)->failures == 0 && s.script(id).steps == 40);
    CHECK(s.script("a").last_step_budget > std::chrono::microseconds(2500));   // each call is given what is left of the step, not a slice of it
}

// Failures are a count of recent trouble: a mod that behaved for a minute of game time starts over with a clean slate.
static void failures_decay_after_a_quiet_minute() {
    const auto dir = fresh("host_decay");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    s.make_host();
    s.host->start();
    s.script("a").fail_fire = true;
    s.host->step(); s.host->step();
    s.script("a").fail_fire = false;
    CHECK(s.status("a")->failures == 2 && !s.status("a")->disabled_for_session);
    for (int i = 0; i < 1800; ++i) s.host->step();
    CHECK(s.status("a")->failures == 0);
    s.script("a").fail_fire = true;
    s.host->step(); s.host->step();
    CHECK(!s.status("a")->disabled_for_session);   // two more would have been the third and fourth failure without the decay
}

// The manifest accepts ".LUA" (it lower-cases the extension); the host must pick the Lua runtime for it too.
static void script_extension_is_case_insensitive() {
    const auto dir = fresh("host_ext");
    add_mod(dir, "a", R"("script":"scripts/MAIN.LUA")");
    add_mod(dir, "b", R"("script":"scripts/Main.Js")");
    Setup s(dir, {"a", "b"});
    s.make_host();
    s.host->start();
    CHECK(s.kinds == std::vector<RuntimeKind>({RuntimeKind::lua, RuntimeKind::js}));
}

// A mod whose script did not start is switched off, and so are its data patches.
static void failed_mod_does_not_patch() {
    const auto dir = fresh("host_patch_failed");
    add_mod(dir, "a", R"("touches_gameplay":true,"script":"scripts/main.lua")");
    fs::create_directories(dir / "a" / "data");
    std::ofstream(dir / "a" / "data" / "w.json") << R"({"weapon.0.damage": 1})";
    Setup s(dir, {"a"});
    int writes = 0;
    Member damage;
    damage.path = "weapon.damage"; damage.scope = "id";
    damage.set = [&](const json&, const json&) { ++writes; };
    s.api.add(std::move(damage));
    s.script("a").fail_load = true;
    s.make_host();
    s.host->start();
    CHECK(s.status("a")->disabled_for_session && writes == 0);
}

// The menu thread must never wait for a mod that is busy: it asks with try_edit_settings and tries again next frame.
static void try_edit_settings_does_not_wait() {
    const auto dir = fresh("host_try");
    add_mod(dir, "a", R"("script":"scripts/main.lua","settings":[{"key":"scale","type":"number","default":1.0,"min":0,"max":10,"label":{"en":"S"}}])");
    Setup s(dir, {"a"});
    s.config.ui_budget = {std::chrono::seconds(1)};
    s.make_host();
    s.host->start();
    CHECK(s.host->ids() == std::vector<std::string>{"a"});
    bool ran = false;
    CHECK(s.host->try_edit_settings("a", [&](ModSettings&) { ran = true; }) && ran);
    s.script("a").spin_ms = 300;
    std::thread busy([&] { s.host->draw_ui("a"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ran = false;
    const auto begin = Clock::now();
    CHECK(!s.host->try_edit_settings("a", [&](ModSettings&) { ran = true; }) && !ran && Clock::now() - begin < std::chrono::milliseconds(100));
    busy.join();
    CHECK(s.host->try_edit_settings("a", [&](ModSettings&) { ran = true; }) && ran);
}

// Mods are switched on and off while the game runs: on_load / on_unload at the right moments, what a mod set is taken back, and the state is what a fresh
// start in the new order would give (a change restarts the mods after it).
static void mods_switch_on_and_off_live() {
    hook_registry().clear();
    const auto dir = fresh("host_live");
    for (const char* id : {"a", "b", "c"}) add_mod(dir, id, R"("touches_gameplay":true,"script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    std::vector<std::string> dropped;
    s.config.on_mod_dropped = [&](const std::string& id) { dropped.push_back(id); };
    s.make_host();
    s.host->start();
    CHECK(s.host->running_count() == 1 && s.script("b").loads == 0 && s.host->ids().size() == 3);
    s.registry.set_enabled("b", true);                                   // b on: only b starts (it is last in load order)
    s.host->sync();
    CHECK(s.host->running_count() == 2 && s.script("a").loads == 1 && s.script("b").loads == 1 && s.script("b").count("fire:on_load") == 1);
    s.host->step();
    CHECK(s.script("a").steps == 1 && s.script("b").steps == 1 && s.script("c").steps == 0);
    s.registry.set_enabled("a", false);                                  // a off: it is told, what it set is taken back, b restarts (it came after a)
    s.host->sync();
    CHECK(s.script("a").count("fire:on_unload") == 1 && !s.status("a")->running && s.status("b")->running);
    CHECK(s.script("b").loads == 2 && s.script("b").count("fire:on_unload") == 1);
    CHECK(std::count(dropped.begin(), dropped.end(), "a") == 1 && std::count(dropped.begin(), dropped.end(), "b") == 1);
    s.registry.set_enabled("c", true);                                   // c on after b: nothing before it restarts
    s.host->sync();
    CHECK(s.script("b").loads == 2 && s.script("c").loads == 1 && s.host->running_count() == 2);
    s.registry.move("c", -1);                                            // c before b: the load order changed, both restart in the new order
    s.host->sync();
    CHECK(s.script("c").loads == 2 && s.script("b").loads == 3);
    s.registry.set_enabled("b", false); s.registry.set_enabled("c", false);
    s.host->sync();
    const int steps_before = s.script("a").steps;
    CHECK(s.host->running_count() == 0);
    s.host->step();                                                      // nothing is on: nothing runs
    CHECK(s.script("a").steps == steps_before && s.script("b").steps == 1);
    s.registry.set_enabled("a", true);                                   // and a mod that was off starts clean
    s.host->sync();
    CHECK(s.script("a").loads == 2 && s.status("a")->running && s.status("a")->failures == 0);
}

// A mod switched off while it was failing starts clean the next time; hooks of a stopped mod are gone and its data patches are taken back.
static void switching_off_clears_failures_and_hooks() {
    hook_registry().clear();
    const auto dir = fresh("host_live_hooks");
    add_mod(dir, "a", R"("touches_gameplay":true,"script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    s.script("a").install_hook = true;
    s.make_host();
    s.host->start();
    CHECK(hook_registry().count(0x8006C268u) == 1);
    s.script("a").fail_fire = true;
    for (int i = 0; i < 3; ++i) s.host->step();
    CHECK(s.status("a")->disabled_for_session);
    s.registry.set_enabled("a", false);
    s.host->sync();
    CHECK(hook_registry().count(0x8006C268u) == 0 && !s.status("a")->running && !s.status("a")->disabled_for_session);
    s.script("a").fail_fire = false;
    s.registry.set_enabled("a", true);
    s.host->sync();
    CHECK(s.status("a")->running && !s.status("a")->disabled_for_session && s.status("a")->failures == 0 && hook_registry().count(0x8006C268u) == 1);
}

// Reloading the scripts puts the mod's data patches back (the reload took back everything the mod had set).
static void reload_applies_data_patches_again() {
    const auto dir = fresh("host_reload_patch");
    add_mod(dir, "a", R"("touches_gameplay":true,"script":"scripts/main.lua")");
    fs::create_directories(dir / "a" / "data");
    std::ofstream(dir / "a" / "data" / "w.json") << R"({"weapon.0.damage": 7})";
    Setup s(dir, {"a"});
    int writes = 0;
    Member damage;
    damage.path = "weapon.damage"; damage.scope = "id";
    damage.set = [&](const json&, const json&) { ++writes; };
    s.api.add(std::move(damage));
    s.make_host();
    s.host->start();
    CHECK(writes == 1);
    s.host->reload_scripts();
    CHECK(writes == 2);
}

// Settings changes queue up once per (mod, key) while nothing steps (a paused game).
static void settings_changes_are_deduplicated() {
    const auto dir = fresh("host_dedupe");
    add_mod(dir, "a", R"("script":"scripts/main.lua","settings":[{"key":"scale","type":"number","default":1.0,"min":0,"max":10,"label":{"en":"S"}}])");
    Setup s(dir, {"a"});
    s.make_host();
    s.host->start();
    for (int i = 0; i < 50; ++i) s.host->settings_changed("a", "scale");
    s.host->step();
    CHECK(s.script("a").count("fire:on_settings_changed:scale") == 1);
}

// A mod that stays inside its own share is not failed for a slow mod ahead of it eating the step.
static void innocent_mod_is_not_failed_for_a_slow_neighbour() {
    const auto dir = fresh("host_innocent");
    for (const char* id : {"a", "b"}) add_mod(dir, id, R"("script":"scripts/main.lua")");
    Setup s(dir, {"a", "b"});
    s.config.step_budget = {std::chrono::milliseconds(4)};
    s.config.max_failures = 100;
    s.make_host();
    s.host->start();
    s.script("a").spin_ms = 3.8;   // nearly all of the step
    s.script("b").spin_ms = 0.3;   // well inside its half (2 ms) but only 0.2 ms are left when a runs first
    for (int i = 0; i < 20; ++i) s.host->step();
    CHECK(s.status("b")->failures == 0);
}

static void step_total_budget() {
    const auto dir = fresh("host_budget");
    for (const char* id : {"a", "b", "c"}) add_mod(dir, id, R"("script":"scripts/main.lua")");
    Setup s(dir, {"a", "b", "c"});
    s.config.max_failures = 100;
    s.make_host();
    s.host->start();
    for (const char* id : {"a", "b", "c"}) s.script(id).spin_ms = 0.8;
    s.host->step();   // a runs (0.8 ms of the 1 ms), b gets the 0.2 ms left and overruns, c never starts
    CHECK(s.script("a").steps == 1 && s.script("b").steps == 1 && s.script("c").steps == 0);
    CHECK(s.status("a")->failures == 0 && s.status("b")->failures == 1 && s.status("c")->failures == 0);
    for (int i = 0; i < 6; ++i) s.host->step();   // the starting mod rotates, nobody starves
    CHECK(s.script("c").steps > 0 && s.script("a").steps > 1);
}

static void three_failures_disable() {
    const auto dir = fresh("host_fail");
    add_mod(dir, "bad", R"("script":"scripts/main.lua")");
    add_mod(dir, "good", R"("script":"scripts/main.lua")");
    Setup s(dir, {"bad", "good"});
    s.make_host();
    s.host->start();
    s.script("bad").fail_fire = true;
    for (int i = 0; i < 3; ++i) s.host->step();
    CHECK(s.status("bad")->disabled_for_session && s.status("bad")->failures == 3 && s.status("bad")->last_error.find("boom") != std::string::npos);
    s.host->step();
    CHECK(s.script("bad").steps == 3 && s.script("good").steps == 4);   // no fourth call; the other mod is unaffected
    CHECK(!s.status("good")->disabled_for_session);
}

static void load_failure_disables() {
    const auto dir = fresh("host_loadfail");
    add_mod(dir, "bad", R"("script":"scripts/main.lua")");
    Setup s(dir, {"bad"});
    s.behavior("bad", RuntimeKind::lua)->fail_load = true;
    s.make_host();
    s.host->start();
    CHECK(s.status("bad")->disabled_for_session && s.status("bad")->last_error.find("cannot start") != std::string::npos);
    s.host->step();
    CHECK(s.script("bad").steps == 0 && s.script("bad").count("fire:on_load") == 0);
}

static void reload_scripts_restarts_script_mods_only() {
    const auto dir = fresh("host_reload");
    add_mod(dir, "a", R"("script":"scripts/main.lua","native":"native/a.dll")");
    Setup s(dir, {"a"});
    s.make_host();
    s.host->start();
    CHECK(s.script("a").loads == 1 && s.behavior("a", RuntimeKind::native)->loads == 1);
    s.host->reload_scripts();
    CHECK(s.script("a").loads == 2 && s.behavior("a", RuntimeKind::native)->loads == 1);
    CHECK(s.script("a").count("fire:on_unload") == 1 && s.script("a").count("fire:on_load") == 2);
    CHECK(s.behavior("a", RuntimeKind::native)->count("fire:on_unload") == 0);
}

static void concurrent_calls_serialised() {
    const auto dir = fresh("host_threads");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    s.config.step_budget = {std::chrono::seconds(1)};
    s.config.ui_budget = {std::chrono::seconds(1)};
    s.config.max_failures = 1000;
    s.make_host();
    s.host->start();
    s.script("a").spin_ms = 2;
    std::thread ui([&] { for (int i = 0; i < 20; ++i) s.host->draw_ui("a"); });
    for (int i = 0; i < 20; ++i) s.host->step();
    ui.join();
    CHECK(s.script("a").overlaps == 0 && s.script("a").steps == 20 && s.script("a").count("draw") == 20);
}

static void no_mods_no_cost() {
    const auto dir = fresh("host_none");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {});   // nothing enabled
    int ticks = 0;
    s.config.ticks.push_back([&] { ++ticks; });
    s.make_host();
    s.host->start();
    s.host->step();
    CHECK(s.created == 0 && s.host->running_count() == 0 && !s.status("a")->running && ticks == 0);   // nothing is on: not even the game API ticks run
    CHECK(s.host->draw_ui("a").ok);
    s.host->shutdown();
}

static void ticks_run_before_mods() {
    const auto dir = fresh("host_ticks");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    std::vector<std::string> order;
    s.config.ticks.push_back([&] { order.push_back("tick"); });
    s.make_host();
    s.host->start();
    s.host->step();
    CHECK(s.script("a").events.back() == "fire:on_step" && order == std::vector<std::string>{"tick"});
}

static void draw_ui_counts_failures() {
    const auto dir = fresh("host_draw");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    s.make_host();
    s.host->start();
    s.script("a").fail_draw = true;
    for (int i = 0; i < 3; ++i) CHECK(!s.host->draw_ui("a").ok);
    CHECK(s.status("a")->disabled_for_session);
    CHECK(s.host->draw_ui("a").ok && s.script("a").count("draw") == 3);   // switched off: not called again
}

static void settings_events_and_stored_values() {
    const auto dir = fresh("host_settings");
    add_mod(dir, "a", R"("script":"scripts/main.lua","settings":[{"key":"scale","type":"number","default":1.0,"min":0,"max":10,"label":{"en":"S"}}])");
    Setup s(dir, {"a"});
    s.config.stored_settings = [](const std::string& id) { return id == "a" ? json{{"scale", 7}} : json(); };
    s.make_host();
    s.host->start();
    CHECK(s.host->settings_of("a") && s.host->settings_of("a")->get("scale") == 7 && !s.host->settings_of("zzz"));
    // The menu thread reports the change; the script hears about it on the game thread, at the start of the next step.
    std::thread menu([&] { s.host->settings_changed("a", "scale"); });
    menu.join();
    CHECK(s.script("a").count("fire:on_settings_changed:scale") == 0);
    s.host->step();
    CHECK(s.script("a").count("fire:on_settings_changed:scale") == 1 && s.script("a").settings_thread == std::this_thread::get_id());
    s.host->step();
    CHECK(s.script("a").count("fire:on_settings_changed:scale") == 1);   // delivered once
}

// Setters, memory writes and hooks need "touches_gameplay" in the manifest; every call runs under the mod's owner name.
static void gameplay_flag_gates_writes_and_hooks() {
    const auto dir = fresh("host_gameplay_flag");
    add_mod(dir, "plain", R"("script":"scripts/main.lua")");
    add_mod(dir, "game", R"("script":"scripts/main.lua","touches_gameplay":true)");
    Setup s(dir, {"plain", "game"});
    int writes = 0;
    std::string owner;
    Member damage;
    damage.path = "weapon.damage"; damage.scope = "id";
    damage.get = [](const json&) { return json(1); };
    damage.set = [&](const json&, const json&) { ++writes; owner = OwnerScope::current(); };
    s.api.add(std::move(damage));
    Member write;
    write.path = "mem.write_u32"; write.writes = true;
    write.call = [&](const json&) { ++writes; owner = OwnerScope::current(); return json(); };
    s.api.add(std::move(write));
    Member read;
    read.path = "mem.read_u32";
    read.call = [](const json&) { return json(7); };
    s.api.add(std::move(read));
    s.make_host();
    s.host->start();
    ModContext& plain = *s.script("plain").context;
    ModContext& game = *s.script("game").context;
    const auto refused = [](const std::function<void()>& f) {
        try { f(); } catch (const ApiError& e) { return std::string(e.what()).find("touches_gameplay") != std::string::npos; }
        return false;
    };
    CHECK(refused([&] { plain.api.find("weapon.damage")->set(json::object(), 5); }));
    CHECK(refused([&] { plain.api.find("mem.write_u32")->call(json::object()); }));
    CHECK(writes == 0 && plain.api.find("weapon.damage")->get(json::object()) == 1 && plain.api.find("mem.read_u32")->call(json::object()) == 7);
    CHECK(plain.hooks == nullptr && game.hooks != nullptr);
    game.api.find("weapon.damage")->set(json::object(), 5);
    CHECK(writes == 1 && owner == "game");
    game.api.find("mem.write_u32")->call(json::object());
    CHECK(writes == 2 && owner == "game" && OwnerScope::current().empty());   // the owner is only set during the call
}

// A mod that is switched off, reloaded or shut down must not leave its effects in the game.
static void dropped_mods_are_reported() {
    hook_registry().clear();
    const auto dir = fresh("host_dropped");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    add_mod(dir, "b", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a", "b"});
    std::vector<std::string> dropped;
    s.config.on_mod_dropped = [&](const std::string& id) { dropped.push_back(id); };
    s.make_host();
    s.host->start();
    CHECK(dropped.empty());
    s.script("a").fail_fire = true;
    for (int i = 0; i < 3; ++i) s.host->step();
    CHECK(s.status("a")->disabled_for_session && dropped == std::vector<std::string>{"a"});   // dropped once, when it was switched off
    s.host->reload_scripts();
    CHECK((dropped == std::vector<std::string>{"a", "a", "b"}));
    dropped.clear();
    s.host->shutdown();
    CHECK((dropped == std::vector<std::string>{"b", "a"}));   // stopped in reverse start order: a mod can rely on the ones before it until the end
}

// A mod with only data files (no script, no DLL) still starts: its patches go through the API in load order, the later mod wins, and a bad key
// is logged without switching the mod off.
static void data_patches_apply_in_load_order() {
    const auto dir = fresh("host_patch");
    add_mod(dir, "a", R"("touches_gameplay":true)");
    add_mod(dir, "b", R"("touches_gameplay":true)");
    fs::create_directories(dir / "a" / "data"); fs::create_directories(dir / "b" / "data");
    std::ofstream(dir / "a" / "data" / "w.json") << R"({"weapon.0.damage": 1, "weapon.0.nope": 5})";
    std::ofstream(dir / "b" / "data" / "w.json") << R"({"weapon.0.damage": 2})";
    Setup s(dir, {"a", "b"});
    std::vector<int> values;
    Member damage;
    damage.path = "weapon.damage";
    damage.scope = "id";
    damage.set = [&](const json&, const json& v) { values.push_back(v.get<int>()); };
    s.api.add(std::move(damage));
    std::vector<std::string> log;
    s.config.log = [&](std::string_view id, std::string_view line) { log.push_back(std::string(id) + ": " + std::string(line)); };
    s.make_host();
    s.host->start();
    CHECK((values == std::vector<int>{1, 2}));   // a first, b last: the last value stays
    CHECK(!s.status("a")->disabled_for_session && !s.status("b")->disabled_for_session && s.created == 0);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("a: ") == 0 && l.find("weapon.0.nope") != std::string::npos; }));
}

static void shutdown_fires_unload_once() {
    const auto dir = fresh("host_shutdown");
    add_mod(dir, "a", R"("script":"scripts/main.lua")");
    Setup s(dir, {"a"});
    s.make_host();
    s.host->start();
    s.host->shutdown();
    s.host->shutdown();
    CHECK(s.script("a").count("fire:on_unload") == 1 && s.host->empty());
    s.host->step();   // harmless after shutdown
}

static ppc::Ctx hook_ctx(uint32_t r3) {
    static ppc::Ctx c;
    c = {};
    c.r[3] = r3;
    return c;
}

static void hook_through_host() {
    hook_registry().clear();
    const auto dir = fresh("host_hook");
    add_mod(dir, "a", R"("script":"scripts/main.lua","touches_gameplay":true)");
    add_mod(dir, "b", R"("script":"scripts/main.lua","touches_gameplay":true)");
    Setup s(dir, {"a", "b"});
    s.script("a").install_hook = true;
    s.make_host();
    s.host->start();
    ppc::Ctx c = hook_ctx(7);
    hook_enter<0x8006C268u>(c);
    CHECK(s.script("a").hook_calls == 1 && c.r[4] == 8);
    s.script("a").fail_hook = true;
    for (int i = 0; i < 3; ++i) { c = hook_ctx(7); hook_enter<0x8006C268u>(c); CHECK(c.r[4] == 0); }   // a failed hook changes nothing
    CHECK(s.script("a").hook_calls == 4 && s.status("a")->disabled_for_session && s.status("a")->failures == 3);
    c = hook_ctx(7);
    hook_enter<0x8006C268u>(c);
    CHECK(s.script("a").hook_calls == 4);   // switched off: the hook point no longer calls in
    s.host->shutdown();
    CHECK(hook_registry().count(0x8006C268u) == 0);
}

static void hooks_follow_script_reloads_and_shutdown() {
    hook_registry().clear();
    const auto dir = fresh("host_hook_reload");
    add_mod(dir, "a", R"("script":"scripts/main.lua","touches_gameplay":true)");
    Setup s(dir, {"a"});
    s.script("a").install_hook = true;
    s.make_host();
    s.host->start();
    CHECK(hook_registry().count(0x8006C268u) == 1);
    s.host->reload_scripts();
    CHECK(hook_registry().count(0x8006C268u) == 1 && s.script("a").loads == 2);   // the old hook was dropped, the new script installed its own
    ppc::Ctx c = hook_ctx(1);
    hook_enter<0x8006C268u>(c);
    CHECK(s.script("a").hook_calls == 1 && c.r[4] == 2);
    s.host->shutdown();
    CHECK(hook_registry().count(0x8006C268u) == 0);
    c = hook_ctx(1);
    hook_enter<0x8006C268u>(c);
    CHECK(s.script("a").hook_calls == 1);
}

int main() {
    hook_through_host(); hooks_follow_script_reloads_and_shutdown();
    start_creates_runtime_per_mod_and_fires_on_load(); step_fires_each_mod_once(); step_total_budget(); three_failures_disable(); load_failure_disables();
    reload_scripts_restarts_script_mods_only(); concurrent_calls_serialised(); no_mods_no_cost(); ticks_run_before_mods(); draw_ui_counts_failures();
    settings_events_and_stored_values(); data_patches_apply_in_load_order(); shutdown_fires_unload_once();
    gameplay_flag_gates_writes_and_hooks(); dropped_mods_are_reported(); on_step_follows_nominal_frames();
    innocent_mod_is_not_failed_for_a_slow_neighbour(); mods_switch_on_and_off_live(); switching_off_clears_failures_and_hooks(); reload_applies_data_patches_again(); settings_changes_are_deduplicated();
    mods_within_the_total_budget_are_never_failed(); failures_decay_after_a_quiet_minute(); script_extension_is_case_insensitive(); failed_mod_does_not_patch(); try_edit_settings_does_not_wait();
    std::puts("ok");
}
