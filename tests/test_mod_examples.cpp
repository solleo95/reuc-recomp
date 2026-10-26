// The example mods of mods-examples/, run for real: each one is copied into a mods folder, started through GameMods (real registry, host, Lua/JS
// runtimes, game API) over synthetic game structures in guest memory, and its effect on that memory is checked.
#include "check.h"
#include "game_mods.h"
#include "guest_mem.h"
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::ordered_json;

namespace {
constexpr uint32_t kPlayerTable = 0x806C3AD8, kScenePointer = 0x806C3B78, kWeaponTable = 0x8046A86C, kStride = 168;
constexpr uint32_t kP1 = 0x80100000, kP2 = 0x80101000, kScene = 0x80200000;

// Widgets that draw nothing; remembers the text and whether a button was asked for.
struct RecordingUi : UiApi {
    std::vector<std::string> texts, buttons;
    bool open_headers = false;                      // what collapsing_header answers
    std::set<std::string> press;                    // buttons that report a click
    std::map<std::string, std::string> typed;       // input_text answers by label (others return what they were given)
    std::map<std::string, int> chosen;              // combo answers by label
    bool has(const std::string& part) const { for (const auto& t : texts) if (t.find(part) != std::string::npos) return true; return false; }
    size_t count(const std::string& part) const { size_t n = 0; for (const auto& t : texts) n += t.find(part) != std::string::npos; return n; }
    void text(const std::string& t) override { texts.push_back(t); }
    bool button(const std::string& l) override { buttons.push_back(l); return press.count(l) > 0; }
    bool checkbox(const std::string&, bool v) override { return v; }
    int slider_int(const std::string&, int v, int, int) override { return v; }
    double slider_float(const std::string&, double v, double, double) override { return v; }
    int combo(const std::string& l, int s, const std::vector<std::string>&) override { const auto at = chosen.find(l); return at == chosen.end() ? s : at->second; }
    std::string input_text(const std::string& l, const std::string& v) override { const auto at = typed.find(l); return at == typed.end() ? v : at->second; }
    void separator() override {}
    void same_line() override {}
    bool collapsing_header(const std::string&) override { return open_headers; }
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
            view.write_f32(kWeaponTable + i * kStride + 0x0C, 1.f);   // a shot per second
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
    CHECK(found == 8);
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

// ---- the inspector: read-only view of everything the API exposes ----

namespace {
std::vector<std::string> inspector_log_lines(const World& w) {   // the snapshot lines the mod wrote: "[inspector] {json}"
    std::ifstream in(w.exe / "logs" / "mods.log");
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) if (line.rfind("[inspector] {", 0) == 0) lines.push_back(line.substr(12));
    return lines;
}
constexpr const char* kWatch1 = "Address or symbol 1";
}  // namespace

static void inspector_in_a_mission() {
    World w("inspector", "inspector");
    w.ui.open_headers = true;
    CHECK(w.mods->host()->status()[0].has_ui && w.mods->host()->draw_ui("inspector").ok);
    CHECK(w.ui.has("Player 1") && w.ui.has("magazine 15") && w.ui.has("Player 2") && w.ui.has("magazine 8"));
    CHECK(!w.ui.has("not in a mission"));
}

static void inspector_outside_a_mission() {
    World w("inspector", "inspector");
    w.ui.open_headers = true;
    w.view.write_u32(0x805C5D74, 77);   // the mission clock keeps its last value after a mission: it must not be shown as current
    w.view.write_u32(kPlayerTable, 0);
    w.view.write_u32(kPlayerTable + 4, 0);
    w.view.write_u32(kScenePointer, 0);
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    CHECK(w.ui.count("not in a mission") == 2 && !w.ui.has("Player 1") && !w.ui.has("Mission time"));   // the top line and the Mission section
    w.ui.press.insert("Log snapshot");
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    const auto json = nlohmann::json::parse(inspector_log_lines(w).back(), nullptr, false);
    CHECK(!json.is_discarded() && json["game"].is_null() && json["camera"].is_null() && json["enemies"].empty());
}

static void memory_watch_reads_what_is_typed() {
    World w("inspector", "inspector");
    w.ui.open_headers = true;
    w.view.write_u32(0x80100200, 123);
    w.ui.typed[kWatch1] = "0x80100200";
    w.ui.chosen["Type 1"] = 2;   // u32
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    CHECK(w.ui.has("123") && w.ui.has("0x0000007B"));
    w.ui.texts.clear();
    w.ui.typed[kWatch1] = "2148532736";   // the same address in decimal
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.has("0x0000007B"));
    w.view.write_f32(0x80100204, 2.5f);
    w.ui.typed[kWatch1] = "0x80100204";
    w.ui.chosen["Type 1"] = 4;   // f32
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.has("2.5"));
    for (const auto& [bits, shown] : {std::pair<uint32_t, const char*>{0x7F800000u, "NaN or infinity"}, {0xFF800000u, "NaN or infinity"}, {0x80000000u, "-0"}, {0x7FC00000u, "NaN or infinity"}}) {
        w.view.write_u32(0x80100208, bits);
        w.ui.typed[kWatch1] = "0x80100208";
        w.ui.texts.clear();
        CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.has(std::string("= ") + shown));
    }
    w.ui.typed[kWatch1] = "0x80100204";
    w.ui.texts.clear();
    w.ui.typed[kWatch1] = "";
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.has("type an address"));
    const size_t others = w.ui.count("! ");   // notes of the other sections (the rig has no camera)
    for (const char* bad : {"zz", "0x", "-5", "0xFFFFFFFF", "no_such_symbol", "0x80100200 junk", "1e30"}) {
        w.ui.texts.clear();
        w.ui.typed[kWatch1] = bad;
        CHECK(w.mods->host()->draw_ui("inspector").ok);
        CHECK(w.ui.count("! ") == others + 1 && w.ui.count("type an address") == 3);   // (the three other, empty rows show their hint)   // one message line for the bad input, no throw out of the drawing function
    }
}

