#include "check.h"
#include "game_api.h"
#include "guest_mem.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

using namespace uc::mods;
using nlohmann::json;

namespace {
// RBUE08 addresses, confirmed with the harness (docs/spikes/M1-weapon-data.md).
constexpr uint32_t kPlayerTable = 0x806C3AD8, kScenePointer = 0x806C3B78, kActorTable = 0x804421C4, kCamera = 0x8044B4E0, kGrenades = 0x8056F338;
constexpr uint32_t kP1 = 0x80100000, kP2 = 0x80101000, kScene = 0x80200000, kZombie1 = 0x80300000, kZombie2 = 0x80301000, kPool = 0x80302000, kLinked = 0x80303000;
constexpr uint32_t kZombieClass = 0x803F2B60, kPlayerClass = 0x803F2388;

struct Fixture {
    ApiRegistry api;
    GuestMemory memory;
    std::shared_ptr<GameApiTicker> ticker;
    std::vector<std::pair<std::string, json>> events;
    Fixture() : memory((uc::guest_mem_init(), uc::guest_mem_base())) {
        std::memset(uc::guest_mem_base() + 0x100000, 0, 0x300000);
        memory.write_u32(kPlayerTable, kP1);
        memory.write_u32(kPlayerTable + 4, kP2);
        memory.write_u32(kScenePointer, kScene);
        memory.write_u32(kP1 + 0x10, kPlayerClass);
        memory.write_u32(kP2 + 0x10, kPlayerClass);
        memory.write_u32(kP1 + 0x4D4, 15);
        memory.write_u32(kP2 + 0x4D4, 8);
        ticker = register_game_api(api, memory, SymbolTable{[](std::string_view) { return std::optional<uint32_t>(); }, [](uint32_t) { return std::string(); }});
    }
    json call(const std::string& path, json args = json::object()) { return api.find(path)->call(args); }
    json get(const std::string& path, json args = json::object()) { return api.find(path)->get(args); }
    void set(const std::string& path, json args, json value) { api.find(path)->set(args, value); }
    void tick() { ticker->tick([this](std::string_view event, const json& payload) { events.emplace_back(std::string(event), payload); }); }
    bool throws(const std::function<void()>& f) { try { f(); } catch (const ApiError&) { return true; } return false; }
};
json slot(int n) { return {{"slot", n}}; }
}  // namespace

static void player_present_and_missing() {
    Fixture f;
    CHECK(f.get("player.present", slot(1)) == true && f.get("player.present", slot(2)) == true);
    f.memory.write_u32(kPlayerTable + 4, 0);
    CHECK(f.get("player.present", slot(2)) == false);
    CHECK(f.throws([&] { f.get("player.magazine", slot(2)); }));   // a missing player is an error that says so, not a crash
    f.memory.write_u32(kPlayerTable, 0x12345678);   // a pointer outside guest memory
    CHECK(f.get("player.present", slot(1)) == false && f.throws([&] { f.set("player.magazine", slot(1), 5); }));
    CHECK(f.throws([&] { f.get("player.present", slot(3)); }) && f.throws([&] { f.get("player.present", json::object()); }) && f.throws([&] { f.get("player.present", {{"slot", "x"}}); }));
}

static void magazine_and_grenades_roundtrip() {
    Fixture f;
    CHECK(f.get("player.magazine", slot(1)) == 15 && f.get("player.magazine", slot(2)) == 8);
    f.set("player.magazine", slot(1), 99);
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 99u && f.get("player.magazine", slot(1)) == 99);
    CHECK(f.throws([&] { f.set("player.magazine", slot(1), -1); }) && f.throws([&] { f.set("player.magazine", slot(1), "many"); }) && f.throws([&] { f.set("player.magazine", slot(1), 70000); }));
    f.memory.write_u32(kGrenades, 3);
    CHECK(f.get("player.grenades", slot(1)) == 3);
    f.set("player.grenades", slot(2), 9);
    CHECK(f.memory.read_u32(kGrenades) == 9u && f.get("player.grenades", slot(1)) == 9);   // one stock for both players
}

static void damage_roundtrip_both_slots() {
    Fixture f;
    f.memory.write_f32(kScene + 0x6C00, 20.f);
    f.memory.write_f32(kScene + 0x6C14, 10.f);
    CHECK(f.get("player.damage", slot(1)) == 20.0 && f.get("player.damage", slot(2)) == 10.0);
    f.set("player.damage", slot(1), 0);
    f.set("player.damage", slot(2), 35.5);
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 0.f && f.memory.read_f32(kScene + 0x6C14) == 35.5f);
    f.memory.write_u32(kScenePointer, 0);   // no scene (menu, loading)
    CHECK(f.throws([&] { f.get("player.damage", slot(1)); }));
}

