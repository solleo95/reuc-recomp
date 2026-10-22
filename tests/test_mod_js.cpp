#include "mod_test_rig.h"
#include <thread>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;
using rig::fresh;
using rig::kBudget;

namespace {
struct Rig : rig::Rig {
    explicit Rig(const fs::path& folder) : rig::Rig(folder, make_js_runtime, ".js") {}
};
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
}  // namespace

static void no_std_os() {
    Rig rig(fresh(u8"js_sandbox"));
    const auto r = rig.run("const names = ['std','os','require','process','fetch','XMLHttpRequest','setTimeout','importScripts','Deno','Bun','scriptArgs'];\n"
                           "log.write('present:' + names.filter(n => typeof globalThis[n] !== 'undefined').join(','));\n"
                           "log.write('has:' + [typeof uc, typeof log, typeof settings, typeof on, typeof ui, typeof JSON, typeof Math.floor].join(','));\n"
                           "log.write('eval:' + eval('1 + 2'));");
    CHECK(r.ok);
    CHECK(rig.log.size() == 3 && rig.log[0] == "present:" && rig.log[1] == "has:object,object,object,function,object,object,function" && rig.log[2] == "eval:3");
}

static void import_confined() {
    const auto root = fresh(u8"js_import");
    Rig rig(root);
    write(root / "scripts" / "lib" / "util.js", "export function twice(x) { return x * 2; }\nexport const name = 'util';");
    write(root / "scripts" / "lib" / "deeper.js", "import { twice } from './util.js';\nexport const four = twice(2);");
    write(root / "secret.js", "export const leak = 1;");
    write(root.parent_path() / "outside_js_import.js", "export const leak = 2;");
    CHECK(rig.run("import { twice } from './lib/util.js';\nimport { four } from './lib/deeper.js';\nlog.write(twice(21) + ':' + four);").ok);
    CHECK(rig.log.size() == 1 && rig.log[0] == "42:4");
    CHECK(rig.run("import { leak } from '../secret.js'; log.write('inside the mod folder is fine: ' + leak);").ok);   // up one level stays in the mod folder
    for (const char* bad : {"../../outside_js_import.js", "../../../x.js", "/etc/x.js", "C:/x.js", "https://example.com/x.js", "file:///c:/x.js", "bare", "./missing.js", ".\\\\lib\\\\util.js"})
        CHECK(!rig.run(std::string("import '") + bad + "';\n").ok);
    CHECK(!rig.run("const m = await import('../../outside_js_import.js');").ok);   // dynamic import goes through the same loader
}

static void non_ascii_mod_root() {
    const auto root = fresh(u8"js_\u043c\u043e\u0434\u044b");
    Rig rig(root);
    write(root / "scripts" / "dep.js", "export const seven = 7;");
    const auto r = rig.run("import { seven } from './dep.js'; log.write(seven + 1);");
    CHECK(r.ok && rig.log.size() == 1 && rig.log[0] == "8");
}

static void events() {
    Rig rig(fresh(u8"js_events"));
    CHECK(rig.run("let n = 0;\non('on_step', () => { n++; log.write('step ' + n); });\non('on_step', () => log.write('second'));\non('on_load', () => log.write('load'));").ok);
    CHECK(rig.log.empty());
    CHECK(rig.runtime->fire("on_load", json::object(), kBudget).ok);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok);
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok);
    CHECK((rig.log == std::vector<std::string>{"load", "step 1", "second", "step 2", "second"}));
    CHECK(rig.runtime->fire("on_unknown_event", json::object(), kBudget).ok);
    CHECK(rig.run("on('on_weapon_fired', p => log.write('slot ' + p.slot + ' ' + p.flag));").ok);
    CHECK(rig.runtime->fire("on_weapon_fired", json{{"slot", 2}, {"flag", true}}, kBudget).ok && rig.log.back() == "slot 2 true");
}

static void promise_jobs_run() {
    Rig rig(fresh(u8"js_promise"));
    CHECK(rig.run("Promise.resolve().then(() => log.write('job'));\nlog.write('main');").ok);
    CHECK((rig.log == std::vector<std::string>{"main", "job"}));
    CHECK(rig.run("on('on_step', () => { Promise.resolve().then(() => log.write('later')); });").ok);
    rig.log.clear();
    CHECK(rig.runtime->fire("on_step", json::object(), kBudget).ok && rig.log == std::vector<std::string>{"later"});
    const auto rejected = rig.run("await Promise.reject(new Error('nope'));");
    CHECK(!rejected.ok && rejected.error.find("nope") != std::string::npos);
}

static void api_read_write() {
    Rig rig(fresh(u8"js_api"));
    CHECK(rig.run("log.write(uc.mem.read_u32(21));\nconst p = uc.player(1);\nlog.write(p.magazine);\np.magazine = 7;\nlog.write(uc.player(2).magazine);\n"
                  "const v = uc.camera.view;\nlog.write(v.length + ':' + v[1]);").ok);
    CHECK((rig.log == std::vector<std::string>{"42", "115", "207", "3:2.5"}));
    CHECK(rig.last_set_args == json({{"slot", 1}}));
    CHECK(rig.magazine == 7);
}