static void events_are_logged_and_trimmed() {
    World w("inspector", "inspector", {{"events_kept", 5}});
    w.ui.open_headers = true;
    w.step();   // the first step only learns the magazines
    for (uint32_t magazine = 14; magazine >= 7; --magazine) { w.view.write_u32(kP1 + 0x4D4, magazine); w.step(); }   // eight shots
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    CHECK(w.ui.count("weapon_fired") == 5);                                   // the ring keeps the newest five
    CHECK(w.ui.has("slot=1"));
    w.ui.press.insert("Clear events");
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    w.ui.press.clear();
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.count("weapon_fired") == 0 && w.ui.has("no events yet"));
}

static void events_kept_can_be_lowered_at_once() {
    World w("inspector", "inspector", {{"events_kept", 20}});
    w.ui.open_headers = true;
    w.step();
    for (uint32_t magazine = 14; magazine >= 9; --magazine) { w.view.write_u32(kP1 + 0x4D4, magazine); w.step(); }   // six events
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.count("weapon_fired") == 6);
    w.change_setting("inspector", "events_kept", 5);
    w.step();
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("inspector").ok && w.ui.count("weapon_fired") == 5);
}

static void snapshot_is_one_line_of_json() {
    World w("inspector", "inspector");
    w.ui.open_headers = true;
    w.ui.press.insert("Log snapshot");
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    const auto lines = inspector_log_lines(w);
    CHECK(lines.size() == 1);
    const auto json = nlohmann::json::parse(lines[0], nullptr, false);
    CHECK(!json.is_discarded() && json.is_object());
    for (const char* key : {"players", "game", "camera", "enemies"}) CHECK(json.contains(key));
    CHECK(json["players"].is_array() && json["players"].size() == 2 && json["players"][0]["magazine"] == 15 && json["players"][1]["magazine"] == 8);
    CHECK(json["enemies"].is_array());
    w.view.write_u32(kPlayerTable + 4, 0);   // player 2 gone: null, not an error
    w.ui.press.insert("Log snapshot");
    CHECK(w.mods->host()->draw_ui("inspector").ok);
    const auto again = nlohmann::json::parse(inspector_log_lines(w).back(), nullptr, false);
    CHECK(!again.is_discarded() && again["players"][1].is_null());
}

