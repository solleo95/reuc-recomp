#include "mod_test_rig.h"

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;
using rig::fresh;
using rig::kBudget;

namespace {
struct Rig : rig::Rig {
    explicit Rig(const fs::path& folder) : rig::Rig(folder, make_lua_runtime, ".lua") {}
};
}  // namespace

static void sandbox_missing_libs() {
    Rig rig(fresh(u8"lua_sandbox"));
    const auto r = rig.run("local gone = {} for _, n in ipairs({'io','os','package','debug','dofile','loadfile','load','loadstring','coroutine'}) do if _G[n] ~= nil then gone[#gone+1] = n end end\n"
                           "log.write('present:' .. table.concat(gone, ','))\n"
                           "log.write('dump:' .. tostring(string.dump))\n"
                           "log.write('gc:' .. tostring(collectgarbage('count') > 0) .. ' ' .. tostring(pcall(collectgarbage, 'collect')))\n"
                           "log.write('has:' .. tostring(type(string.format) == 'function' and type(table.sort) == 'function' and type(math.floor) == 'function' and type(utf8.char) == 'function'))");
    CHECK(r.ok);
    CHECK(rig.log.size() == 4 && rig.log[0] == "present:" && rig.log[1] == "dump:nil" && rig.log[2] == "gc:true false" && rig.log[3] == "has:true");
    const auto bad = rig.run("os.execute('calc')");
    CHECK(!bad.ok && bad.error.find("os") != std::string::npos);
}

static void require_confined() {
    const auto root = fresh(u8"lua_require");
    Rig rig(root);
    fs::create_directories(root / "scripts" / "lib");
    std::ofstream(root / "scripts" / "lib" / "util.lua") << "local M = {} function M.twice(x) return x * 2 end return M";
    std::ofstream(root / "secret.lua") << "return 'leak'";
    CHECK(rig.run("local u = require('lib.util') log.write(u.twice(21)) log.write(tostring(require('lib.util') == u))").ok);
    CHECK(rig.log[0] == "42" && rig.log[1] == "true");   // cached
    CHECK(!rig.run("require('../secret')").ok);
    CHECK(!rig.run("require('..secret')").ok);
    CHECK(!rig.run("require('C:/x')").ok);
    CHECK(!rig.run("require('lib/util')").ok);
    CHECK(!rig.run("require('missing.module')").ok);
}

static void non_ascii_mod_root() {
    const auto root = fresh(u8"lua_\u043c\u043e\u0434\u044b");
    Rig rig(root);
    fs::create_directories(root / "scripts");
    std::ofstream(root / "scripts" / "dep.lua") << "return 7";
    const auto r = rig.run("log.write(require('dep') + 1)");
    CHECK(r.ok && rig.log.size() == 1 && rig.log[0] == "8");
}

static void events() {
    Rig rig(fresh(u8"lua_events"));
    CHECK(rig.run("local n = 0\n on('on_step', function(p) n = n + 1 log.write('step ' .. n) end)\n on('on_step', function() log.write('second') end)\n on('on_load', function(p) log.write('load') end)").ok);
    CHECK(rig.log.empty());   // registering runs nothing
    CHECK(rig.runtime->fire("on_load", json::object(), kBudget).ok);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok);
    CHECK((rig.log == std::vector<std::string>{"load", "step 1", "second", "step 2", "second"}));
    CHECK(rig.runtime->fire("on_unknown_event", json::object(), kBudget).ok);   // nothing registered
    CHECK(rig.run("on('on_weapon_fired', function(p) log.write('slot ' .. p.slot .. ' ' .. tostring(p.flag)) end)").ok);
    CHECK(rig.runtime->fire("on_weapon_fired", json{{"slot", 2}, {"flag", true}}, kBudget).ok && rig.log.back() == "slot 2 true");
}

static void api_read_write() {
    Rig rig(fresh(u8"lua_api"));
    CHECK(rig.run("log.write(uc.mem.read_u32(21))\n"
                  "local p = uc.player(1)\n log.write(p.magazine)\n p.magazine = 7\n log.write(uc.player(2).magazine)\n"
                  "local v = uc.camera.view log.write(#v .. ':' .. v[2])").ok);
    CHECK((rig.log == std::vector<std::string>{"42", "115", "207", "3:2.5"}));
    CHECK(rig.last_set_args == json({{"slot", 1}}));
    CHECK(rig.magazine == 7);
}

static void api_error_is_lua_error() {
    Rig rig(fresh(u8"lua_error"));
    CHECK(rig.run("local ok, err = pcall(uc.mem.boom) log.write(tostring(ok)) log.write(err)").ok);
    CHECK(rig.log[0] == "false" && rig.log[1].find("outside guest memory") != std::string::npos);
    const auto uncaught = rig.run("uc.mem.boom()");
    CHECK(!uncaught.ok && uncaught.error.find("outside guest memory") != std::string::npos);
    CHECK(!rig.run("uc.mem.read_u32('x')").ok);   // a wrong argument type reaches the API as JSON and is its job to refuse; the runtime must not crash
    CHECK(!rig.run("uc.nothing.at_all()").ok);
}

