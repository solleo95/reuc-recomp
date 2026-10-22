// The mod platform's part of the Insert menu, headless: a real SettingsMenu over a real registry and host (fake runtimes), clicked with synthetic mouse events.
#include "check.h"
#include "live_settings.h"
#include "menu_ui.h"
#include "options_config.h"
#include "settings_menu.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;

namespace {
struct Behavior {
    std::vector<std::string> events;   // "load", "fire:<event>", "draw"
    bool has_ui = false, ok_pressed = false, draw_fails = false;
    std::vector<bool> presses;
};

class FakeRuntime : public ModRuntime {
public:
    FakeRuntime(std::shared_ptr<Behavior> b, ModContext& c) : b_(std::move(b)), ctx_(c) {}
    std::string name() const override { return "fake"; }
    bool has_ui() const override { return b_->has_ui; }
    CallResult load(const fs::path&, const std::string&) override { b_->events.push_back("load"); return {}; }
    CallResult fire(std::string_view event, const Value& payload, Budget) override {
        b_->events.push_back("fire:" + std::string(event) + (event == "on_settings_changed" ? ":" + payload["key"].get<std::string>() : ""));
        return {};
    }
    CallResult draw_ui(Budget) override {
        b_->events.push_back("draw");
        if (!b_->has_ui) return {};
        if (b_->draw_fails) return {false, "draw broke", false};
        ctx_.ui->text("hello");
        b_->presses.push_back(ctx_.ui->button("OK"));
        return {};
    }
private:
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

struct ModSpec { std::string id, extra; bool enabled = true; bool ui = false; };

struct World {
    fs::path dir;
    ModRegistry registry;
    ApiRegistry api;
    std::map<std::string, std::shared_ptr<Behavior>> behaviors;
    std::vector<json> saved;   // every section the menu asked to save
    int applies = 0;           // how often the manager asked the game to follow its changes
    std::unique_ptr<ModHost> host;
    std::unique_ptr<ModMenuUi> ui;
    ModHost* host_ptr = nullptr;
    json config_section = json::object();
    fs::path config_path;
    std::unique_ptr<uc::LiveSettings> live;
    std::unique_ptr<uc::SettingsMenu> menu;
    uc::FeatureReport features;
    ImGuiTabBar* tabs = nullptr;
    ImGuiContext* context = nullptr;

    World(const char* name, const std::vector<ModSpec>& specs, const std::string& language = "en") : dir(fresh(name)), registry(build(dir, specs)) {
        host_ptr = nullptr;
        ModHostConfig config;
        config.runtime_factory = [this](RuntimeKind, ModContext& c) { return std::unique_ptr<ModRuntime>(new FakeRuntime(behavior(c.mod_id), c)); };
        config.native.allow_native = true;
        config.native.confirmed = [](const std::string&, const std::string&) { return true; };
        ui = std::make_unique<ModMenuUi>([this] { return &registry; }, [this] { return host_ptr; }, language);
        config.ui = ui.get();
        host = std::make_unique<ModHost>(registry, api, std::move(config));
        for (const auto& s : specs) behavior(s.id)->has_ui = s.ui;
        host->start();
        host_ptr = host.get();
        ui->set_save([this](const json& section) { saved.push_back(section); });
        ui->set_apply([this] { ++applies; host->sync(); return std::vector<std::string>(); });   // what GameMods::apply does for the host
        // The settings menu around it, in a headless ImGui context (the same arrangement as tests/test_settings_menu.cpp).
        config_path = dir / "config.json";
        uc::ConfigDocument document; uc::Options options;
        uc::store_options(document, options); uc::save_config(config_path, document);
        uc::SettingsTargets targets;
        targets.render_scale = [](double) {}; targets.upscaling = [](const uc::Options&) {}; targets.fps_limit = [](std::optional<uint32_t>) {}; targets.bloom = [](const uc::BloomSettings&) {};
        targets.crosshair_scale = [](uint32_t) {}; targets.effects = [](const uc::GraphicsEffects&) {}; targets.dlss5 = [](const uc::Dlss5Settings&) {}; targets.prompt_style = [](uc::PromptStyle) {};
        targets.hdr = [](const uc::HdrSettings&) {}; targets.bindings = [](const uc::Bindings&) {};
        live = std::make_unique<uc::LiveSettings>(config_path, document, targets);
        features.effects = true; features.anisotropy_max = 16;
        menu = std::make_unique<uc::SettingsMenu>(*live, language, [this] { return &features; }, 2560, 1440);
        menu->set_mod_menu(ui.get());
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = {2560, 1440}; io.DeltaTime = 1.f / 60; io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        menu->setup();
        for (int i = 0; i < 6; ++i) frame();
        tabs = ImGui::GetCurrentContext()->TabBars.GetByIndex(0);
    }
    ~World() { host.reset(); ImGui::DestroyContext(context); }   // its own context, whichever one is current: tests keep several worlds alive