static void log_every_writes_snapshots() {
    {
        World w("inspector", "inspector", {{"log_every", 1}});
        CHECK(inspector_log_lines(w).empty());
        w.step(31);
        CHECK(inspector_log_lines(w).size() == 1);
        w.step(30);
        CHECK(inspector_log_lines(w).size() == 2);
        for (const auto& line : inspector_log_lines(w)) CHECK(!nlohmann::json::parse(line, nullptr, false).is_discarded());
    }
    World off("inspector", "inspector");   // the default: nothing is written without a click
    off.step(100);
    CHECK(inspector_log_lines(off).empty());
}

static void log_every_waits_while_the_game_is_paused() {
    World w("inspector", "inspector", {{"log_every", 1}});
    w.view.write_u32(0x805965A8, 2);   // the game's pause menu is open: the steps still come, the game's time does not
    w.step(70);
    CHECK(inspector_log_lines(w).empty());
    w.view.write_u32(0x805965A8, 1);
    w.step(31);
    CHECK(inspector_log_lines(w).size() == 1);
}

static void inspector_settings_are_declared() {
    std::ifstream in(fs::path(UC_EXAMPLES_DIR) / "inspector" / "mod.json");
    const auto manifest = nlohmann::json::parse(in);
    CHECK(manifest["api"] == "1.1" && !manifest.contains("touches_gameplay"));
    std::map<std::string, std::array<double, 3>> expected = {{"enemies_shown", {1, 30, 8}}, {"events_kept", {5, 100, 30}}, {"log_every", {0, 600, 0}}};
    for (const auto& setting : manifest["settings"]) {
        const auto at = expected.find(setting["key"]);
        CHECK(at != expected.end() && setting["type"] == "int");   // whole numbers: the integer slider
        CHECK(setting["min"] == at->second[0] && setting["max"] == at->second[1] && setting["default"] == at->second[2]);
        expected.erase(at);
    }
    CHECK(expected.empty());
}

// ---- the randomizer: weapon stats, zombie hit points and the start weapon from a seed ----

namespace {
struct WeaponValues { std::vector<float> damage, reload, rate; std::vector<uint32_t> capacity; };

WeaponValues weapon_values(World& w) {
    WeaponValues v;
    for (uint32_t id = 0; id < 20; ++id) {
        v.damage.push_back(w.weapon_damage(id));
        v.capacity.push_back(w.weapon_word(id, 0x14));
        v.reload.push_back(w.view.read_f32(kWeaponTable + id * kStride + 0x38).value());
        v.rate.push_back(w.view.read_f32(kWeaponTable + id * kStride + 0x0C).value());
    }
    return v;
}

WeaponValues randomized(const ordered_json& settings) {   // a world of its own (the rig's folder is per example)
    World w("randomizer", "randomizer", settings);
    w.step(2);
    return weapon_values(w);
}

bool near(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max(1.f, std::fabs(b)); }
}  // namespace

static void randomizer_weapons_follow_the_seed() {
    const auto a = randomized({{"seed", 7}, {"strength", 50}});
    int changed = 0;
    for (size_t id = 0; id < 20; ++id) {
        const float original = 10.f * float(id + 1);
        CHECK(a.damage[id] >= original / 2 - 1e-3f && a.damage[id] <= original * 2 + 1e-3f);
        CHECK(a.capacity[id] >= 8 && a.capacity[id] <= 30);
        CHECK(a.reload[id] >= 0.5f - 1e-3f && a.reload[id] <= 2.f + 1e-3f);
        CHECK(a.rate[id] >= 0.5f - 1e-3f && a.rate[id] <= 2.f + 1e-3f);
        changed += !near(a.damage[id], original);
    }
    CHECK(changed >= 10);
    const auto same = randomized({{"seed", 7}, {"strength", 50}});
    CHECK(same.damage == a.damage && same.capacity == a.capacity && same.reload == a.reload && same.rate == a.rate);   // deterministic
    const auto other = randomized({{"seed", 8}, {"strength", 50}});
    CHECK(other.damage != a.damage);
}

