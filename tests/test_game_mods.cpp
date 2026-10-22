#include "check.h"
#include "game_mods.h"
#include "guest_mem.h"
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::ordered_json;

static fs::path fresh(const char* name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

static void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// exe folder with a disc tree and, optionally, one Lua mod that counts game steps and replaces a disc file.
static fs::path setup(const char* name, bool mod_present = true, bool gameplay = false) {
    const auto exe = fresh(name);
    put(exe / "disc" / "DATA" / "a.bin", "abc");
    if (mod_present) {
        put(exe / "mods" / "counter" / "mod.json", std::string(R"({"id":"counter","name":"Counter","version":"1.0","api":"1.0","script":"scripts/main.lua")") + (gameplay ? R"(,"touches_gameplay":true})" : "}"));
        put(exe / "mods" / "counter" / "scripts" / "main.lua",
            "on('on_step', function() uc.mem.write_u32(0x80000400, uc.mem.read_u32(0x80000400) + 1) end)\n"
            "on('on_load', function() uc.mem.write_u32(0x80000404, 7) end)");
        put(exe / "mods" / "counter" / "files" / "DATA" / "a.bin", "replaced!");
    }
    return exe;
}

static ordered_json enabled(std::initializer_list<const char*> ids) {
    ordered_json section = {{"enabled", ordered_json::array()}};
    for (const char* id : ids) section["enabled"].push_back(id);
    return section;
}

static void applies_overlay_and_runs_scripts() {
    uc::guest_mem_init();
    uint8_t* memory = uc::guest_mem_base();
    const auto exe = setup("gm_full", true, true);   // it writes game memory: a gameplay mod (explicit save root, as the harness has, so no save folder is made)
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, enabled({"counter"}), dvd, "RBUE08", false);
    CHECK(mods.active());
    CHECK(dvd.length(dvd.entry_of("DATA/a.bin")) == 9);   // the overlay is in before the game opens anything
    mods.start(memory, nullptr);
    CHECK(mods.host() && !mods.host()->empty());
    uc::mods::GuestMemory view(memory);
    CHECK(view.read_u32(0x80000404) == 7u);               // on_load ran
    view.write_u32(0x80000400, 0);
    uc::mods::mods_step(); uc::mods::mods_step();         // what hle_GameFrameWait does once per step
    CHECK(view.read_u32(0x80000400) == 2u);
    mods.shutdown();
    uc::mods::mods_step();                                 // no host any more: harmless
    CHECK(view.read_u32(0x80000400) == 2u);
}

static void nothing_without_the_folder_or_when_disabled() {
    uc::guest_mem_init();
    for (const bool disabled : {false, true}) {
        const auto exe = setup("gm_none", disabled);   // no mods folder at all / a mod present but --no-mods
        uc::DvdFs dvd;
        CHECK(dvd.open_root(exe / "disc"));
        uc::GameMods mods(exe, enabled({"counter"}), dvd, "RBUE08", disabled);
        CHECK(!mods.active() && !mods.registry());
        CHECK(dvd.length(dvd.entry_of("DATA/a.bin")) == 3);
        mods.start(uc::guest_mem_base(), nullptr);
        CHECK(!mods.host());
        mods.shutdown();
    }
}

static void mod_not_enabled_stays_off() {
    uc::guest_mem_init();
    const auto exe = setup("gm_off");
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, ordered_json::object(), dvd, "RBUE08", false);
    CHECK(!mods.active() && mods.registry() && dvd.length(dvd.entry_of("DATA/a.bin")) == 3);
    mods.start(uc::guest_mem_base(), nullptr);
    CHECK(mods.host() && mods.host()->running_count() == 0);   // the host is there (mods can be switched on live) but no runtime exists
}

// Everything the manager can change takes effect while the game runs, gameplay mods included: the file overlay, the texture table and the scripts.
static void mods_switch_on_and_off_while_the_game_runs() {
    uc::guest_mem_init();
    uint8_t* memory = uc::guest_mem_base();
    uc::mods::GuestMemory view(memory);
    const auto exe = setup("gm_live", true, true);   // the counter mod changes gameplay (it writes game memory)
    put(exe / "mods" / "counter" / "textures" / "tex1_4x4_f7c67301db6713f0_6.dds", "not a real dds: only the name is indexed");
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, enabled({}), dvd, "RBUE08", false);   // nothing is on at startup
    gx::TextureReplacements table;
    int textures_raised = 0;
    mods.set_texture_handler([&] { ++textures_raised; });
    CHECK(!mods.active() && mods.load_textures(table) == 0 && table.empty());
    mods.start(memory, nullptr);
    CHECK(mods.host() && mods.host()->running_count() == 0 && dvd.length(dvd.entry_of("DATA/a.bin")) == 3);
    view.write_u32(0x80000400, 0);
    uc::mods::mods_step();
    CHECK(view.read_u32(0x80000400) == 0u);                              // nothing runs

    mods.registry()->set_enabled("counter", true);
    mods.apply();
    CHECK(mods.active() && mods.host()->running_count() == 1);           // a gameplay mod starts at once, nothing to wait for
    CHECK(dvd.length(dvd.entry_of("DATA/a.bin")) == 9);                  // the overlay is in
    CHECK(!table.empty() && textures_raised == 1);                       // the textures too
    view.write_u32(0x80000400, 0);
    uc::mods::mods_step(); uc::mods::mods_step();
    CHECK(view.read_u32(0x80000400) == 2u);                              // and the script runs

    mods.registry()->set_enabled("counter", false);
    mods.apply();
    CHECK(!mods.active() && mods.host()->running_count() == 0);
    CHECK(dvd.length(dvd.entry_of("DATA/a.bin")) == 3 && table.empty());   // everything is back
    uc::mods::mods_step();
    CHECK(view.read_u32(0x80000400) == 2u);
    mods.registry()->set_enabled("counter", true);
    mods.apply();
    CHECK(mods.host()->running_count() == 1);                            // and on again
    mods.shutdown();
}

