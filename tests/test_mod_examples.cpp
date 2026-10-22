// The example mods of mods-examples/, run for real: each one is copied into a mods folder, started through GameMods (real registry, host, Lua/JS
// runtimes, game API) over synthetic game structures in guest memory, and its effect on that memory is checked.
#include "check.h"
#include "game_mods.h"
#include "guest_mem.h"
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::ordered_json;

namespace {
constexpr uint32_t kPlayerTable = 0x806C3AD8, kScenePointer = 0x806C3B78, kWeaponTable = 0x8046A86C, kStride = 168;
constexpr uint32_t kP1 = 0x80100000, kP2 = 0x80101000, kScene = 0x80200000;

// Widgets that draw nothing; remembers the text and whether a button was asked for.
struct RecordingUi : UiApi {
    std::vector<std::string> texts, buttons;
    void text(const std::string& t) override { texts.push_back(t); }
    bool button(const std::string& l) override { buttons.push_back(l); return false; }
    bool checkbox(const std::string&, bool v) override { return v; }
    int slider_int(const std::string&, int v, int, int) override { return v; }
    double slider_float(const std::string&, double v, double, double) override { return v; }
    int combo(const std::string&, int s, const std::vector<std::string>&) override { return s; }
    std::string input_text(const std::string&, const std::string& v) override { return v; }
    void separator() override {}
    void same_line() override {}
    bool collapsing_header(const std::string&) override { return false; }
    void tooltip(const std::string&) override {}
    std::array<float, 4> color_edit(const std::string&, std::array<float, 4> c) override { return c; }
};

struct World {
    RecordingUi ui;
    fs::path exe;
    uint8_t* memory;
    GuestMemory view;
    uc::DvdFs dvd;
    std::unique_ptr<uc::GameMods> mods;

    // `example`: a folder name under mods-examples; `settings`: the stored "settings" object of the mod (may be empty).
    World(const char* example, const char* id, const ordered_json& settings = ordered_json::object())
        : memory((uc::guest_mem_init(), uc::guest_mem_base())), view(memory) {
        exe = fs::current_path() / (std::string("examples_") + example);
        std::error_code error;
        fs::remove_all(exe, error);
        fs::create_directories(exe / "disc" / "DATA");
        fs::create_directories(exe / "saves");
        fs::create_directories(exe / "mods");
        fs::copy(fs::path(UC_EXAMPLES_DIR) / example, exe / "mods" / example, fs::copy_options::recursive);
        std::memset(memory + 0x100000, 0, 0x300000);
        view.write_u32(kPlayerTable, kP1); view.write_u32(kPlayerTable + 4, kP2); view.write_u32(kScenePointer, kScene);
        view.write_u32(kP1 + 0x4D4, 15); view.write_u32(kP2 + 0x4D4, 8);
        for (uint32_t i = 0; i < 20; ++i) {   // a weapon table the way the game fills it: damage 10 * (i + 1), capacity 15, reload 1.0, mode 1
            view.write_f32(kWeaponTable + i * kStride + 0x24, 10.f * float(i + 1));
            view.write_u32(kWeaponTable + i * kStride + 0x14, 15);
            view.write_f32(kWeaponTable + i * kStride + 0x38, 1.f);
            view.write_u32(kWeaponTable + i * kStride + 0x10, 1);
        }
        ordered_json section = {{"enabled", {id}}};
        if (!settings.empty()) section[id] = {{"settings", settings}};
        CHECK(dvd.open_root(exe / "disc"));
        mods = std::make_unique<uc::GameMods>(exe, section, dvd, "RBUE08", false);
        CHECK(mods->active());
        mods->start(memory, &ui);
        CHECK(mods->host() && !mods->host()->status().empty() && !mods->host()->status()[0].disabled_for_session);
    }
    ~World() { mods.reset(); }
    void step(int n = 1) { for (int i = 0; i < n; ++i) uc::mods::mods_step(); }
    float weapon_damage(uint32_t id) { return view.read_f32(kWeaponTable + id * kStride + 0x24).value(); }
    uint32_t weapon_word(uint32_t id, uint32_t offset) { return view.read_u32(kWeaponTable + id * kStride + offset).value(); }
    void change_setting(const char* id, const char* key, const nlohmann::json& value) {
        mods->host()->edit_settings(id, [&](ModSettings& s) { CHECK(s.set(key, value)); });
        mods->host()->settings_changed(id, key);
    }
};
}  // namespace