static void randomizer_strength_zero_changes_nothing() {
    const auto v = randomized({{"seed", 7}, {"strength", 0}});
    for (size_t id = 0; id < 20; ++id) CHECK(near(v.damage[id], 10.f * float(id + 1)) && v.capacity[id] == 15 && near(v.reload[id], 1.f) && near(v.rate[id], 1.f));
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 0}});   // nothing is pinned: a value the game writes later (an upgrade, another table) stays
    w.step(2);
    w.view.write_f32(kWeaponTable + 0x24, 99.f);
    w.step(3);
    CHECK(near(w.weapon_damage(0), 99.f));
}

static void randomizer_strength_100_stays_within_a_third_to_triple_and_the_limits() {
    for (int seed : {1, 2, 999999}) {
        const auto v = randomized({{"seed", seed}, {"strength", 100}});
        for (size_t id = 0; id < 20; ++id) {
            const float original = 10.f * float(id + 1);
            CHECK(std::isfinite(v.damage[id]) && v.damage[id] >= original / 3 - 1e-3f && v.damage[id] <= original * 3 + 1e-3f && v.damage[id] <= 10000.f);
            CHECK(v.capacity[id] >= 5 && v.capacity[id] <= 45);
            CHECK(v.reload[id] >= 1.f / 3 - 1e-3f && v.reload[id] <= 3.f + 1e-3f);
            CHECK(v.rate[id] >= 1.f / 3 - 1e-3f && v.rate[id] <= 3.f + 1e-3f);
        }
    }
}

static void randomizer_switching_and_replanning_start_from_the_game_values() {
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 50}});
    w.step(2);
    const auto first = weapon_values(w);
    w.change_setting("randomizer", "weapons", false);
    w.step(2);
    const auto off = weapon_values(w);
    for (size_t id = 0; id < 20; ++id) CHECK(near(off.damage[id], 10.f * float(id + 1)) && off.capacity[id] == 15 && near(off.reload[id], 1.f));
    w.change_setting("randomizer", "weapons", true);
    w.step(2);
    const auto back = weapon_values(w);
    CHECK(back.damage == first.damage && back.capacity == first.capacity && back.reload == first.reload);   // re-planned from the base: no compounding
    w.change_setting("randomizer", "strength", 100);
    w.step(2);
    const auto wild = weapon_values(w);
    CHECK(wild.damage != first.damage);
    for (size_t id = 0; id < 20; ++id) CHECK(wild.damage[id] >= 10.f * float(id + 1) / 3 - 1e-3f && wild.damage[id] <= 10.f * float(id + 1) * 3 + 1e-3f);
    w.change_setting("randomizer", "strength", 50);
    w.step(2);
    const auto again = weapon_values(w);
    CHECK(again.damage == first.damage);
}

static void randomizer_window_shows_the_seed_and_new_seed_changes_it() {
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 50}});
    w.ui.open_headers = true;
    w.step(2);
    const auto first = weapon_values(w);
    CHECK(w.mods->host()->status()[0].has_ui && w.mods->host()->draw_ui("randomizer").ok);
    CHECK(w.ui.has("Seed 7  "));   // (two spaces: a new seed that merely starts with 7 must not match)
    w.ui.press.insert("New seed");
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("randomizer").ok);
    w.ui.press.clear();
    w.ui.texts.clear();
    CHECK(w.mods->host()->draw_ui("randomizer").ok && !w.ui.has("Seed 7  ") && w.ui.has("Seed "));
    w.step(2);
    CHECK(weapon_values(w).damage != first.damage);   // the weapons follow the new seed
}