    std::shared_ptr<Behavior>& behavior(const std::string& id) { auto& b = behaviors[id]; if (!b) b = std::make_shared<Behavior>(); return b; }
    static ModRegistry build(const fs::path& dir, const std::vector<ModSpec>& specs) {
        json section = {{"enabled", json::array()}, {"order", json::array()}};
        for (const auto& s : specs) {
            fs::create_directories(dir / s.id);
            std::ofstream(dir / s.id / "mod.json") << R"({"id":")" << s.id << R"(","name":")" << s.id << R"(","version":"1.0","api":"1.0","script":"s.lua")" << (s.extra.empty() ? "" : "," + s.extra) << "}";
            section["order"].push_back(s.id);
            if (s.enabled) section["enabled"].push_back(s.id);
        }
        return ModRegistry::discover(dir, section, {1, 0}, "RBUE08");
    }
    void frame() {
        ImGui::NewFrame(); menu->build(); ImGui::Render();
        for (auto* texture : ImGui::GetPlatformIO().Textures) { texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK); }
        tabs = ImGui::GetCurrentContext()->TabBars.GetAliveCount() ? ImGui::GetCurrentContext()->TabBars.GetByIndex(0) : nullptr;
    }
    void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
    void select(int index) { ImGui::TabBarQueueFocus(tabs, &tabs->Tabs[index]); frames(4); CHECK(tabs->SelectedTabId == tabs->Tabs[index].ID); }
    std::string tab_name(int index) {   // the label without its "###id" part, as the menu shows it
        std::string name = ImGui::TabBarGetTabName(tabs, &tabs->Tabs[index]);
        return name.substr(0, name.find("###"));
    }
    void click_at(float x, float y) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame(); frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));   // commit() waits a moment after a widget was active
    }
    bool click(const std::string& key) {
        const auto rect = ui->rect_of(key);
        if (!rect) return false;
        click_at(((*rect)[0] + (*rect)[2]) / 2, ((*rect)[1] + (*rect)[3]) / 2);
        return true;
    }
};
}  // namespace

static const char* kSettings = R"("settings":[{"key":"on","type":"bool","default":false,"label":{"en":"Switch"}},{"key":"scale","type":"number","default":1.5,"min":0,"max":10,"label":{"en":"Scale","ru":"Масштаб"}}])";

static void tabs_appear_after_builtins() {
    World w("menu_tabs", {{"alpha", kSettings}, {"beta", "", true, true}, {"gamma", kSettings, true, true}, {"quiet", ""}, {"off", kSettings, false}});
    CHECK(w.tabs && w.tabs->Tabs.Size == 3 + 1 + 3);   // Main, Effects, Controls (DLSS 5 only when enabled), Mods, then the mods with settings or a drawing function
    CHECK(w.tab_name(3) == "Mods" && w.tab_name(4) == "alpha" && w.tab_name(5) == "beta" && w.tab_name(6) == "gamma");
}

