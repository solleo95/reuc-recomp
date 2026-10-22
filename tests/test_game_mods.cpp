#include "check.h"
#include "audio/brstm.h"
#include "game_mods.h"
#include "guest_mem.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

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

static std::string tone_wav(uint32_t rate, double seconds) {   // mono 16-bit WAV of a 440 Hz sine
    const uint32_t frames = uint32_t(rate * seconds);
    std::string d;
    const auto le32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(char(v >> (8 * i))); };
    const auto le16 = [&](uint32_t v) { d.push_back(char(v)); d.push_back(char(v >> 8)); };
    d += "RIFF"; le32(36 + frames * 2); d += "WAVEfmt "; le32(16); le16(1); le16(1); le32(rate); le32(rate * 2); le16(2); le16(16);
    d += "data"; le32(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) le16(uint16_t(int16_t(8000 * std::sin(6.2831853 * 440 * i / rate))));
    return d;
}

// A mod's audio/<stream>.wav replaces the disc stream of that name once the converter is done (the disc file was a 48 kHz mono BRSTM, so the result
// must be 48 kHz mono), and switching the mod off puts the original back.
static void audio_of_a_mod_replaces_a_disc_stream() {
    uc::guest_mem_init();
    const auto exe = fresh("gm_audio");
    uc::mods::PcmAudio original;
    original.rate = 48000;
    original.channels = {std::vector<int16_t>(4800, 100)};
    const auto original_file = uc::mods::write_brstm_pcm16(original);
    put(exe / "disc" / "Data" / "sound" / "stream" / "music.brstm", std::string(original_file.begin(), original_file.end()));
    put(exe / "mods" / "tune" / "mod.json", R"({"id":"tune","name":"Tune","version":"1.0","api":"1.1"})");
    put(exe / "mods" / "tune" / "audio" / "Music.wav", tone_wav(44100, 0.5));
    put(exe / "mods" / "tune" / "audio" / "other.wav", tone_wav(44100, 0.1));   // no such stream on the disc
    uc::DvdFs dvd;
    CHECK(dvd.open_root(exe / "disc"));
    uc::GameMods mods(exe, enabled({"tune"}), dvd, "RBUE08", false);
    const auto length = [&] { return dvd.length(dvd.entry_of("Data/sound/stream/music.brstm")); };
    CHECK(length() == original_file.size());   // until the conversion is done the original plays
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (length() == original_file.size() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(length() != original_file.size());
    std::vector<uint8_t> bytes(length());
    CHECK(dvd.read(dvd.entry_of("Data/sound/stream/music.brstm"), bytes.data(), uint32_t(bytes.size()), 0) == int64_t(bytes.size()));
    const auto converted = uc::mods::read_brstm(bytes);
    CHECK(converted && converted->rate == 48000 && converted->channels.size() == 1 && std::abs(int(converted->frames()) - 24000) <= 1);
    CHECK(mods.audio_status("tune").find("other.wav") != std::string::npos);   // the stray file is reported, the rest works
    mods.registry()->set_enabled("tune", false);
    mods.apply();
    CHECK(length() == original_file.size());   // switched off: the original again
}

int main() {
    audio_of_a_mod_replaces_a_disc_stream();
    mods_switch_on_and_off_while_the_game_runs();
    log_is_capped_and_bad_config_is_reported();
    applies_overlay_and_runs_scripts(); nothing_without_the_folder_or_when_disabled(); mod_not_enabled_stays_off(); game_api_ticks_through_the_host(); textures_of_mods_are_indexed();
    std::puts("ok");
}