namespace {
constexpr uint32_t kActorTable = 0x804421C4, kZombieClass = 0x803F2B60, kZombieA = 0x80300000, kZombieB = 0x80301000;
void put_zombie(World& w, uint32_t address, uint32_t id, float hp) {
    w.view.write_u32(address, id);
    w.view.write_u32(address + 0x10, kZombieClass);
    w.view.write_f32(address + 0xF48, 1.f); w.view.write_f32(address + 0xF4C, 2.f); w.view.write_f32(address + 0xF50, 3.f);
    w.view.write_f32(address + 0x3A4, hp);
    w.view.write_f32(address + 0x1058, 1.f);
    w.view.write_u32(kActorTable + id * 8, id);
    w.view.write_u32(kActorTable + id * 8 + 4, address);
}
void remove_zombie(World& w, uint32_t id) { w.view.write_u32(kActorTable + id * 8, 0); w.view.write_u32(kActorTable + id * 8 + 4, 0); }
float hp_of(World& w, uint32_t address) { return w.view.read_f32(address + 0x3A4).value(); }

std::pair<float, float> zombies_after(const ordered_json& settings, float a, float b) {   // hit points of two zombies that were there before the mod looked
    World w("randomizer", "randomizer", settings);
    put_zombie(w, kZombieA, 10, a);
    put_zombie(w, kZombieB, 11, b);
    w.step(14);   // the periodic scan runs every sixth step
    return {hp_of(w, kZombieA), hp_of(w, kZombieB)};
}
}  // namespace

static void randomizer_zombies_follow_the_seed() {
    const auto [a, b] = zombies_after({{"seed", 7}, {"strength", 100}}, 20.f, 30.f);
    CHECK(a >= 20.f / 3 - 0.1f && a <= 60.f + 0.1f && b >= 10.f - 0.1f && b <= 90.f + 0.1f);
    CHECK(std::fabs(a - 20.f) > 0.05f || std::fabs(b - 30.f) > 0.05f);
    CHECK(zombies_after({{"seed", 7}, {"strength", 100}}, 20.f, 30.f) == std::make_pair(a, b));   // deterministic
    CHECK(zombies_after({{"seed", 8}, {"strength", 100}}, 20.f, 30.f) != std::make_pair(a, b));
    CHECK(zombies_after({{"seed", 7}, {"strength", 100}, {"enemies", false}}, 20.f, 30.f) == std::make_pair(20.f, 30.f));
}

static void randomizer_strength_zero_leaves_zombies_exactly_as_they_are() {
    CHECK(zombies_after({{"seed", 7}, {"strength", 0}}, 20.f, 0.04f) == std::make_pair(20.f, 0.04f));   // a nearly dead zombie is not raised to 1 either
    const auto [a, b] = zombies_after({{"seed", 7}, {"strength", 100}}, 0.f, 0.04f);
    CHECK(a == 0.f && b > 0.f && b <= 0.04f * 3 + 0.1f);   // a dead zombie is left alone and a hurt one stays alive
}

static void randomizer_zombie_ids_are_reused_by_later_waves() {
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 100}});
    put_zombie(w, kZombieA, 10, 20.f);
    w.step(14);
    const float first = hp_of(w, kZombieA);
    CHECK(std::fabs(first - 20.f) > 0.05f);
    remove_zombie(w, 10);                 // the wave ends
    w.step(2);
    put_zombie(w, kZombieA, 10, 25.f);    // the next wave brings a new zombie with the same id
    w.step(14);
    CHECK(std::fabs(hp_of(w, kZombieA) - 25.f) > 0.05f);   // randomized again, from its own hit points
}

static void randomizer_start_weapon_that_cannot_be_given_is_not_reported() {
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 50}, {"start_weapon", true}});
    w.ui.open_headers = true;
    w.step(100);   // the rig has no guest code: give_weapon fails
    CHECK(w.mods->host()->draw_ui("randomizer").ok);
    CHECK(!w.ui.has("started with") && w.ui.has("none given yet"));
}