static void api_error_is_exception() {
    Rig rig(fresh(u8"js_error"));
    CHECK(rig.run("try { uc.mem.boom(); log.write('no'); } catch (e) { log.write('caught ' + e.message); }").ok);
    CHECK(rig.log.size() == 1 && rig.log[0].find("caught") == 0 && rig.log[0].find("outside guest memory") != std::string::npos);
    const auto uncaught = rig.run("uc.mem.boom();");
    CHECK(!uncaught.ok && uncaught.error.find("outside guest memory") != std::string::npos);
    CHECK(!rig.run("uc.mem.read_u32('x');").ok);
    CHECK(!rig.run("uc.nothing.at_all();").ok);
}

static void settings_get() {
    Rig rig(fresh(u8"js_settings"));
    CHECK(rig.run("log.write(settings.get('scale'));\nlog.write(String(settings.get('nothere')));\nlog.write(String(settings.set('scale', 99)));\n"
                  "log.write(settings.get('scale'));\nlog.write(String(settings.set('scale', 'x')));").ok);
    CHECK((rig.log == std::vector<std::string>{"2.5", "null", "true", "10", "false"}));
    CHECK(rig.settings.dirty());
}

static void budget_endless_loop() {
    Rig rig(fresh(u8"js_budget"));
    CHECK(rig.run("on('on_step', () => { for (;;) {} });").ok);
    auto begin = std::chrono::steady_clock::now();
    auto r = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    CHECK(!r.ok && r.budget_exceeded && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(200));
    Rig catching(fresh(u8"js_budget_catch"));   // a script cannot swallow the interruption
    CHECK(catching.run("on('on_step', () => { try { for (;;) {} } catch (e) { log.write('swallowed'); } });").ok);
    begin = std::chrono::steady_clock::now();
    r = catching.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(5)});
    CHECK(!r.ok && r.budget_exceeded && catching.log.empty() && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(200));
    CHECK(rig.runtime->fire("on_unknown", json::object(), kBudget).ok);
    const auto load_loop = rig.run("for (;;) {}");   // the entry file has a budget too
    CHECK(!load_loop.ok && load_loop.budget_exceeded);
}

// The runtime is created on one thread and called from others (guest thread, menu thread): the stack limit is measured from the calling thread.
static void stack_limit_follows_the_calling_thread() {
    Rig rig(fresh(u8"js_stack"));
    CHECK(rig.run("function down(n) { return n == 0 ? 0 : 1 + down(n - 1); } on('shallow', () => log.write(down(100))); on('deep', () => down(1e9));").ok);
    CallResult shallow, deep;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::thread other([&] {
            volatile char pad[200 << 10];   // moves this thread's stack pointer far from where the runtime was created
            pad[0] = 0;
            shallow = rig.runtime->fire("shallow", json::object(), kBudget);
            deep = rig.runtime->fire("deep", json::object(), kBudget);
        });
        other.join();
        CHECK(shallow.ok && rig.log.back() == "100");
        CHECK(!deep.ok && deep.error.find("stack") != std::string::npos);
    }
}

