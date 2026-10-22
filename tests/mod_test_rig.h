#pragma once
// Shared by the script runtime tests (Lua, JavaScript): a recording UI, a small fake API registry and a runtime wired to them.
#include "check.h"
#include "runtime.h"
#include <filesystem>
#include <fstream>

namespace rig {
namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;

struct FakeUi : UiApi {
    std::vector<std::string> calls;
    bool click = false;
    void text(const std::string& t) override { calls.push_back("text:" + t); }
    bool button(const std::string& l) override { calls.push_back("button:" + l); return click; }
    bool checkbox(const std::string& l, bool v) override { calls.push_back("checkbox:" + l); return !v; }
    int slider_int(const std::string& l, int v, int, int) override { calls.push_back("slider_int:" + l); return v + 1; }
    double slider_float(const std::string& l, double v, double, double) override { calls.push_back("slider_float:" + l); return v * 2; }
    int combo(const std::string& l, int s, const std::vector<std::string>& items) override { calls.push_back("combo:" + l + ":" + std::to_string(items.size())); return s + 1; }
    std::string input_text(const std::string& l, const std::string& v) override { calls.push_back("input_text:" + l); return v + "!"; }
    void separator() override { calls.push_back("separator"); }
    void same_line() override { calls.push_back("same_line"); }
    bool collapsing_header(const std::string& l) override { calls.push_back("header:" + l); return true; }
    void tooltip(const std::string& t) override { calls.push_back("tooltip:" + t); }
    std::array<float, 4> color_edit(const std::string& l, std::array<float, 4> c) override { calls.push_back("color:" + l); c[0] = 0.5f; return c; }
};

// Records the hook callbacks a script installs; only "GameWeaponUpdate" is hookable.
struct FakeHooks : HookHost {
    std::vector<std::function<CallResult(HookRegs&, Budget)>> callbacks;
    std::vector<const void*> owners;
    bool add_pre(const void* owner, const std::string& name, std::function<CallResult(HookRegs&, Budget)> callback) override {
        if (name != "GameWeaponUpdate") return false;
        owners.push_back(owner);
        callbacks.push_back(std::move(callback));
        return true;
    }
    std::vector<std::string> names() const override { return {"GameWeaponUpdate"}; }
};

struct Rig {
    using Factory = std::unique_ptr<ModRuntime> (*)(ModContext&);
    fs::path root;
    std::string extension;
    ApiRegistry api;
    ModSettings settings;
    FakeUi ui;
    FakeHooks hooks;
    std::vector<std::string> log;
    std::unique_ptr<ModContext> context;
    std::unique_ptr<ModRuntime> runtime;
    int magazine = 15;
    json last_set_args;

    Rig(const fs::path& folder, Factory make, std::string ext) : root(folder), extension(std::move(ext)), settings(decls(), json::object()) {
        api.add({"mem.read_u32", {"address"}, nullptr, nullptr, [](const json& a) { return json(a["address"].get<int>() * 2); }, ""});
        api.add({"mem.boom", {}, nullptr, nullptr, [](const json&) -> json { throw ApiError("address 0x10 is outside guest memory"); }, ""});
        api.add({"player.magazine", {}, [this](const json& a) { return json(a["slot"].get<int>() * 100 + magazine); },
                 [this](const json& a, const json& v) { last_set_args = a; magazine = v.get<int>(); }, nullptr, "slot"});
        api.add({"camera.view", {}, [](const json&) { return json::array({1.0, 2.5, 3.0}); }, nullptr, nullptr, ""});
        context = std::make_unique<ModContext>(ModContext{api, settings, "testmod", [this](std::string_view line) { log.emplace_back(line); }, &ui, nullptr, &hooks});
        runtime = make(*context);
    }
    static std::vector<SettingDecl> decls() {
        return parse_manifest(R"({"id":"m","name":"m","version":"1","api":"1.0","settings":[{"key":"scale","type":"number","default":2.5,"min":0,"max":10,"label":{"en":"S"}}]})").manifest->settings;
    }
    // Writes scripts/<file> (default main.<ext>) and loads it as the mod's entry.
    CallResult run(const std::string& script, std::string file = {}) {
        if (file.empty()) file = "main" + extension;
        fs::create_directories(root / "scripts");
        std::ofstream(root / "scripts" / file, std::ios::binary) << script;
        return runtime->load(root, "scripts/" + file);
    }
};

inline fs::path fresh(const std::u8string& name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}
inline const Budget kBudget{std::chrono::milliseconds(200)};
}  // namespace rig