static void settings_get() {
    Rig rig(fresh(u8"lua_settings"));
    CHECK(rig.run("log.write(settings.get('scale')) log.write(tostring(settings.get('nothere'))) log.write(tostring(settings.set('scale', 99))) log.write(settings.get('scale')) log.write(tostring(settings.set('scale', 'x')))").ok);
    CHECK((rig.log == std::vector<std::string>{"2.5", "nil", "true", "10.0", "false"}));
    CHECK(rig.settings.dirty());
}

static void budget_endless_loop() {
    Rig rig(fresh(u8"lua_budget"));
    CHECK(rig.run("on('on_step', function() while true do end end)").ok);
    const auto begin = std::chrono::steady_clock::now();
    const auto r = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    const auto took = std::chrono::steady_clock::now() - begin;
    CHECK(!r.ok && r.budget_exceeded);
    CHECK(took < std::chrono::milliseconds(200));
    CHECK(rig.runtime->fire("on_unknown", json::object(), kBudget).ok);   // the runtime is still usable
}

// pcall must not be a way around the time budget, and one long library call (a pathological pattern) must be stoppable too.
static void pcall_cannot_swallow_the_budget() {
    Rig rig(fresh(u8"lua_pcall"));
    CHECK(rig.run("on('on_step', function() while true do pcall(function() for i = 1, 1e9 do end end) end end)").ok);
    auto begin = std::chrono::steady_clock::now();
    const auto r = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    CHECK(!r.ok && r.budget_exceeded && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    // The state stays usable: the next call runs (and is stopped again) normally.
    begin = std::chrono::steady_clock::now();
    const auto again = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    CHECK(!again.ok && again.budget_exceeded && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    CHECK(rig.run("log.write('alive')").ok);
}

static void pattern_bomb_is_stopped() {
    Rig rig(fresh(u8"lua_pattern"));
    CHECK(rig.run("on('on_step', function() return string.find(string.rep('a', 5000), '.-.-.-.-.-.-.-.-b') end)").ok);
    const auto begin = std::chrono::steady_clock::now();
    const auto r = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(20)});
    CHECK(!r.ok && r.budget_exceeded && std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    Rig fine(fresh(u8"lua_pattern_ok"));   // ordinary pattern work is not slowed or refused
    CHECK(fine.run("on('on_step', function() log.write(string.gsub(string.rep('ab', 2000), 'a', 'c'):sub(1, 4)) end)").ok);
    CHECK(fine.runtime->fire("on_step", json::object(), kBudget).ok && fine.log == std::vector<std::string>{"cbcb"});
}

// A mod folder whose name is outside the system code page: the entry file and a required module are found.
static void non_code_page_mod_root() {
    const auto root = fresh(u8"lua_日本語");
    Rig rig(root);
    fs::create_directories(root / "scripts");
    std::ofstream(root / "scripts" / "dep.lua", std::ios::binary) << "return { seven = 7 }";
    const auto r = rig.run("log.write(require('dep').seven + 1)");
    CHECK(r.ok && rig.log.size() == 1 && rig.log[0] == "8");
}

// C loops that never poll the clock, and finalizers (which run with the count hook off), must not be a way to freeze the game thread.
static void other_ways_to_run_forever_are_closed() {
    Rig gc(fresh(u8"lua_gc"));
    const auto finalizer = gc.run("setmetatable({}, {__gc = function() while true do end end}) collectgarbage('collect')");   // collectgarbage('count') only is allowed
    CHECK(!finalizer.ok && finalizer.error.find("__gc") != std::string::npos);
    Rig rep(fresh(u8"lua_rep"));
    CHECK(rep.run("on('on_step', function() return string.rep('', math.maxinteger) end)").ok);
    auto begin = std::chrono::steady_clock::now();
    auto r = rep.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(20)});
    CHECK(r.ok && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));   // an empty result is an empty string, at once
    CHECK(rep.run("log.write(string.rep('ab', 3, '-') .. '|' .. string.rep('', 5, '') .. '|' .. string.rep('x', 0))").ok && rep.log.back() == "ab-ab-ab||");
    Rig move(fresh(u8"lua_move"));
    CHECK(move.run("on('on_step', function() return table.move({}, 1, math.maxinteger - 1, 1, {}) end)").ok);
    begin = std::chrono::steady_clock::now();
    r = move.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(20)});
    CHECK(!r.ok && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    CHECK(move.run("local t = table.move({1, 2, 3}, 1, 3, 2) log.write(table.concat(t, ','))").ok && move.log.back() == "1,1,2,3");   // ordinary use is unchanged
}

static void memory_cap() {
    Rig rig(fresh(u8"lua_memory"));
    const auto r = rig.run("local t = {} for i = 1, 1e9 do t[i] = ('x'):rep(1000) .. i end");
    CHECK(!r.ok && r.error.find("memory") != std::string::npos);
    CHECK(rig.run("log.write('still alive')").ok);
    const auto big = rig.run("local s = ('x'):rep(200 * 1024 * 1024)");
    CHECK(!big.ok);
}