static void invulnerable_zeroes_damage_each_tick() {
    Fixture f;
    f.memory.write_f32(kScene + 0x6C00, 20.f);
    f.tick();
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 20.f);   // off by default
    f.set("player.invulnerable", slot(1), true);
    CHECK(f.get("player.invulnerable", slot(1)) == true && f.get("player.invulnerable", slot(2)) == false);
    f.tick();
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 0.f);
    f.memory.write_f32(kScene + 0x6C00, 30.f);
    f.tick();
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 0.f);
    f.set("player.invulnerable", slot(1), false);
    f.memory.write_f32(kScene + 0x6C00, 30.f);
    f.tick();
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 30.f);
    f.memory.write_u32(kScenePointer, 0);   // no scene: a tick must not touch memory or throw
    f.set("player.invulnerable", slot(1), true);
    f.tick();
}

static void infinite_ammo_refills() {
    Fixture f;
    f.set("player.infinite_ammo", slot(1), true);
    f.tick();                                           // learns the full magazine: 15
    f.memory.write_u32(kP1 + 0x4D4, 14);
    f.tick();
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 15u);
    f.memory.write_u32(kP1 + 0x4D4, 0);
    f.tick();
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 15u);
    f.memory.write_u32(kP1 + 0x4C8, 5);                 // another weapon with a smaller magazine
    f.memory.write_u32(kP1 + 0x4D4, 8);
    f.tick();
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 8u);        // not raised to the old weapon's 15
    f.memory.write_u32(kP1 + 0x4D4, 7);
    f.tick();
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 8u);
    CHECK(f.memory.read_u32(kP2 + 0x4D4) == 8u);        // player 2 untouched
    f.set("player.infinite_ammo", slot(1), false);
    f.memory.write_u32(kP1 + 0x4D4, 3);
    f.tick();
    CHECK(f.memory.read_u32(kP1 + 0x4D4) == 3u);
}

static void events_from_changes_between_ticks() {
    Fixture f;
    f.tick();
    CHECK(f.events.empty());                            // the first tick only records the state
    f.memory.write_u32(kP1 + 0x4D4, 14);
    f.tick();
    CHECK(f.events.size() == 1 && f.events[0].first == "on_weapon_fired" && f.events[0].second == json({{"slot", 1}}));
    f.events.clear();
    f.memory.write_u32(kP1 + 0x4D8, 2);                 // reload state
    f.tick();
    CHECK(f.events.size() == 1 && f.events[0].first == "on_reload" && f.events[0].second == json({{"slot", 1}}));
    f.events.clear();
    f.memory.write_u32(kP1 + 0x4D4, 15);                // the reload refills: not a shot
    f.memory.write_u32(kP1 + 0x4D8, 1);
    f.tick();
    CHECK(f.events.empty());
    f.memory.write_f32(kScene + 0x6C00, 10.f);
    f.tick();
    CHECK(f.events.size() == 1 && f.events[0].first == "on_damage_taken" && f.events[0].second == json({{"slot", 1}, {"amount", 10.0}}));
    f.events.clear();
    f.memory.write_f32(kScene + 0x6C00, 0.f);           // healing is not damage
    f.memory.write_u32(kP2 + 0x4D4, 7);
    f.tick();
    CHECK(f.events.size() == 1 && f.events[0].second["slot"] == 2);
    f.events.clear();
    f.memory.write_u32(kP1 + 0x4C8, 9);                 // a weapon switch changes the magazine without a shot
    f.memory.write_u32(kP1 + 0x4D4, 6);
    f.tick();
    CHECK(f.events.empty());
}