// The game API's per-step half runs through the host: a script switches invulnerability on, the next game step zeroes the damage meter,
// and a drop of the magazine reaches the script as on_weapon_fired.
static void game_api_ticks_through_the_host() {
    uc::guest_mem_init();
    uint8_t* memory = uc::guest_mem_base();
    uc::mods::GuestMemory view(memory);
    constexpr uint32_t kP1 = 0x80100000, kScene = 0x80200000;
    std::memset(memory + 0x100000, 0, 0x200000);
    view.write_u32(0x806C3AD8, kP1); view.write_u32(0x806C3AD8 + 4, 0); view.write_u32(0x806C3B78, kScene);
    view.write_u32(kP1 + 0x4D4, 15); view.write_f32(kScene + 0x6C00, 20.f); view.write_u32(0x80000500, 0);
    const auto exe = fresh("gm_api");
    put(exe / "disc" / "DATA" / "a.bin", "abc");
    put(exe / "mods" / "god" / "mod.json", R"({"id":"god","name":"God","version":"1.0","api":"1.0","touches_gameplay":true,"script":"scripts/main.lua"})");
    put(exe / "mods" / "god" / "scripts" / "main.lua",
        "uc.player(1).invulnerable = true\n"
        "on('on_weapon_fired', function(p) uc.mem.write_u32(0x80000500, uc.mem.read_u32(0x80000500) + p.slot) end)");
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, enabled({"god"}), dvd, "RBUE08", false);
    mods.start(memory, nullptr);
    CHECK(mods.host() && !mods.host()->status()[0].disabled_for_session);
    uc::mods::mods_step();                                  // first step: baseline, damage zeroed by the invulnerable switch
    CHECK(view.read_f32(kScene + 0x6C00) == 0.f && view.read_u32(0x80000500) == 0u);
    view.write_u32(kP1 + 0x4D4, 14);
    uc::mods::mods_step();
    CHECK(view.read_u32(0x80000500) == 1u);                 // on_weapon_fired {slot = 1} reached the script
    mods.shutdown();
}

static void textures_of_mods_are_indexed() {
    const auto exe = setup("gm_textures");
    put(exe / "mods" / "counter" / "textures" / "tex1_4x4_00ff00ff00ff00ff_6.dds", "not a real dds, only the name is indexed");
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, enabled({"counter"}), dvd, "RBUE08", false);
    gx::TextureReplacements table;
    CHECK(mods.load_textures(table) == 1);
}

static std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A chatty mod cannot fill the disk, and a hand-edited config section that is not an object is reported instead of ignored silently.
static void log_is_capped_and_bad_config_is_reported() {
    uc::guest_mem_init();
    const auto exe = fresh("gm_log");
    put(exe / "disc" / "DATA" / "a.bin", "abc");
    put(exe / "mods" / "chatty" / "mod.json", R"({"id":"chatty","name":"Chatty","version":"1.0","api":"1.0","script":"scripts/main.lua"})");
    put(exe / "mods" / "chatty" / "scripts" / "main.lua", "for i = 1, 5000 do log.write('line ' .. i) end");
    ordered_json section = enabled({"chatty"});
    section["chatty"] = 5;   // hand-edited: mods.chatty is not an object
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, section, dvd, "RBUE08", false);
    mods.start(uc::guest_mem_base(), nullptr);
    mods.shutdown();
    std::string text = slurp(exe / "debug" / "mods.log");
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());   // the log is a text file: CRLF on Windows
    const auto lines = size_t(std::count(text.begin(), text.end(), '\n'));
    CHECK(lines > 100 && lines < 2100);
    CHECK(text.find("[chatty] line 1\n") != std::string::npos && text.find("line 5000") == std::string::npos && text.find("log limit") != std::string::npos);
    CHECK(text.find("mods.chatty") != std::string::npos);
}

int main() {
    mods_switch_on_and_off_while_the_game_runs();
    log_is_capped_and_bad_config_is_reported();
    applies_overlay_and_runs_scripts(); nothing_without_the_folder_or_when_disabled(); mod_not_enabled_stays_off(); game_api_ticks_through_the_host(); textures_of_mods_are_indexed();
    std::puts("ok");
}
