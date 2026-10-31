// The Steam Deck launcher's Mods tab: rows over StandaloneMods, driven by the D-pad through the focus list (host test, headless ImGui).
#include "check.h"
#include "deck/mods_page.h"
#include "deck/settings_pages.h"
#include "imgui.h"
#include "standalone.h"
#include <fstream>

namespace fs = std::filesystem;
using namespace uc;
using namespace uc::deck;
using uc::mods::StandaloneMods;
using nlohmann::ordered_json;

static fs::path fresh(const char* name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}
static void mod(const fs::path& dir, const std::string& id, const std::string& extra = "") {
    fs::create_directories(dir / id);
    std::ofstream(dir / id / "mod.json") << R"({"id":")" << id << R"(","name":")" << id << R"(","version":"1.0","api":"1.0")" << (extra.empty() ? "" : "," + extra) << "}";
}
static const char* kSettings = R"("settings":[{"key":"on","type":"bool","default":false,"label":{"en":"Switch"}},{"key":"n","type":"int","default":5,"min":0,"max":10,"label":{"en":"Count"}},)"
                               R"({"key":"mode","type":"choice","default":"a","choices":["a","b"],"label":{"en":"Mode"}},{"key":"name","type":"text","default":"x","label":{"en":"Name"}}])";

// One headless screen: the focus list, the row context and the Mods tab drawn into a window each frame.
struct Screen {
    ImGuiContext* context;
    FocusList focus;
    RowCtx rows;
    ModsUi ui;
    int part = 0;
    bool settings_window = false;   // draw the settings rows instead of the tab
    Screen() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = {1280, 720}; io.DeltaTime = 1.f / 60; io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        rows.focus = &focus;
        rows.unit = 1;
        rows.text = [](std::string_view key) { return std::string(key); };
        rows.panel_text = [](std::string_view) { return ""; };
        rows.sub = &part;
    }
    ~Screen() { ImGui::DestroyContext(context); }
    bool frame(StandaloneMods& mods, std::vector<DeckKey> keys = {}) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1280, 720});
        ImGui::Begin("w", nullptr, ImGuiWindowFlags_NoDecoration);
        focus.begin_frame(keys);
        const bool changed = settings_window ? draw_mod_settings(rows, mods, ui, "en") : draw_mods(rows, mods, ui, "en");
        focus.end_frame();
        ImGui::End();
        ImGui::Render();
        for (auto* texture : ImGui::GetPlatformIO().Textures) { texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK); }
        return changed;
    }
};

static void tab_count() {
    CHECK(kTabCount == 4 && int(Tab::mods) == 3);
    CHECK(tab_pages(Tab::mods).empty());   // the Mods tab is not a registry page
}

static void empty_list_has_only_the_native_switch() {
    StandaloneMods mods(fresh("dm_none"), ordered_json::object());
    Screen screen;
    screen.frame(mods); screen.frame(mods);
    CHECK(screen.focus.count() == 1);   // "Allow native code"; the "no mods" line is text
    CHECK(!screen.frame(mods, {DeckKey::down, DeckKey::up}));
}

static void a_enables_and_the_parts_are_reached_by_left_right() {
    const auto dir = fresh("dm_parts");
    mod(dir, "a", kSettings);
    mod(dir, "b");
    StandaloneMods mods(dir, ordered_json::object());
    Screen screen;
    screen.frame(mods);
    screen.frame(mods, {DeckKey::down});   // row 1: the first mod
    CHECK(screen.frame(mods, {DeckKey::accept}) && mods.registry().wanted("a"));   // A on the row switches it on
    CHECK(screen.frame(mods, {DeckKey::accept}) && !mods.registry().wanted("a"));  // and off again
    CHECK(!screen.frame(mods, {DeckKey::right}));                                   // the gear
    CHECK(!screen.frame(mods, {DeckKey::accept}) && screen.ui.settings_id == "a");  // A on the gear opens the settings, nothing is saved
    screen.ui.settings_id.clear();
    screen.frame(mods, {DeckKey::left});                                            // back to the row itself
    screen.frame(mods, {DeckKey::down});                                            // the second mod has no gear: right is "load earlier"
    screen.frame(mods, {DeckKey::right});
    CHECK(screen.frame(mods, {DeckKey::accept}));
    CHECK(mods.registry().entries()[0].manifest.id == "b" && mods.registry().entries()[1].manifest.id == "a");
    screen.frame(mods);   // the focus follows the mod that moved: it is the first mod row now, and still on its "load earlier" button
    CHECK(screen.focus.focus() == 1 && screen.part == 1);
}