// Enemy and player events come from comparing the scene between ticks: spawned (a new id), killed (hp from above 0 to 0 or less), headshot (the head comes off), died.
static void enemy_and_death_events() {
    Fixture f;
    const auto zombie = [&](uint32_t address, uint32_t id, float hp) {
        f.memory.write_u32(address, id); f.memory.write_u32(address + 0x10, kZombieClass);
        f.memory.write_f32(address + 0xF48, 1.f); f.memory.write_f32(address + 0xF4C, 2.f); f.memory.write_f32(address + 0xF50, 3.f);
        f.memory.write_f32(address + 0x3A4, hp); f.memory.write_f32(address + 0x1058, 1.f);
    };
    const auto place = [&](uint32_t address, uint32_t id) { f.memory.write_u32(kActorTable + id * 8, id); f.memory.write_u32(kActorTable + id * 8 + 4, address); };
    const auto named = [&](const char* name) { std::vector<json> found; for (const auto& e : f.events) if (e.first == name) found.push_back(e.second); return found; };
    zombie(kZombie1, 10, 20.f); place(kZombie1, 10);
    f.tick();
    CHECK(f.events.empty());                                  // the first tick is the baseline: the zombies that are already there did not "spawn"
    zombie(kZombie2, 11, 30.f); place(kZombie2, 11);
    f.tick();
    CHECK(named("on_enemy_spawned").size() == 1 && named("on_enemy_spawned")[0]["id"] == 11 && named("on_enemy_spawned")[0]["hp"] == 30.0 && named("on_enemy_spawned")[0]["x"] == 1.0);
    f.events.clear();
    f.memory.write_f32(kZombie1 + 0x3A4, 10.f);               // hurt, not dead
    f.tick();
    CHECK(f.events.empty());
    f.memory.write_f32(kZombie1 + 0x1058, 0.1f);              // the head is shot off
    f.tick();
    CHECK(named("on_enemy_headshot").size() == 1 && named("on_enemy_headshot")[0]["id"] == 10 && named("on_enemy_killed").empty());
    f.events.clear();
    f.memory.write_f32(kZombie1 + 0x3A4, -5.f);               // and it dies; the head event does not repeat
    f.tick();
    CHECK(named("on_enemy_killed").size() == 1 && named("on_enemy_killed")[0]["id"] == 10 && named("on_enemy_headshot").empty());
    f.events.clear();
    f.tick();
    CHECK(f.events.empty());                                  // a corpse that stays in the table is not killed again
    f.memory.write_f32(kZombie2 + 0x3A4, 0.f); f.memory.write_f32(kZombie2 + 0x1058, 0.1f);   // one shot to the head kills at once: both events, the kill carries the id
    f.tick();
    CHECK(named("on_enemy_killed").size() == 1 && named("on_enemy_headshot").size() == 1);
    f.events.clear();
    f.memory.write_u32(kActorTable + 11 * 8, 0); f.memory.write_u32(kActorTable + 11 * 8 + 4, 0);   // the wave ends: a zombie leaves quietly
    f.tick();
    CHECK(f.events.empty());
    zombie(kZombie2, 11, 25.f); place(kZombie2, 11);          // the same id comes back (a new wave): a spawn again
    f.tick();
    CHECK(named("on_enemy_spawned").size() == 1);
    f.events.clear();
    f.memory.write_f32(kScene + 0x6C00, 49.f);                // 49 of 50: alive
    f.tick();
    CHECK(named("on_player_died").empty());
    f.memory.write_f32(kScene + 0x6C00, 50.f);
    f.tick();
    CHECK(named("on_player_died").size() == 1 && named("on_player_died")[0]["slot"] == 1);
    f.events.clear();
    f.tick();
    CHECK(named("on_player_died").empty());                   // once per death
    f.memory.write_f32(kScene + 0x6C00, 0.f);
    f.tick();
    f.memory.write_f32(kScene + 0x6C00, 50.f);
    f.tick();
    CHECK(named("on_player_died").size() == 1);               // revived (continue), dead again
    f.events.clear();
    f.memory.write_u32(kPlayerTable, 0);                      // no scene (menu): the baseline resets, nothing spawns when the next scene starts
    f.tick();
    f.memory.write_u32(kPlayerTable, kP1);
    f.tick();
    CHECK(named("on_enemy_spawned").empty() && named("on_enemy_killed").empty());
}

static void camera_view() {
    Fixture f;
    for (int i = 0; i < 12; ++i) f.memory.write_f32(kCamera + 4u * uint32_t(i), float(i) * 0.5f);
    const auto view = f.get("camera.view");
    CHECK(view.is_array() && view.size() == 12 && view[0] == 0.0 && view[3] == 1.5 && view[11] == 5.5);
}

// The game is on rails: the player's eye is the camera. Its position and axes come from the world-to-view matrix (rows = right, up, back).
static void camera_position_and_axes_from_the_view_matrix() {
    Fixture f;
    const float view[12] = {0.f, 0.f, -1.f, 0.95f,   0.f, 1.f, 0.f, -15.11f,   1.f, 0.f, 0.f, -539.45f};   // measured in the train chapter
    for (int i = 0; i < 12; ++i) f.memory.write_f32(kCamera + 4u * uint32_t(i), view[i]);
    const auto near = [](const json& v, double x, double y, double z) { return v.is_array() && v.size() == 3 && std::abs(v[0].get<double>() - x) < 1e-3 && std::abs(v[1].get<double>() - y) < 1e-3 && std::abs(v[2].get<double>() - z) < 1e-3; };
    CHECK(near(f.get("camera.position"), 539.45, 15.11, 0.95));
    CHECK(near(f.get("camera.right"), 0, 0, -1) && near(f.get("camera.up"), 0, 1, 0) && near(f.get("camera.forward"), -1, 0, 0));   // looking along -x, zombies stand at smaller x
    for (int i = 0; i < 12; ++i) f.memory.write_f32(kCamera + 4u * uint32_t(i), 0.f);                                              // no scene (menu, loading): the matrix is empty
    CHECK(f.throws([&] { f.get("camera.position"); }) && f.throws([&] { f.get("camera.forward"); }));
    CHECK(!f.api.find("player.position"));   // not offered: with two players on rails there is one viewpoint, see camera.position
}

