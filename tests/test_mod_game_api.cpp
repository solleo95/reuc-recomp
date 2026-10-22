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

static void camera_view() {
    Fixture f;
    for (int i = 0; i < 12; ++i) f.memory.write_f32(kCamera + 4u * uint32_t(i), float(i) * 0.5f);
    const auto view = f.get("camera.view");
    CHECK(view.is_array() && view.size() == 12 && view[0] == 0.0 && view[3] == 1.5 && view[11] == 5.5);
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
    CHECK(!f.api.find("weapon.fire_rate"));   // found in the table but its effect is not understood: not exposed
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

int main() {
    overlapping_weapon_values_restore_in_order();
    effects_belong_to_the_mod_that_set_them(); concurrent_setters_and_ticks();
    weapon_changes_are_kept_when_the_game_rewrites_the_table();
    weapon_records_roundtrip();
    player_present_and_missing(); magazine_and_grenades_roundtrip(); damage_roundtrip_both_slots(); invulnerable_zeroes_damage_each_tick();
    infinite_ammo_refills(); events_from_changes_between_ticks(); camera_view(); enemies_list_parses_actor_table(); enemies_list_survives_a_loop_in_the_actor_list();
    std::puts("ok");
}