static void randomizer_outside_a_mission_is_plain_text() {
    World w("randomizer", "randomizer", {{"seed", 7}, {"strength", 50}});
    w.ui.open_headers = true;
    for (uint32_t id = 0; id < 20; ++id) w.view.write_f32(kWeaponTable + id * kStride + 0x24, 0.f);   // the game has not filled its table
    w.view.write_u32(kPlayerTable, 0);
    w.view.write_u32(kPlayerTable + 4, 0);
    w.view.write_u32(kScenePointer, 0);
    w.step(3);
    CHECK(w.mods->host()->draw_ui("randomizer").ok && !w.ui.texts.empty());
}

static void randomizer_settings_are_declared() {
    std::ifstream in(fs::path(UC_EXAMPLES_DIR) / "randomizer" / "mod.json");
    const auto manifest = nlohmann::json::parse(in);
    CHECK(manifest["api"] == "1.1" && manifest["touches_gameplay"] == true);
    std::map<std::string, nlohmann::json> by_key;
    for (const auto& setting : manifest["settings"]) by_key[setting["key"]] = setting;
    CHECK(by_key.size() == 5);
    CHECK(by_key["seed"]["type"] == "int" && by_key["seed"]["min"] == 1 && by_key["seed"]["max"] == 999999 && by_key["seed"]["default"] == 1);
    CHECK(by_key["strength"]["type"] == "int" && by_key["strength"]["min"] == 0 && by_key["strength"]["max"] == 100 && by_key["strength"]["step"] == 5 && by_key["strength"]["default"] == 50);
    CHECK(by_key["weapons"]["type"] == "bool" && by_key["weapons"]["default"] == true);
    CHECK(by_key["enemies"]["type"] == "bool" && by_key["enemies"]["default"] == true);
    CHECK(by_key["start_weapon"]["type"] == "bool" && by_key["start_weapon"]["default"] == false);
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
                                                         {"pistol_autofire", pistol_autofire}, {"damage_multiplier", damage_multiplier}, {"faster_handgun", faster_handgun},
                                                         {"inspector_in_a_mission", inspector_in_a_mission}, {"inspector_outside_a_mission", inspector_outside_a_mission},
                                                         {"memory_watch_reads_what_is_typed", memory_watch_reads_what_is_typed}, {"events_are_logged_and_trimmed", events_are_logged_and_trimmed},
                                                         {"events_kept_can_be_lowered_at_once", events_kept_can_be_lowered_at_once}, {"snapshot_is_one_line_of_json", snapshot_is_one_line_of_json},
                                                         {"log_every_writes_snapshots", log_every_writes_snapshots}, {"log_every_waits_while_the_game_is_paused", log_every_waits_while_the_game_is_paused}, {"inspector_settings_are_declared", inspector_settings_are_declared},
                                                         {"randomizer_weapons_follow_the_seed", randomizer_weapons_follow_the_seed}, {"randomizer_strength_zero_changes_nothing", randomizer_strength_zero_changes_nothing},
                                                         {"randomizer_strength_100_limits", randomizer_strength_100_stays_within_a_third_to_triple_and_the_limits},
                                                         {"randomizer_switching_and_replanning", randomizer_switching_and_replanning_start_from_the_game_values},
                                                         {"randomizer_window", randomizer_window_shows_the_seed_and_new_seed_changes_it},
                                                         {"randomizer_zombies_follow_the_seed", randomizer_zombies_follow_the_seed}, {"randomizer_strength_zero_zombies", randomizer_strength_zero_leaves_zombies_exactly_as_they_are},
                                                         {"randomizer_zombie_ids_are_reused", randomizer_zombie_ids_are_reused_by_later_waves}, {"randomizer_start_weapon_failure", randomizer_start_weapon_that_cannot_be_given_is_not_reported}, {"randomizer_outside_a_mission", randomizer_outside_a_mission_is_plain_text},
                                                         {"randomizer_settings_are_declared", randomizer_settings_are_declared}};
    for (const auto& [name, run] : tests) {
        std::printf("%s\n", name);
        std::fflush(stdout);
        try { run(); } catch (const std::exception& e) { std::fprintf(stderr, "%s threw: %s\n", name, e.what()); return 1; }
    }
    std::puts("ok");
}