// player.weapon (the id of the held weapon), player.health (the damage meter as a fraction) and player.reserve (the spare ammo of the held weapon).
static void weapon_health_and_reserve() {
    Fixture f;
    constexpr uint32_t kReserveTable = 0x8056A2D0 + 0x5064;   // u16 counts: 21 ammo keys per player, 42 bytes apart; key 0 means infinite
    f.memory.write_u32(kP1 + 0x4C8, 19); f.memory.write_u32(kP2 + 0x4C8, 2);
    CHECK(f.get("player.weapon", slot(1)) == 19 && f.get("player.weapon", slot(2)) == 2);
    CHECK(!f.api.find("player.weapon") || !f.api.find("player.weapon")->set);   // read only: the game's equip logic is not run by a write
    f.memory.write_f32(kScene + 0x6C00, 0.f); f.memory.write_f32(kScene + 0x6C14, 25.f);
    CHECK(f.get("player.health", slot(1)) == 1.0 && f.get("player.health", slot(2)) == 0.5);
    f.set("player.health", slot(1), 0.2);
    CHECK(std::abs(f.memory.read_f32(kScene + 0x6C00).value() - 40.f) < 1e-4);
    f.set("player.health", slot(2), 1);
    CHECK(f.memory.read_f32(kScene + 0x6C14) == 0.f);
    CHECK(f.throws([&] { f.set("player.health", slot(1), 1.5); }) && f.throws([&] { f.set("player.health", slot(1), -0.1); }) && f.throws([&] { f.set("player.health", slot(1), "full"); }));
    f.memory.write_u32(kP1 + 0x4CC, 0);   // the handgun: key 0, infinite reserve
    CHECK(f.get("player.reserve", slot(1)).is_null());
    CHECK(f.throws([&] { f.set("player.reserve", slot(1), 5); }));
    f.memory.write_u32(kP1 + 0x4CC, 7); f.memory.write_u32(kP2 + 0x4CC, 7);
    f.memory.write_u16(kReserveTable + 7 * 2, 437); f.memory.write_u16(kReserveTable + 42 + 7 * 2, 12);
    CHECK(f.get("player.reserve", slot(1)) == 437 && f.get("player.reserve", slot(2)) == 12);   // player 2 has its own set
    f.set("player.reserve", slot(1), 100); f.set("player.reserve", slot(2), 0);
    CHECK(f.memory.read_u16(kReserveTable + 7 * 2) == 100u && f.memory.read_u16(kReserveTable + 42 + 7 * 2) == 0u);
    CHECK(f.throws([&] { f.set("player.reserve", slot(1), -1); }) && f.throws([&] { f.set("player.reserve", slot(1), 10000); }) && f.throws([&] { f.set("player.reserve", slot(1), 1.5); }));
    f.memory.write_u32(kP1 + 0x4CC, 99);   // a key outside the table (0..20) is not trusted
    CHECK(f.throws([&] { f.get("player.reserve", slot(1)); }));
    f.memory.write_u32(kPlayerTable, 0);   // player 1 is not in the game
    CHECK(f.throws([&] { f.get("player.weapon", slot(1)); }) && f.throws([&] { f.get("player.reserve", slot(1)); }));
}

static void game_state_and_stats() {
    Fixture f;
    f.memory.write_u32(0x805C5D74, 125); f.memory.write_u32(0x804C77C4, 300); f.memory.write_u32(0x805965A8, 1);
    CHECK(f.get("game.time") == 125 && f.get("game.stars") == 300 && f.get("game.paused") == false);
    f.memory.write_u32(0x805965A8, 2);
    CHECK(f.get("game.paused") == true && !f.api.find("game.paused")->set);
    f.set("game.time", json::object(), 0); f.set("game.stars", json::object(), 9999);
    CHECK(f.memory.read_u32(0x805C5D74) == 0u && f.memory.read_u32(0x804C77C4) == 9999u);
    CHECK(f.throws([&] { f.set("game.time", json::object(), -1); }) && f.throws([&] { f.set("game.stars", json::object(), 10000); }) && f.throws([&] { f.set("game.time", json::object(), 1.5); }));
    f.memory.write_u32(0x805C5CC0, 7); f.memory.write_u32(0x805C5CC4, 9); f.memory.write_u32(0x805C5CC8, 2); f.memory.write_u32(0x805C5CD4, 4);
    CHECK(f.get("player.hits", slot(1)) == 7 && f.get("player.hits", slot(2)) == 9 && f.get("player.critical_hits", slot(1)) == 2 && f.get("player.objects_destroyed", slot(2)) == 4);
    f.set("player.hits", slot(2), 0); f.set("player.critical_hits", slot(1), 255);
    CHECK(f.memory.read_u32(0x805C5CC4) == 0u && f.memory.read_u32(0x805C5CC8) == 255u);
    CHECK(f.throws([&] { f.set("player.hits", slot(1), -1); }) && f.throws([&] { f.set("player.hits", slot(1), 100000); }));
}