static void the_first_mod_cannot_move_up() {
    const auto dir = fresh("dm_order");
    mod(dir, "a");
    mod(dir, "b");
    StandaloneMods mods(dir, ordered_json::object());
    Screen screen;
    screen.frame(mods);
    screen.frame(mods, {DeckKey::down});
    screen.frame(mods, {DeckKey::right});   // the first mod's "load earlier" is there (the parts keep their places) but moves nothing
    CHECK(!screen.frame(mods, {DeckKey::accept}));
    CHECK(mods.registry().entries()[0].manifest.id == "a");
    screen.frame(mods, {DeckKey::right});   // "load later"
    CHECK(screen.frame(mods, {DeckKey::accept}));
    CHECK(mods.registry().entries()[0].manifest.id == "b");
}

static void native_code_needs_the_warning() {
    StandaloneMods mods(fresh("dm_native"), ordered_json::object());
    Screen screen;
    screen.frame(mods);
    CHECK(!screen.frame(mods, {DeckKey::accept}));
    CHECK(screen.ui.native_warning && !mods.registry().allow_native());   // the modal asks first
    mods.registry().set_allow_native(true);
    screen.ui.native_warning = false;
    CHECK(screen.frame(mods, {DeckKey::accept}) && !mods.registry().allow_native());   // switching it off needs no question
}

static void settings_rows() {
    const auto dir = fresh("dm_settings");
    mod(dir, "a", kSettings);
    StandaloneMods mods(dir, ordered_json::object());
    Screen screen;
    screen.ui.settings_id = "a";
    screen.settings_window = true;
    std::vector<std::string> list_labels;
    std::function<void(int)> choose;
    screen.ui.open_list = [&](const std::string&, std::vector<std::string> labels, int, std::function<void(int)> chosen) { list_labels = std::move(labels); choose = std::move(chosen); };
    screen.frame(mods);
    CHECK(screen.focus.count() == 5);   // switch, count, mode, name and Close
    CHECK(screen.frame(mods, {DeckKey::accept}) && mods.settings("a")->get("on") == true);                // the switch
    screen.frame(mods, {DeckKey::down});
    CHECK(screen.frame(mods, {DeckKey::right}) && mods.settings("a")->get("n") == 6);                       // the slider: one step per press
    for (int i = 0; i < 9; ++i) screen.frame(mods, {DeckKey::right});
    CHECK(mods.settings("a")->get("n") == 10);                                                              // and it stops at the end
    screen.frame(mods, {DeckKey::down});
    CHECK(!screen.frame(mods, {DeckKey::accept}) && list_labels == (std::vector<std::string>{"a", "b"}));  // the choice opens a list
    choose(1);
    CHECK(mods.settings("a")->get("mode") == "b");
    screen.frame(mods, {DeckKey::down});
    screen.frame(mods, {DeckKey::accept});
    CHECK(screen.ui.text_open && screen.ui.text_key == "name" && screen.ui.text_buffer == "x");               // the text opens its entry
    screen.ui.text_open = false;
    screen.frame(mods, {DeckKey::down});
    screen.frame(mods, {DeckKey::accept});
    CHECK(screen.ui.settings_id.empty());                                                                   // Close
}

int main() {
    tab_count();
    empty_list_has_only_the_native_switch();
    a_enables_and_the_parts_are_reached_by_left_right();
    the_first_mod_cannot_move_up();
    native_code_needs_the_warning();
    settings_rows();
    std::puts("deck mods passed");
}