static void tab_bar_scrolls_with_many_mods() {
    std::vector<ModSpec> specs;
    for (int i = 0; i < 12; ++i) specs.push_back({"mod" + std::string(i < 10 ? "0" : "") + std::to_string(i), kSettings});
    World w("menu_many", specs);
    CHECK(w.tabs && w.tabs->Tabs.Size == 3 + 1 + 12);
    CHECK((w.tabs->Flags & ImGuiTabBarFlags_FittingPolicyScroll) != 0);
    CHECK(w.tabs->WidthAllTabs > w.tabs->BarRect.GetWidth());   // they do not fit ...
    const int last = w.tabs->Tabs.Size - 1;
    w.select(last);                                              // ... and the last one can still be reached
    CHECK(w.tabs->ScrollingTarget > 0 || w.tabs->ScrollingAnim > 0);
    CHECK(w.tab_name(last) == "mod11");
}

static void declared_setting_click_and_commit() {
    World w("menu_setting", {{"alpha", kSettings}});
    w.select(4);
    CHECK(!w.host->settings_of("alpha")->dirty() && w.host->settings_of("alpha")->get("on") == false);
    CHECK(w.click("setting/alpha/on"));
    CHECK(w.host->settings_of("alpha")->get("on") == true && w.host->settings_of("alpha")->dirty());
    CHECK(w.saved.empty());                                      // drawing does not save: the main loop's commit does
    w.menu->commit();
    CHECK(w.saved.size() == 1 && w.saved[0]["alpha"]["settings"]["on"] == true && w.saved[0]["alpha"]["settings"]["scale"] == 1.5);
    CHECK(w.saved[0]["enabled"] == json::array({"alpha"}));
    w.host->step();                                              // the change reaches the script on the game thread, at the next step
    CHECK(std::count(w.behavior("alpha")->events.begin(), w.behavior("alpha")->events.end(), "fire:on_settings_changed:on") == 1);
    CHECK(std::count(w.behavior("alpha")->events.begin(), w.behavior("alpha")->events.end(), "fire:on_settings_changed:scale") == 0);   // only the changed key
    w.menu->commit();
    CHECK(w.saved.size() == 1 && std::count(w.behavior("alpha")->events.begin(), w.behavior("alpha")->events.end(), "fire:on_settings_changed:on") == 1);   // nothing new: nothing saved or fired
    w.host->edit_settings("alpha", [](ModSettings& s) { s.set("scale", 99); });   // a script (or anything else) changes a value: clamped, saved, announced
    w.menu->commit();
    CHECK(w.saved.size() == 2 && w.saved[1]["alpha"]["settings"]["scale"] == 10.0);
}

// Dragging a slider changes the value every frame; config.json is written (and the mod told) once, when the drag ends.
static void commit_waits_for_the_end_of_a_drag() {
    World w("menu_drag", {{"alpha", kSettings}});
    w.select(4);
    const auto rect = w.ui->rect_of("setting/alpha/scale");
    CHECK(rect);
    auto& io = ImGui::GetIO();
    const float y = ((*rect)[1] + (*rect)[3]) / 2, x = (*rect)[0] + 5;
    io.AddMousePosEvent(x, y); w.frame();
    io.AddMouseButtonEvent(0, true); w.frame();
    // The game commits from its main loop, where the menu's ImGui context is not current: leave none current here too.
    const auto commit_without_context = [&] { ImGui::SetCurrentContext(nullptr); w.menu->commit(); ImGui::SetCurrentContext(w.context); };
    for (int i = 1; i <= 5; ++i) { io.AddMousePosEvent(x + 20.f * float(i), y); w.frame(); commit_without_context(); }
    CHECK(w.host->settings_of("alpha")->dirty() && w.saved.empty());   // still dragging: nothing written yet
    io.AddMouseButtonEvent(0, false); w.frame(); w.frame();
    commit_without_context();
    CHECK(w.saved.empty());                                            // released a moment ago: the value may still be in flight
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    commit_without_context();
    CHECK(w.saved.size() == 1 && !w.host->settings_of("alpha")->dirty());
    w.host->step();
    CHECK(std::count(w.behavior("alpha")->events.begin(), w.behavior("alpha")->events.end(), "fire:on_settings_changed:scale") == 1);
}