static void enemies_list_parses_actor_table() {
    Fixture f;
    const auto zombie = [&](uint32_t address, uint32_t id, float x, float y, float z, float hp) {
        f.memory.write_u32(address, id);
        f.memory.write_u32(address + 0x10, kZombieClass);
        f.memory.write_f32(address + 0xF48, x); f.memory.write_f32(address + 0xF4C, y); f.memory.write_f32(address + 0xF50, z);
        f.memory.write_f32(address + 0x3A4, hp);
    };
    zombie(kZombie1, 10, 1.f, 2.f, 3.f, 20.f);
    zombie(kZombie2, 11, -5.f, 0.f, 7.5f, -10.f);
    f.memory.write_u32(kPool + 0x10, 0x803F1A40);        // a pooled non-enemy object
    f.memory.write_u32(kActorTable + 10 * 8, 10); f.memory.write_u32(kActorTable + 10 * 8 + 4, kZombie1);
    f.memory.write_u32(kActorTable + 11 * 8, 11); f.memory.write_u32(kActorTable + 11 * 8 + 4, kZombie2);
    f.memory.write_u32(kActorTable + 12 * 8, 12); f.memory.write_u32(kActorTable + 12 * 8 + 4, kPool);
    f.memory.write_u32(kActorTable + 2 * 8, 2);  f.memory.write_u32(kActorTable + 2 * 8 + 4, kP1);   // the player is not an enemy
    f.memory.write_u32(kActorTable + 13 * 8, 13); f.memory.write_u32(kActorTable + 13 * 8 + 4, 0x12345678);   // garbage pointer
    // An actor outside the 200-entry table (id >= 200) is reached through the player's linked list: object+8 is the next object + 8.
    zombie(kLinked, 604, 9.f, 9.f, 9.f, 30.f);
    f.memory.write_u32(kP1 + 8, kLinked + 8);
    const auto list = f.call("enemies.list");
    CHECK(list.is_array() && list.size() == 3);
    std::map<int, json> by_id;
    for (const auto& e : list) by_id[e["id"].get<int>()] = e;
    CHECK(by_id.count(10) && by_id.count(11) && by_id.count(604));
    CHECK(by_id[10]["x"] == 1.0 && by_id[10]["y"] == 2.0 && by_id[10]["z"] == 3.0 && by_id[10]["hp"] == 20.0 && by_id[10]["class"] == json(kZombieClass));
    CHECK(by_id[11]["hp"] == -10.0 && by_id[604]["z"] == 9.0);
    f.memory.write_f32(kZombie1 + 0xF48, std::nanf(""));   // a non-finite position is a broken object: skipped
    CHECK(f.call("enemies.list").size() == 2);
}