// A built-in that loops over a huge array-like in C must reach the interrupt handler too.
static void builtin_loops_are_interrupted() {
    for (const char* body : {"Array.prototype.indexOf.call({length: 2 ** 53}, 0)", "Array.prototype.fill.call({length: 2 ** 40}, 1)",
                             "Array.from({length: 2 ** 32}, (x) => 0)", "'x'.repeat(2 ** 29).split('').length", "/(a+)+b/.test('a'.repeat(60))"}) {
        Rig rig(fresh(u8"js_builtin"));
        CHECK(rig.run(std::string("on('on_step', () => { ") + body + "; });").ok);
        const auto begin = std::chrono::steady_clock::now();
        const auto r = rig.runtime->fire("on_step", json::object(), Budget{std::chrono::milliseconds(20)});
        CHECK(!r.ok && std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    }
}

static void memory_cap() {
    Rig rig(fresh(u8"js_memory"));
    const auto r = rig.run("const a = []; for (;;) a.push('x'.repeat(1000) + a.length);");
    CHECK(!r.ok && r.error.find("memory") != std::string::npos);
    // The module that filled the memory stays loaded in that runtime (the host disables such a mod); another mod's runtime is unaffected.
    Rig other(fresh(u8"js_memory_other"));
    CHECK(other.run("log.write('still alive');").ok);
    CHECK(!other.run("const s = 'x'.repeat(200 * 1024 * 1024); log.write(s.length);").ok);
}

static void syntax_error_reported() {
    Rig rig(fresh(u8"js_syntax"));
    const auto r = rig.run("let x = = 1;\n", "bad.js");
    CHECK(!r.ok && r.error.find("bad.js:1") != std::string::npos);
    const auto missing = rig.runtime->load(rig.root, "scripts/missing.js");
    CHECK(!missing.ok && missing.error.find("missing.js") != std::string::npos);
}

static void runtime_error_has_location() {
    Rig rig(fresh(u8"js_location"));
    const auto r = rig.run("function inner() {\n  const a = null;\n  return a.x;\n}\ninner();\n", "oops.js");
    CHECK(!r.ok && r.error.find("cannot read property") != std::string::npos);
    CHECK(r.error.find("at inner (scripts/oops.js:") != std::string::npos && r.error.find("scripts/oops.js:5") != std::string::npos);   // the stack names the frames with file:line (QuickJS gives the call-site line for the outer frame)
}

static void ui_calls_forwarded() {
    Rig rig(fresh(u8"js_ui"));
    CHECK(rig.run("ui.draw(() => {\n ui.text('hello');\n if (ui.button('Go')) log.write('clicked');\n log.write(String(ui.checkbox('c', false)));\n"
                  " log.write(ui.slider_int('i', 4, 0, 9));\n log.write(ui.slider_float('f', 1.5, 0, 2));\n log.write(ui.combo('co', 0, ['a', 'b']));\n"
                  " log.write(ui.input_text('t', 'x'));\n ui.separator(); ui.same_line();\n log.write(String(ui.collapsing_header('h')));\n ui.tooltip('tip');\n"
                  " const c = ui.color_edit('col', [1, 1, 1, 1]);\n log.write(c[0]);\n});").ok);
    CHECK(rig.runtime->draw_ui(kBudget).ok);
    CHECK(rig.log.size() == 7 && rig.log[0] == "true" && rig.log[1] == "5" && rig.log[2] == "3" && rig.log[3] == "1" && rig.log[4] == "x!" && rig.log[5] == "true" && rig.log[6] == "0.5");
    CHECK(rig.ui.calls.front() == "text:hello" && rig.ui.calls[1] == "button:Go" && rig.ui.calls.back() == "color:col");
    rig.ui.click = true; rig.log.clear();
    CHECK(rig.runtime->draw_ui(kBudget).ok && rig.log[0] == "clicked");
    Rig none(fresh(u8"js_ui_none"));
    CHECK(none.runtime->draw_ui(kBudget).ok);
    CHECK(!none.run("ui.text('x');").ok);   // widgets only work inside the draw function
}

static void hook_pre_regs_round_trip() {
    Rig rig(fresh(u8"js_hook"));
    CHECK(rig.run("log.write(hook.list().join(','));\n"
                  "hook.pre('GameWeaponUpdate', (regs) => {\n log.write(regs.r3 + ',' + regs.f1);\n regs.r4 = regs.r3 + 1;\n regs.f2 = 2.5;\n regs.r10 = 4294967295;\n});").ok);
    CHECK(rig.log == std::vector<std::string>{"GameWeaponUpdate"} && rig.hooks.callbacks.size() == 1 && rig.hooks.owners[0] != nullptr);
    HookRegs regs;
    regs.r[0] = 7; regs.f[0] = 1.5; regs.r[2] = 99;
    const auto result = rig.hooks.callbacks[0](regs, kBudget);
    CHECK(result.ok && rig.log.back() == "7,1.5");
    CHECK(regs.r[0] == 7 && regs.r[1] == 8 && regs.f[1] == 2.5 && regs.r[7] == 0xFFFFFFFFu && regs.r[2] == 99);
    const auto unknown = rig.run("hook.pre('Nope', () => {});");
    CHECK(!unknown.ok && unknown.error.find("hookable") != std::string::npos);
    CHECK(!rig.run("hook.post('GameWeaponUpdate', () => {});").ok && !rig.run("hook.replace('GameWeaponUpdate', () => {});").ok);
    CHECK(rig.run("hook.pre('GameWeaponUpdate', () => { for (;;) {} });").ok);
    const auto loop = rig.hooks.callbacks.back()(regs, Budget{std::chrono::milliseconds(5)});
    CHECK(!loop.ok && loop.budget_exceeded);
    CHECK(rig.run("hook.pre('GameWeaponUpdate', () => { throw new Error('hook failed'); });").ok);
    const auto thrown = rig.hooks.callbacks.back()(regs, kBudget);
    CHECK(!thrown.ok && thrown.error.find("hook failed") != std::string::npos);
}

int main() {
    hook_pre_regs_round_trip();
    no_std_os(); import_confined(); non_ascii_mod_root(); events(); promise_jobs_run(); api_read_write(); api_error_is_exception(); settings_get(); budget_endless_loop();
    stack_limit_follows_the_calling_thread(); builtin_loops_are_interrupted(); memory_cap(); syntax_error_reported(); runtime_error_has_location(); ui_calls_forwarded();
    std::puts("ok");
}