static void language_fallback_for_names_and_labels() {
    World w("menu_language", {{"alpha", R"("names":{"ru":"Альфа"},)" + std::string(kSettings)}, {"beta", ""}}, "ru");
    const auto tabs = w.ui->tabs();
    CHECK(tabs.size() == 1 && tabs[0].title == "Альфа");   // the Russian name; a mod without one shows its English name (see below)
    World en("menu_language_en", {{"alpha", R"("names":{"ru":"Альфа"},)" + std::string(kSettings)}}, "en");
    CHECK(en.ui->tabs()[0].title == "alpha");
    World de("menu_language_de", {{"alpha", R"("names":{"ru":"Альфа"},)" + std::string(kSettings)}}, "de");
    CHECK(de.ui->tabs()[0].title == "alpha");   // an unknown language falls back to English
}

static void free_draw_ids_unique_between_mods() {
    World w("menu_draw", {{"one", "", true, true}, {"two", "", true, true}});
    w.select(4);
    w.frames(2);
    CHECK(w.click("ui/one/OK"));
    CHECK(!w.behavior("one")->presses.empty() && w.behavior("one")->presses.front() == false);   // not pressed before the click
    CHECK(std::count(w.behavior("one")->presses.begin(), w.behavior("one")->presses.end(), true) == 1);
    CHECK(std::count(w.behavior("two")->events.begin(), w.behavior("two")->events.end(), "draw") == 0);   // the other mod's tab is not drawn
    w.select(5);
    w.frames(2);
    CHECK(std::count(w.behavior("two")->presses.begin(), w.behavior("two")->presses.end(), true) == 0);   // the click went to one's button only
    CHECK(w.ui->rect_of("ui/two/OK").has_value());
    w.behavior("two")->draw_fails = true;                                                                    // a failing drawing function is shown, never thrown
    w.frames(3);
    CHECK(w.host->status()[1].failures >= 1);
}

// Mods are switched on and off from the manager while the game runs: the change is saved and then applied (GameMods::apply in the game).
static void manager_toggle_switches_mods_live() {
    World w("menu_manager", {{"alpha", ""}, {"beta", "", false}});
    w.select(3);
    const auto count = [&](const char* id, const char* event) { return std::count(w.behavior(id)->events.begin(), w.behavior(id)->events.end(), event); };
    CHECK(w.registry.wanted("alpha") && !w.registry.wanted("beta") && w.host->status()[0].running && !w.host->status()[1].running && w.applies == 0);
    CHECK(w.click("manager/enable/beta"));
    CHECK(w.registry.wanted("beta"));
    w.menu->commit();
    CHECK(w.saved.size() == 1 && w.saved[0]["enabled"] == json::array({"alpha", "beta"}) && w.applies == 1);
    CHECK(w.host->status()[1].running && count("beta", "fire:on_load") == 1 && count("alpha", "fire:on_load") == 1);   // beta started, alpha was left alone
    CHECK(w.click("manager/enable/beta"));                                                                                 // and off again
    w.menu->commit();
    CHECK(!w.registry.wanted("beta") && !w.host->status()[1].running && count("beta", "fire:on_unload") == 1 && w.applies == 2);
    w.menu->commit();
    CHECK(w.applies == 2);                                                                                                  // nothing changed: nothing applied
}