// uc.enemy(id): read and change one zombie by the id enemies.list() gives. Only zombies (the class with a known layout); anything else is refused.
static void enemy_members() {
    Fixture f;
    const auto zombie = [&](uint32_t address, uint32_t id, float hp) {
        f.memory.write_u32(address, id); f.memory.write_u32(address + 0x10, kZombieClass);
        f.memory.write_f32(address + 0xF48, 1.f); f.memory.write_f32(address + 0xF4C, 2.f); f.memory.write_f32(address + 0xF50, 3.f);
        f.memory.write_f32(address + 0x3A4, hp); f.memory.write_f32(address + 0x1058, 1.f);
    };
    zombie(kZombie1, 10, 20.f);
    f.memory.write_u32(kActorTable + 10 * 8, 10); f.memory.write_u32(kActorTable + 10 * 8 + 4, kZombie1);
    f.memory.write_u32(kPool + 0x10, 0x803F1A40);
    f.memory.write_u32(kActorTable + 12 * 8, 12); f.memory.write_u32(kActorTable + 12 * 8 + 4, kPool);   // not a zombie
    zombie(kLinked, 604, 30.f);                                                                          // outside the table, reached through the player's list
    f.memory.write_u32(kP1 + 8, kLinked + 8);
    const auto id = [](int n) { return json{{"id", n}}; };
    CHECK(f.get("enemy.hp", id(10)) == 20.0 && f.get("enemy.alive", id(10)) == true && f.get("enemy.head", id(10)) == true);
    CHECK(f.get("enemy.position", id(10)) == json::array({1.0, 2.0, 3.0}) && f.get("enemy.hp", id(604)) == 30.0);
    f.set("enemy.hp", id(10), 0);
    CHECK(f.memory.read_f32(kZombie1 + 0x3A4) == 0.f && f.get("enemy.alive", id(10)) == false);
    f.set("enemy.hp", id(604), 500.5);
    CHECK(f.memory.read_f32(kLinked + 0x3A4) == 500.5f);
    f.set("enemy.position", id(10), json::array({4.5, 0.0, -7.25}));
    CHECK(f.memory.read_f32(kZombie1 + 0xF48) == 4.5f && f.memory.read_f32(kZombie1 + 0xF50) == -7.25f);
    f.memory.write_f32(kZombie1 + 0x1058, 0.1f);
    CHECK(f.get("enemy.head", id(10)) == false);
    CHECK(f.throws([&] { f.get("enemy.hp", id(99)); }) && f.throws([&] { f.set("enemy.hp", id(99), 1); }));          // no such actor
    CHECK(f.throws([&] { f.get("enemy.hp", id(12)); }) && f.throws([&] { f.set("enemy.hp", id(12), 1); }));          // an actor of another class
    CHECK(f.throws([&] { f.get("enemy.hp", json::object()); }) && f.throws([&] { f.get("enemy.hp", json{{"id", "x"}}); }));
    CHECK(f.throws([&] { f.set("enemy.hp", id(10), -5000); }) && f.throws([&] { f.set("enemy.hp", id(10), 1e9); }) && f.throws([&] { f.set("enemy.hp", id(10), "dead"); }));
    CHECK(f.throws([&] { f.set("enemy.position", id(10), json::array({1.0, 2.0})); }) && f.throws([&] { f.set("enemy.position", id(10), json::array({1e9, 0.0, 0.0})); }));
    CHECK(f.memory.read_f32(kZombie1 + 0xF48) == 4.5f);   // refused writes change nothing
    CHECK(!f.api.find("enemy.alive")->set && !f.api.find("enemy.head")->set);
}

static void enemies_list_survives_a_loop_in_the_actor_list() {
    Fixture f;
    f.memory.write_u32(kP1 + 8, kP1 + 8);   // points at itself
    CHECK(f.call("enemies.list").empty());
    f.memory.write_u32(kP1 + 8, kLinked + 8); f.memory.write_u32(kLinked + 8, kP1 + 8);   // a two-object cycle
    CHECK(f.call("enemies.list").empty());
}

static json weapon(int id) { return {{"id", id}}; }

static void weapon_records_roundtrip() {
    Fixture f;
    constexpr uint32_t kTable = 0x8046A86C, kStride = 168;
    f.memory.write_u32(kTable + 0 * kStride + 0x14, 15);   // handgun: capacity
    f.memory.write_f32(kTable + 0 * kStride + 0x24, 10.f);
    f.memory.write_u32(kTable + 0 * kStride + 0x10, 1);
    f.memory.write_f32(kTable + 0 * kStride + 0x38, 1.f);
    f.memory.write_u32(kTable + 2 * kStride + 0x14, 50);   // submachine gun
    f.memory.write_u32(kTable + 2 * kStride + 0x10, 0);
    CHECK(f.get("weapon.capacity", weapon(0)) == 15 && f.get("weapon.capacity", weapon(2)) == 50);
    CHECK(f.get("weapon.damage", weapon(0)) == 10.0 && f.get("weapon.reload_time", weapon(0)) == 1.0);
    CHECK(f.api.find("weapon.fire_rate") && f.get("weapon.fire_rate", weapon(0)) == 1.0);
    CHECK(f.get("weapon.fire_mode", weapon(0)) == "semi" && f.get("weapon.fire_mode", weapon(2)) == "auto");
    f.set("weapon.capacity", weapon(0), 99);
    f.set("weapon.damage", weapon(0), 25.5);
    f.set("weapon.reload_time", weapon(0), 0.25);
    f.set("weapon.fire_mode", weapon(0), "auto");
    CHECK(f.memory.read_u32(kTable + 0x14) == 99u && f.memory.read_f32(kTable + 0x24) == 25.5f);
    CHECK(f.memory.read_f32(kTable + 0x38) == 0.25f && f.memory.read_u32(kTable + 0x10) == 0u && f.get("weapon.fire_mode", weapon(0)) == "auto");
    f.set("weapon.fire_mode", weapon(0), "semi");
    CHECK(f.memory.read_u32(kTable + 0x10) == 1u);
    f.memory.write_u32(kTable + 0x10, 3);
    CHECK(f.get("weapon.fire_mode", weapon(0)) == "burst");
    f.memory.write_u32(kTable + 0x10, 7);
    CHECK(f.get("weapon.fire_mode", weapon(0)) == "mode 7");   // a value the API does not name is shown, not hidden
    // Bad input is an error that changes nothing.
    CHECK(f.throws([&] { f.get("weapon.capacity", weapon(20)); }) && f.throws([&] { f.get("weapon.capacity", weapon(-1)); }) && f.throws([&] { f.get("weapon.capacity", json::object()); }));
    CHECK(f.throws([&] { f.set("weapon.damage", weapon(0), -1); }) && f.throws([&] { f.set("weapon.damage", weapon(0), "strong"); }) && f.throws([&] { f.set("weapon.damage", weapon(0), 1e9); }));
    CHECK(f.throws([&] { f.set("weapon.capacity", weapon(0), 100000); }) && f.throws([&] { f.set("weapon.fire_mode", weapon(0), "laser"); }) && f.throws([&] { f.set("weapon.reload_time", weapon(0), -3); }));
    CHECK(f.memory.read_f32(kTable + 0x24) == 25.5f);
}