static void every_example_manifest_is_valid() {
    int found = 0;
    for (const auto& item : fs::directory_iterator(UC_EXAMPLES_DIR)) {
        if (!item.is_directory() || !fs::exists(item.path() / "mod.json")) continue;
        std::ifstream in(item.path() / "mod.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        const auto parsed = parse_manifest(text);
        if (!parsed.manifest) { for (const auto& e : parsed.errors) std::fprintf(stderr, "%s: %s\n", item.path().filename().string().c_str(), e.c_str()); }
        CHECK(parsed.manifest);
        CHECK(parsed.manifest->api.major == 1 && !parsed.manifest->names.empty());   // every example has a Russian name too
        ++found;
    }
    CHECK(found == 6);
}

static void infinite_ammo() {
    World w("infinite_ammo", "infinite-ammo");
    w.step();                                                   // learns the full magazine
    w.view.write_u32(kP1 + 0x4D4, 9);
    w.view.write_u32(kP2 + 0x4D4, 3);
    w.step();
    CHECK(w.view.read_u32(kP1 + 0x4D4) == 15u);                 // player 1 (default on) refilled
    CHECK(w.view.read_u32(kP2 + 0x4D4) == 3u);                  // player 2 (default off) is not
    w.change_setting("infinite-ammo", "player2", true);
    w.step(); w.step(); w.view.write_u32(kP2 + 0x4D4, 2); w.step();   // the first step delivers the change, the second learns the magazine
    CHECK(w.view.read_u32(kP2 + 0x4D4) == 3u);                  // switched on later: refilled to what it had when it was learned
    w.change_setting("infinite-ammo", "player1", false);
    w.step();
    w.view.write_u32(kP1 + 0x4D4, 4);
    w.step();
    CHECK(w.view.read_u32(kP1 + 0x4D4) == 4u);
}

static void god_mode() {
    World w("god_mode", "god-mode", {{"player1", true}, {"player2", false}});
    w.view.write_f32(kScene + 0x6C00, 30.f);
    w.view.write_f32(kScene + 0x6C14, 20.f);
    w.step();
    CHECK(w.view.read_f32(kScene + 0x6C00) == 0.f && w.view.read_f32(kScene + 0x6C14) == 20.f);   // the stored settings: player 1 only
    w.change_setting("god-mode", "player2", true);
    w.step(); w.step();
    CHECK(w.view.read_f32(kScene + 0x6C14) == 0.f);
    CHECK(w.mods->host()->status()[0].has_ui && w.mods->host()->draw_ui("god-mode").ok);   // the free-drawing function runs
    CHECK(w.ui.buttons == std::vector<std::string>{"Heal now"} && w.ui.texts.size() == 2);
    w.view.write_u32(kPlayerTable, 0);                                                    // not in a mission: the tab says so instead of failing
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("god-mode").ok && w.ui.texts.back() == "not in a mission");
}

static void pistol_autofire() {
    World w("pistol_autofire", "pistol-autofire");
    w.step();
    CHECK(w.weapon_word(0, 0x10) == 0u && w.weapon_word(19, 0x10) == 0u && w.weapon_word(1, 0x10) == 1u);   // the handgun records are automatic, the shotgun is not
    w.change_setting("pistol-autofire", "auto", false);
    w.step();
    CHECK(w.weapon_word(0, 0x10) == 1u && w.weapon_word(19, 0x10) == 1u);
    w.change_setting("pistol-autofire", "auto", true);
    for (uint32_t id : {0u, 19u}) w.view.write_u32(kWeaponTable + id * kStride + 0x10, 1);   // the game refills its table
    w.step();
    CHECK(w.weapon_word(0, 0x10) == 0u);                                                        // and the mod's choice is back at the next step
}

static void damage_multiplier() {
    World w("damage_multiplier", "damage-multiplier");
    w.step();                                                   // the first step captures the original values and applies the default scale 2
    CHECK(w.weapon_damage(0) == 20.f && w.weapon_damage(1) == 40.f && w.weapon_damage(19) == 400.f);
    w.change_setting("damage-multiplier", "scale", 3.0);
    w.step();
    CHECK(w.weapon_damage(0) == 30.f && w.weapon_damage(19) == 600.f);   // from the originals, not from the previous result
    w.view.write_f32(kWeaponTable + 0x24, 10.f);               // the game refills the table
    w.step();
    CHECK(w.weapon_damage(0) == 30.f);
    w.change_setting("damage-multiplier", "scale", 0.5);
    w.step();
    CHECK(w.weapon_damage(0) == 5.f);
}

static void faster_handgun() {
    World w("faster_handgun", "faster-handgun");
    CHECK(w.weapon_word(0, 0x14) == 30u && w.weapon_word(19, 0x14) == 30u && w.weapon_word(1, 0x14) == 15u);   // data only: no script ran, the patch did
    CHECK(w.view.read_f32(kWeaponTable + 0x38) == 0.3f);
    w.view.write_u32(kWeaponTable + 0x14, 15);
    w.step();
    CHECK(w.weapon_word(0, 0x14) == 30u);                                                                      // kept when the game refills its table
    CHECK(w.mods->host()->status()[0].has_native == false && !w.mods->host()->status()[0].has_ui);
}

int main() {
    const std::pair<const char*, void (*)()> tests[] = {{"every_example_manifest_is_valid", every_example_manifest_is_valid}, {"infinite_ammo", infinite_ammo}, {"god_mode", god_mode},
                                                         {"pistol_autofire", pistol_autofire}, {"damage_multiplier", damage_multiplier}, {"faster_handgun", faster_handgun}};
    for (const auto& [name, run] : tests) {
        std::printf("%s\n", name);
        std::fflush(stdout);
        try { run(); } catch (const std::exception& e) { std::fprintf(stderr, "%s threw: %s\n", name, e.what()); return 1; }
    }
    std::puts("ok");
}