// The order buttons are real arrow buttons, flush right at the same x in every row.
static void manager_order_buttons_are_aligned_and_work() {
    World w("menu_order", {{"alpha", ""}, {"beta", ""}, {"gamma", ""}});
    w.select(3);
    const auto up_a = w.ui->rect_of("manager/up/alpha"), up_b = w.ui->rect_of("manager/up/beta"), down_a = w.ui->rect_of("manager/down/alpha"), down_c = w.ui->rect_of("manager/down/gamma");
    CHECK(up_a && up_b && down_a && down_c);
    CHECK((*up_a)[0] == (*up_b)[0] && (*down_a)[0] == (*down_c)[0] && (*up_a)[2] - (*up_a)[0] == (*up_a)[3] - (*up_a)[1]);   // same column in every row, square buttons
    CHECK((*down_a)[2] > (*up_a)[2] && (*down_a)[0] - (*up_a)[2] < 20);                                                         // the pair sits together
    const auto enable_a = w.ui->rect_of("manager/enable/alpha");
    CHECK(enable_a && (*up_a)[1] >= (*enable_a)[1] - 1 && (*up_a)[3] <= (*enable_a)[3] + 1);                                    // on the checkbox's line
    CHECK(w.click("manager/down/alpha"));
    w.menu->commit();
    CHECK(w.saved.back()["order"] == json::array({"beta", "alpha", "gamma"}) && w.applies == 1);
    CHECK(w.host->status().size() == 3);
}

static void native_confirmation_flow() {
    World w("menu_native", {{"native_mod", R"("native":"native/mod.dll")", false}});
    fs::create_directories(w.dir / "native_mod" / "native");
    std::ofstream(w.dir / "native_mod" / "native" / "mod.dll", std::ios::binary) << "abc";   // only its hash matters here
    w.select(3);
    CHECK(w.click("manager/enable/native_mod") && w.registry.wanted("native_mod"));
    CHECK(!w.ui->rect_of("manager/confirm/native_mod"));          // nothing to confirm while native mods are off
    CHECK(w.click("manager/allow_native"));
    CHECK(!w.registry.allow_native());                            // a warning comes first
    w.frames(2);
    CHECK(w.click("popup/native_ok") && w.registry.allow_native());
    w.frames(3);
    CHECK(w.ui->rect_of("manager/confirm/native_mod").has_value());
    CHECK(w.registry.native_hash("native_mod").empty());
    CHECK(w.click("manager/confirm/native_mod"));
    w.frames(2);
    CHECK(w.click("popup/confirm_ok"));
    CHECK(w.registry.native_hash("native_mod") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");   // sha256("abc")
    w.menu->commit();
    CHECK(w.saved.back()["allow_native"] == true && w.saved.back()["native_hash"]["native_mod"] == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void no_mods_menu_is_harmless() {
    World w("menu_empty", {});
    CHECK(w.tabs && w.tabs->Tabs.Size == 3 + 1);   // just the manager tab, which says there are no mods
    w.select(3);
    CHECK(w.ui->tabs().empty());
}

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"tabs_appear_after_builtins", tabs_appear_after_builtins}, {"tab_bar_scrolls_with_many_mods", tab_bar_scrolls_with_many_mods},
        {"declared_setting_click_and_commit", declared_setting_click_and_commit}, {"commit_waits_for_the_end_of_a_drag", commit_waits_for_the_end_of_a_drag},
        {"language_fallback_for_names_and_labels", language_fallback_for_names_and_labels},
        {"free_draw_ids_unique_between_mods", free_draw_ids_unique_between_mods}, {"manager_toggle_switches_mods_live", manager_toggle_switches_mods_live},
        {"manager_order_buttons_are_aligned_and_work", manager_order_buttons_are_aligned_and_work},
        {"native_confirmation_flow", native_confirmation_flow}, {"no_mods_menu_is_harmless", no_mods_menu_is_harmless}};
    for (const auto& [name, run] : tests) { std::printf("%s\n", name); std::fflush(stdout); run(); }
    std::puts("ok");
}