// The game fills its weapon table when it boots and again at mission starts; a value a mod set must outlast that.
static void weapon_changes_are_kept_when_the_game_rewrites_the_table() {
    Fixture f;
    constexpr uint32_t kRecord = 0x8046A86C;
    f.memory.write_u32(kRecord + 0x14, 15);
    f.memory.write_f32(kRecord + 0x24, 10.f);
    f.memory.write_u32(kRecord + 0x10, 1);
    f.set("weapon.capacity", weapon(0), 30);
    f.set("weapon.damage", weapon(0), 40.0);
    f.set("weapon.fire_mode", weapon(0), "auto");
    CHECK(f.memory.read_u32(kRecord + 0x14) == 30u);                     // written at once ...
    f.memory.write_u32(kRecord + 0x14, 15);                              // ... and the game re-initialises the table
    f.memory.write_f32(kRecord + 0x24, 10.f);
    f.memory.write_u32(kRecord + 0x10, 1);
    f.tick();                                                            // the next game step puts the mod's values back
    CHECK(f.memory.read_u32(kRecord + 0x14) == 30u && f.memory.read_f32(kRecord + 0x24) == 40.f && f.memory.read_u32(kRecord + 0x10) == 0u);
    CHECK(f.get("weapon.capacity", weapon(0)) == 30);
    f.set("weapon.capacity", weapon(0), 20);                             // the latest value wins
    f.memory.write_u32(kRecord + 0x14, 15);
    f.tick();
    CHECK(f.memory.read_u32(kRecord + 0x14) == 20u);
    f.memory.write_u32(kRecord + 168 + 0x14, 6);                         // another record is not touched
    f.tick();
    CHECK(f.memory.read_u32(kRecord + 168 + 0x14) == 6u);
}

// What a mod set in the game is filed under that mod: dropping it restores the game's values and leaves other mods' settings alone.
static void effects_belong_to_the_mod_that_set_them() {
    Fixture f;
    constexpr uint32_t kRecord = 0x8046A86C;
    f.memory.write_u32(kRecord + 0x14, 15);
    f.memory.write_f32(kRecord + 0x24, 10.f);
    const std::string a = "a", b = "b";
    { OwnerScope scope(a); f.set("weapon.capacity", weapon(0), 30); f.set("player.invulnerable", slot(1), true); f.set("player.infinite_ammo", slot(1), true); }
    { OwnerScope scope(b); f.set("weapon.damage", weapon(0), 40.0); f.set("player.invulnerable", slot(1), true); }
    f.ticker->drop_owner(a);
    CHECK(f.memory.read_u32(kRecord + 0x14) == 15u && f.memory.read_f32(kRecord + 0x24) == 40.f);   // a's capacity is back, b's damage stays
    CHECK(f.get("player.invulnerable", slot(1)) == true && f.get("player.infinite_ammo", slot(1)) == false);   // b still wants invulnerability
    f.memory.write_u32(kRecord + 0x14, 16);   // the game rewrites the field: a's old value is not brought back
    f.tick();
    CHECK(f.memory.read_u32(kRecord + 0x14) == 16u);
    f.ticker->drop_owner(b);
    CHECK(f.memory.read_f32(kRecord + 0x24) == 10.f && f.get("player.invulnerable", slot(1)) == false);
    f.memory.write_f32(kScene + 0x6C00, 5.f);
    f.tick();
    CHECK(f.memory.read_f32(kScene + 0x6C00) == 5.f);   // nothing zeroes the damage meter any more
    f.ticker->drop_owner("nobody");                      // harmless
}

// The menu thread and the game thread both call the game API; the shared tables must survive that.
static void concurrent_setters_and_ticks() {
    Fixture f;
    std::atomic<bool> stop{false};
    std::thread setter([&] {
        const std::string owner = "a";
        for (int i = 0; i < 20000 && !stop; ++i) {
            OwnerScope scope(owner);
            f.set("weapon.capacity", weapon(i % 20), i % 900);
            f.set("player.invulnerable", slot(1 + i % 2), i % 2 == 0);
            if (i % 50 == 0) f.ticker->drop_owner(owner);
        }
    });
    for (int i = 0; i < 20000; ++i) f.tick();
    stop = true;
    setter.join();
    f.ticker->drop_owner("a");
    CHECK(f.get("player.invulnerable", slot(1)) == false && f.get("player.invulnerable", slot(2)) == false);
}