static void syntax_error_reported() {
    Rig rig(fresh(u8"lua_syntax"));
    const auto r = rig.run("local x = = 1\n", "bad.lua");
    CHECK(!r.ok && r.error.find("bad.lua:1") != std::string::npos);
    const auto missing = rig.runtime->load(rig.root, "scripts/missing.lua");
    CHECK(!missing.ok && missing.error.find("missing.lua") != std::string::npos);
}

static void runtime_error_has_location() {
    Rig rig(fresh(u8"lua_location"));
    const auto r = rig.run("local a = nil\nlocal b = a.x\n", "oops.lua");
    CHECK(!r.ok && r.error.find("oops.lua:2") != std::string::npos);
}

static void ui_calls_forwarded() {
    Rig rig(fresh(u8"lua_ui"));
    CHECK(rig.run("ui.draw(function()\n ui.text('hello')\n if ui.button('Go') then log.write('clicked') end\n"
                  " log.write(tostring(ui.checkbox('c', false)))\n log.write(ui.slider_int('i', 4, 0, 9))\n log.write(ui.slider_float('f', 1.5, 0, 2))\n"
                  " log.write(ui.combo('co', 0, {'a','b'}))\n log.write(ui.input_text('t', 'x'))\n ui.separator() ui.same_line()\n"
                  " log.write(tostring(ui.collapsing_header('h')))\n ui.tooltip('tip')\n local c = ui.color_edit('col', {1, 1, 1, 1}) log.write(c[1])\nend)").ok);
    CHECK(rig.runtime->draw_ui(kBudget).ok);
    CHECK(rig.log.size() == 7 && rig.log[0] == "true" && rig.log[1] == "5" && rig.log[2] == "3.0" && rig.log[3] == "1" && rig.log[4] == "x!" && rig.log[5] == "true" && rig.log[6] == "0.5");
    CHECK(rig.ui.calls.front() == "text:hello" && rig.ui.calls[1] == "button:Go" && rig.ui.calls.back() == "color:col");
    rig.ui.click = true; rig.log.clear();
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.log[0] == "clicked");
    Rig none(fresh(u8"lua_ui_none"));
    CHECK(none.runtime->draw_ui(kBudget).ok);   // no draw function registered: nothing happens
    const auto outside = none.run("ui.text('x')");
    CHECK(!outside.ok);   // widgets only work inside the draw function
}

static void hook_pre_regs_round_trip() {
    Rig rig(fresh(u8"lua_hook"));
    CHECK(rig.run("log.write(table.concat(hook.list(), ','))\n"
                  "hook.pre('GameWeaponUpdate', function(regs)\n log.write(regs.r3 .. ',' .. regs.f1)\n regs.r4 = regs.r3 + 1\n regs.f2 = 2.5\n regs.r10 = 4294967295\nend)").ok);
    CHECK(rig.log == std::vector<std::string>{"GameWeaponUpdate"} && rig.hooks.callbacks.size() == 1 && rig.hooks.owners[0] != nullptr);
    HookRegs regs;
    regs.r[0] = 7; regs.f[0] = 1.5; regs.r[2] = 99;
    const auto result = rig.hooks.callbacks[0](regs, kBudget);
    CHECK(result.ok && rig.log.back() == "7,1.5");
    CHECK(regs.r[0] == 7 && regs.r[1] == 8 && regs.f[1] == 2.5 && regs.r[7] == 0xFFFFFFFFu && regs.r[2] == 99);   // untouched registers stay
    const auto unknown = rig.run("hook.pre('Nope', function() end)");
    CHECK(!unknown.ok && unknown.error.find("hookable") != std::string::npos);
    CHECK(!rig.run("hook.post('GameWeaponUpdate', function() end)").ok && !rig.run("hook.replace('GameWeaponUpdate', function() end)").ok);
    CHECK(rig.run("hook.pre('GameWeaponUpdate', function(regs) while true do end end)").ok);
    const auto loop = rig.hooks.callbacks.back()(regs, Budget{std::chrono::milliseconds(5)});
    CHECK(!loop.ok && loop.budget_exceeded);   // a hook is under the same budget as any callback
    CHECK(rig.run("hook.pre('GameWeaponUpdate', function(regs) error('hook failed') end)").ok);
    const auto thrown = rig.hooks.callbacks.back()(regs, kBudget);
    CHECK(!thrown.ok && thrown.error.find("hook failed") != std::string::npos);
}

int main() {
    other_ways_to_run_forever_are_closed(); non_code_page_mod_root(); pcall_cannot_swallow_the_budget(); pattern_bomb_is_stopped();
    hook_pre_regs_round_trip();
    sandbox_missing_libs(); require_confined(); non_ascii_mod_root(); events(); api_read_write(); api_error_is_lua_error(); settings_get(); budget_endless_loop();
    memory_cap(); syntax_error_reported(); runtime_error_has_location(); ui_calls_forwarded();
    std::puts("ok");
}