// Two mods set the same weapon field: switching the later one off puts the earlier one's value back, not the game's.
static void overlapping_weapon_values_restore_in_order() {
    Fixture f;
    constexpr uint32_t kRecord = 0x8046A86C;
    f.memory.write_u32(kRecord + 0x14, 15);
    const std::string a = "a", b = "b";
    { OwnerScope scope(a); f.set("weapon.capacity", weapon(0), 30); }
    { OwnerScope scope(b); f.set("weapon.capacity", weapon(0), 50); }
    CHECK(f.memory.read_u32(kRecord + 0x14) == 50u);
    f.ticker->drop_owner(b);
    CHECK(f.memory.read_u32(kRecord + 0x14) == 30u);
    f.memory.write_u32(kRecord + 0x14, 15);
    f.tick();
    CHECK(f.memory.read_u32(kRecord + 0x14) == 30u);   // still a's, also after the game refills the table
    f.ticker->drop_owner(a);
    CHECK(f.memory.read_u32(kRecord + 0x14) == 15u);
}

// fire_rate: the record's rate (+0x0C) is turned into a per-weapon interval (1 / rate, player + 0x4C4) when a weapon is equipped, and that interval is what
// sets the cadence. So the API writes both, keeps the live interval of players holding that weapon, and gives it back when the mod goes.
static void fire_rate_sets_the_live_interval() {
    Fixture f;
    constexpr uint32_t kRecord = 0x8046A86C, kInterval = 0x4C4, kWeaponId = 0x4C8;
    f.memory.write_f32(kRecord + 0x0C, 1.f);                 // handgun: one shot per second at rest
    f.memory.write_f32(kRecord + 19 * 168 + 0x0C, 1.f);
    f.memory.write_u32(kP1 + kWeaponId, 0); f.memory.write_f32(kP1 + kInterval, 1.f);
    f.memory.write_u32(kP2 + kWeaponId, 2); f.memory.write_f32(kP2 + kInterval, 0.5f);   // player 2 holds another weapon
    const std::string a = "a";
    { OwnerScope scope(a); f.set("weapon.fire_rate", weapon(0), 4.0); }
    CHECK(f.memory.read_f32(kRecord + 0x0C) == 4.f && f.memory.read_f32(kP1 + kInterval) == 0.25f);   // record and the live interval at once
    CHECK(f.memory.read_f32(kP2 + kInterval) == 0.5f);                                                 // another weapon is not touched
    f.get("weapon.fire_rate", weapon(0));
    CHECK(f.get("weapon.fire_rate", weapon(0)) == 4.0);
    f.memory.write_f32(kP1 + kInterval, 1.f);                 // the game recomputes the interval (a weapon change, a mission start)
    f.memory.write_f32(kRecord + 0x0C, 1.f);
    f.tick();
    CHECK(f.memory.read_f32(kRecord + 0x0C) == 4.f && f.memory.read_f32(kP1 + kInterval) == 0.25f);   // the next step puts both back
    CHECK(f.throws([&] { f.set("weapon.fire_rate", weapon(0), 0); }) && f.throws([&] { f.set("weapon.fire_rate", weapon(0), -2); }) && f.throws([&] { f.set("weapon.fire_rate", weapon(0), 1000); }) && f.throws([&] { f.set("weapon.fire_rate", weapon(0), "fast"); }));
    f.ticker->drop_owner(a);
    CHECK(f.memory.read_f32(kRecord + 0x0C) == 1.f && f.memory.read_f32(kP1 + kInterval) == 1.f);     // the game's own rate and interval are back
    CHECK(f.memory.read_f32(kP2 + kInterval) == 0.5f);
}

int main() {
    enemy_and_death_events();
    enemy_members();
    weapon_health_and_reserve();
    game_state_and_stats();
    camera_position_and_axes_from_the_view_matrix();
    camera_position_and_axes_from_the_view_matrix();
    fire_rate_sets_the_live_interval();
    overlapping_weapon_values_restore_in_order();
    effects_belong_to_the_mod_that_set_them(); concurrent_setters_and_ticks();
    weapon_changes_are_kept_when_the_game_rewrites_the_table();
    weapon_records_roundtrip();
    player_present_and_missing(); magazine_and_grenades_roundtrip(); damage_roundtrip_both_slots(); invulnerable_zeroes_damage_each_tick();
    infinite_ammo_refills(); events_from_changes_between_ticks(); camera_view(); enemies_list_parses_actor_table(); enemies_list_survives_a_loop_in_the_actor_list();
    std::puts("ok");
}
