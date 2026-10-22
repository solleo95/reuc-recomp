// player.*, camera.*, enemies.*: the game's structures as found for the NTSC-U release (RBUE08). Every address and offset here was confirmed
// with the test harness; see docs/spikes/G22-autoplay-memory.md and docs/spikes/M1-weapon-data.md.
#include "game_api.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace uc::mods {
namespace {
constexpr uint32_t kPlayerTable = 0x806C3AD8;   // two pointers (player 1, player 2), in the SDA (r13 - 27464)
constexpr uint32_t kScenePointer = 0x806C3B78;  // pointer to the scene object (r13 - 27304)
constexpr uint32_t kActorTable = 0x804421C4;    // 200 x (id, object pointer)
constexpr uint32_t kCameraView = 0x8044B4E0;    // world -> view, 3x4 floats, row-major
constexpr uint32_t kGrenades = 0x8056F338;      // grenade stock (one word for both players)

constexpr uint32_t kPlayerSize = 0x7A0;         // bytes of a player object the API reads
constexpr uint32_t kWeaponId = 0x4C8, kWeaponKey = 0x4CC, kMagazine = 0x4D4, kWeaponState = 0x4D8;   // player + 0x48 is the weapon object: +0x480, +0x484, +0x48C, +0x490
constexpr uint32_t kReloading = 2;              // weapon state while a reload runs
constexpr uint32_t kSceneSize = 0x6C18, kDamage = 0x6C00, kDamageStride = 0x14;   // damage meter of player 1 at scene+6C00, player 2 at +6C14 (0 unhurt .. 50 dead)
constexpr uint32_t kZombieClass = 0x803F2B60, kEnemyPosition = 0xF48, kEnemyHealth = 0x3A4, kEnemyHead = 0x1058, kActorClass = 0x10;
constexpr uint32_t kMaxMagazine = 65535;
// Spare ammo: u16 counts per ammo key (21 keys, 42 bytes per player), found through fn_801BD014 and confirmed by a reload taking one off the count. Key 0 is the
// handgun's infinite reserve. (The two words after the table's start hold the grenades at 8056F338: key 3.)
constexpr uint32_t kReserveTable = 0x8056F334, kReserveSetStride = 42, kReserveKeys = 21, kMaxReserve = 9999;
constexpr double kDamageDead = 50.0;   // the damage meter's value at which a player is dead

// The weapon table: 20 records of 168 bytes, found through the record accessor 8006C8C8 (id clamped to 0..19, 8046A86C + id * 168).
constexpr uint32_t kWeaponTable = 0x8046A86C, kWeaponStride = 168, kWeaponCount = 20;
constexpr uint32_t kRecordFireRate = 0x0C, kWeaponInterval = 0x4C4;   // the record's rate becomes the per-weapon interval 1 / rate (player + 0x4C4) when the weapon is equipped (fn 8006C7B0)
constexpr uint32_t kRecordFireMode = 0x10, kRecordCapacity = 0x14, kRecordDamage = 0x24, kRecordReloadTime = 0x38;
constexpr uint32_t kModeAuto = 0, kModeSemi = 1, kModeBurst = 3;   // values seen in the table: submachine gun 0, handgun/shotgun/others 1, 3 on record 3
constexpr long long kMaxCapacity = 999;

struct SlotState {
    bool invulnerable = false, infinite_ammo = false;
    bool known = false, has_damage = false, dead = false;
    uint32_t weapon = 0, state = 0, magazine = 0, max_magazine = 0;
    float damage = 0;
};

class PlayerWeaponApi : public GameApiTicker, public std::enable_shared_from_this<PlayerWeaponApi> {
public:
    explicit PlayerWeaponApi(GuestMemory& memory) : memory_(memory) {}

    void register_members(ApiRegistry& api) {
        const auto self = shared_from_this();
        const auto add_property = [&](const char* name, std::function<Value(PlayerWeaponApi&, int)> get, std::function<void(PlayerWeaponApi&, int, const Value&)> set) {
            Member member;
            member.path = std::string("player.") + name;
            member.scope = "slot";
            if (get) member.get = [self, get](const Value& args) { return get(*self, slot_of(args)); };
            if (set) member.set = [self, set](const Value& args, const Value& value) { set(*self, slot_of(args), value); };
            api.add(std::move(member));
        };
        add_property("present", [](PlayerWeaponApi& s, int slot) { return Value(s.player_object(slot).has_value()); }, nullptr);
        add_property("magazine",
            [](PlayerWeaponApi& s, int slot) { return Value(s.read(s.require_player(slot) + kMagazine)); },
            [](PlayerWeaponApi& s, int slot, const Value& v) { s.write(s.require_player(slot) + kMagazine, count_of(v, "magazine")); });
        add_property("grenades",
            [](PlayerWeaponApi& s, int) { return Value(s.read(kGrenades)); },
            [](PlayerWeaponApi& s, int, const Value& v) { s.write(kGrenades, count_of(v, "grenades")); });
        add_property("damage",
            [](PlayerWeaponApi& s, int slot) { return Value(double(s.read_float(s.damage_address(slot)))); },
            [](PlayerWeaponApi& s, int slot, const Value& v) {
                if (!v.is_number()) throw ApiError("damage must be a number");
                const double d = v.get<double>();
                if (!std::isfinite(d) || d < 0 || d > 1000) throw ApiError("damage must be between 0 and 1000 (0 is unhurt, 50 is dead)");
                if (!s.memory_.write_f32(s.damage_address(slot), float(d))) throw ApiError("the damage meter is not readable right now");
            });
        add_property("weapon",
            [](PlayerWeaponApi& s, int slot) { return Value(s.read(s.require_player(slot) + kWeaponId)); }, nullptr);
        add_property("health",
            [](PlayerWeaponApi& s, int slot) { return Value(std::clamp(1.0 - double(s.read_float(s.damage_address(slot))) / kDamageDead, 0.0, 1.0)); },
            [](PlayerWeaponApi& s, int slot, const Value& v) {
                if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < 0 || v.get<double>() > 1) throw ApiError("health must be a number from 0 (dead) to 1 (unhurt)");
                if (!s.memory_.write_f32(s.damage_address(slot), float((1.0 - v.get<double>()) * kDamageDead))) throw ApiError("the damage meter is not writable right now");
            });
        add_property("reserve",
            [](PlayerWeaponApi& s, int slot) {
                const uint32_t key = s.read(s.require_player(slot) + kWeaponKey);
                if (key == 0) return Value(nullptr);   // infinite
                return Value(s.read_u16(kReserveTable + uint32_t(slot - 1) * kReserveSetStride + s.checked_key(key) * 2));
            },
            [](PlayerWeaponApi& s, int slot, const Value& v) {
                const uint32_t key = s.read(s.require_player(slot) + kWeaponKey);
                if (key == 0) throw ApiError("this weapon has an infinite reserve (ammo key 0): there is nothing to set");
                if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > kMaxReserve) throw ApiError("reserve must be a whole number from 0 to 9999");
                if (!s.memory_.write_u16(kReserveTable + uint32_t(slot - 1) * kReserveSetStride + s.checked_key(key) * 2, uint16_t(v.get<long long>()))) throw ApiError("game memory is not writable right now");
            });
        add_property("invulnerable",
            [](PlayerWeaponApi& s, int slot) { std::lock_guard lock(s.mutex_); return Value(s.state_[size_t(slot - 1)].invulnerable); },
            [](PlayerWeaponApi& s, int slot, const Value& v) { s.set_switch(s.invulnerable_by_[size_t(slot - 1)], s.state_[size_t(slot - 1)].invulnerable, flag_of(v, "invulnerable")); });
        add_property("infinite_ammo",
            [](PlayerWeaponApi& s, int slot) { std::lock_guard lock(s.mutex_); return Value(s.state_[size_t(slot - 1)].infinite_ammo); },
            [](PlayerWeaponApi& s, int slot, const Value& v) { s.set_switch(s.infinite_ammo_by_[size_t(slot - 1)], s.state_[size_t(slot - 1)].infinite_ammo, flag_of(v, "infinite_ammo")); });

        // weapon.*: the 20 records of the game's weapon table, addressed by weapon type id (uc.weapon(0) is the handgun).
        const auto add_weapon = [&](const char* name, std::function<Value(PlayerWeaponApi&, uint32_t)> get, std::function<void(PlayerWeaponApi&, uint32_t, const Value&)> set) {
            Member member;
            member.path = std::string("weapon.") + name;
            member.scope = "id";
            member.get = [self, get](const Value& args) { return get(*self, weapon_record(args)); };
            member.set = [self, set](const Value& args, const Value& value) { set(*self, weapon_record(args), value); };
            api.add(std::move(member));
        };
        const auto real_field = [](const char* what, uint32_t offset, double high) {
            return std::pair{
                std::function<Value(PlayerWeaponApi&, uint32_t)>([offset](PlayerWeaponApi& s, uint32_t record) { return Value(double(s.read_float(record + offset))); }),
                std::function<void(PlayerWeaponApi&, uint32_t, const Value&)>([what, offset, high](PlayerWeaponApi& s, uint32_t record, const Value& v) {
                    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < 0 || v.get<double>() > high)
                        throw ApiError(std::string(what) + " must be a number from 0 to " + std::to_string(int(high)));
                    s.write_float(record + offset, float(v.get<double>()));
                })};
        };
        const auto damage = real_field("damage", kRecordDamage, 10000);
        add_weapon("damage", damage.first, damage.second);
        // fire_rate: shots per second. The game turns the record's rate into the weapon's interval (1 / rate) once, when the weapon is equipped, and the
        // interval is what sets the cadence of automatic fire (measured: shots in a fixed time follow 1 / interval). So both are kept: see refresh_intervals.
        add_weapon("fire_rate",
            [](PlayerWeaponApi& s, uint32_t record) { return Value(double(s.read_float(record + kRecordFireRate))); },
            [](PlayerWeaponApi& s, uint32_t record, const Value& v) {
                if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < 0.05 || v.get<double>() > 60) throw ApiError("fire_rate must be a number from 0.05 to 60 (shots per second)");
                s.write_float(record + kRecordFireRate, float(v.get<double>()));
                s.refresh_intervals();
            });
        const auto reload_time = real_field("reload_time", kRecordReloadTime, 1000);
        add_weapon("reload_time", reload_time.first, reload_time.second);
        add_weapon("capacity",
            [](PlayerWeaponApi& s, uint32_t record) { return Value(s.read(record + kRecordCapacity)); },
            [](PlayerWeaponApi& s, uint32_t record, const Value& v) {
                if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > kMaxCapacity) throw ApiError("capacity must be a whole number from 0 to 999");
                s.override_word(record + kRecordCapacity, uint32_t(v.get<long long>()));
            });
        add_weapon("fire_mode",
            [](PlayerWeaponApi& s, uint32_t record) {
                const uint32_t mode = s.read(record + kRecordFireMode);
                return Value(mode == kModeSemi ? "semi" : mode == kModeAuto ? "auto" : mode == kModeBurst ? "burst" : "mode " + std::to_string(mode));
            },
            [](PlayerWeaponApi& s, uint32_t record, const Value& v) {
                const std::string mode = v.is_string() ? v.get<std::string>() : std::string();
                if (mode != "semi" && mode != "auto" && mode != "burst") throw ApiError("fire_mode must be 'semi', 'auto' or 'burst'");
                s.override_word(record + kRecordFireMode, mode == "semi" ? kModeSemi : mode == "auto" ? kModeAuto : kModeBurst);
            });

        Member camera;
        camera.path = "camera.view";
        camera.get = [self](const Value&) {
            Value view = Value::array();
            for (uint32_t i = 0; i < 12; ++i) view.push_back(double(self->read_float(kCameraView + 4 * i)));
            return view;
        };
        api.add(std::move(camera));
        // The game is on rails and both players share one viewpoint: the "player position" is the camera's. The view matrix has the camera axes as its
        // rows (right, up, back) and -R^T t as the eye position (measured: eye height 1.5 m, zombies 3 to 6 m ahead along -back).
        const auto add_camera_vector = [&](const char* name, int what) {
            Member member;
            member.path = std::string("camera.") + name;
            member.get = [self, what](const Value&) { return self->camera_vector(what); };
            api.add(std::move(member));
        };
        add_camera_vector("position", 0); add_camera_vector("forward", 1); add_camera_vector("right", 2); add_camera_vector("up", 3);

        const auto add_enemy = [&](const char* name, std::function<Value(PlayerWeaponApi&, uint32_t)> get, std::function<void(PlayerWeaponApi&, uint32_t, const Value&)> set) {
            Member member;
            member.path = std::string("enemy.") + name;
            member.scope = "id";
            member.get = [self, get](const Value& args) { return get(*self, self->find_enemy(args)); };
            if (set) member.set = [self, set](const Value& args, const Value& value) { set(*self, self->find_enemy(args), value); };
            api.add(std::move(member));
        };
        add_enemy("hp", [](PlayerWeaponApi& s, uint32_t a) { return Value(double(s.read_float(a + kEnemyHealth))); },
            [](PlayerWeaponApi& s, uint32_t a, const Value& v) {
                if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < -1000 || v.get<double>() > 100000) throw ApiError("hp must be a number from -1000 to 100000 (zombies start with 10 to 30; 0 or less is dead)");
                s.write_float_direct(a + kEnemyHealth, float(v.get<double>()));
            });
        add_enemy("alive", [](PlayerWeaponApi& s, uint32_t a) { return Value(s.read_float(a + kEnemyHealth) > 0.f); }, nullptr);
        add_enemy("head", [](PlayerWeaponApi& s, uint32_t a) { return Value(s.read_float(a + kEnemyHead) > 0.5f); }, nullptr);
        add_enemy("position", [](PlayerWeaponApi& s, uint32_t a) {
                return Value::array({double(s.read_float(a + kEnemyPosition)), double(s.read_float(a + kEnemyPosition + 4)), double(s.read_float(a + kEnemyPosition + 8))});
            },
            [](PlayerWeaponApi& s, uint32_t a, const Value& v) {
                if (!v.is_array() || v.size() != 3) throw ApiError("position must be a list of three numbers {x, y, z} (units of 10 cm, y up)");
                float p[3];
                for (int i = 0; i < 3; ++i) {
                    if (!v[size_t(i)].is_number() || !std::isfinite(v[size_t(i)].get<double>()) || std::abs(v[size_t(i)].get<double>()) > 1e6) throw ApiError("position must be a list of three numbers {x, y, z} (units of 10 cm, y up)");
                    p[i] = float(v[size_t(i)].get<double>());
                }
                for (int i = 0; i < 3; ++i) s.write_float_direct(a + kEnemyPosition + 4 * uint32_t(i), p[i]);
            });

        Member enemies;
        enemies.path = "enemies.list";
        enemies.call = [self](const Value&) { return self->list_enemies(); };
        api.add(std::move(enemies));
    }

    void tick(const GameEventSink& emit) override {
        std::vector<std::pair<const char*, Value>> events;   // delivered after the lock is released: handlers call back into this API
        {
            std::lock_guard lock(mutex_);
            tick_locked(events);
        }
        for (const auto& [event, payload] : events) emit(event, payload);
    }

    void drop_owner(const std::string& owner) override {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < 2; ++i) {
            invulnerable_by_[i].erase(owner);
            infinite_ammo_by_[i].erase(owner);
            state_[i].invulnerable = !invulnerable_by_[i].empty();
            state_[i].infinite_ammo = !infinite_ammo_by_[i].empty();
        }
        std::vector<uint32_t> restored_rates;   // rate fields that went back to the game's value: the live intervals follow
        for (auto it = weapon_overrides_.begin(); it != weapon_overrides_.end();) {
            auto& stack = it->second.stack;
            const auto mine = std::remove_if(stack.begin(), stack.end(), [&](const auto& entry) { return entry.first == owner; });
            if (mine == stack.end()) { ++it; continue; }
            stack.erase(mine, stack.end());
            if (stack.empty()) {
                memory_.write_u32(it->first, it->second.original);
                if ((it->first - kWeaponTable) % kWeaponStride == kRecordFireRate) restored_rates.push_back(it->first);
                it = weapon_overrides_.erase(it);
            }   // nobody else set it: the game's own value
            else { memory_.write_u32(it->first, stack.back().second); ++it; }                                           // the mod that set it before is in force again
        }
        for (int slot = 1; slot <= 2; ++slot) {
            const auto object = player_object(slot);
            if (!object) continue;
            const uint32_t id = memory_.read_u32(*object + kWeaponId).value_or(kWeaponCount);
            if (id >= kWeaponCount || std::find(restored_rates.begin(), restored_rates.end(), kWeaponTable + id * kWeaponStride + kRecordFireRate) == restored_rates.end()) continue;
            const float rate = memory_.read_f32(kWeaponTable + id * kWeaponStride + kRecordFireRate).value_or(0.f);
            if (rate > 0) memory_.write_f32(*object + kWeaponInterval, 1.f / rate);
        }
        enemies_known_ = false;
        for (auto& slot : state_) slot.known = false;   // the ticker may have been idle: the next tick starts from a fresh baseline, not from stale values
    }

private:
    void tick_locked(std::vector<std::pair<const char*, Value>>& events) {
        const auto emit = [&](const char* event, Value payload) { events.emplace_back(event, std::move(payload)); };
        reapply_weapon_overrides();
        refresh_intervals_locked();
        track_enemies(emit);
        const auto scene_address = scene();
        for (int slot = 1; slot <= 2; ++slot) {
            SlotState& s = state_[size_t(slot - 1)];
            const auto object = player_object(slot);
            if (!object) { s.known = false; continue; }
            const uint32_t weapon = memory_.read_u32(*object + kWeaponId).value_or(0), state = memory_.read_u32(*object + kWeaponState).value_or(0);
            uint32_t magazine = memory_.read_u32(*object + kMagazine).value_or(0);
            const uint32_t damage_at = scene_address ? *scene_address + kDamage + uint32_t(slot - 1) * kDamageStride : 0;
            float damage = damage_at ? memory_.read_f32(damage_at).value_or(0.f) : 0.f;
            if (s.known) {
                if (weapon == s.weapon && state != kReloading && magazine < s.magazine) emit("on_weapon_fired", Value{{"slot", slot}});
                if (state == kReloading && s.state != kReloading) emit("on_reload", Value{{"slot", slot}});
                if (damage_at && s.has_damage && std::isfinite(damage) && damage > s.damage) emit("on_damage_taken", Value{{"slot", slot}, {"amount", double(damage - s.damage)}});
                if (damage_at && std::isfinite(damage) && damage >= kDamageDead && !s.dead) emit("on_player_died", Value{{"slot", slot}});
            }
            if (s.invulnerable && damage_at && memory_.write_f32(damage_at, 0.f)) damage = 0.f;
            if (s.infinite_ammo) {
                // The largest magazine seen since this weapon was equipped is the refill target (the game's capacity table is not exposed yet).
                if (!s.known || weapon != s.weapon) s.max_magazine = magazine; else s.max_magazine = std::max(s.max_magazine, magazine);
                if (magazine < s.max_magazine && memory_.write_u32(*object + kMagazine, s.max_magazine)) magazine = s.max_magazine;
            }
            s.dead = damage_at != 0 && std::isfinite(damage) && damage >= kDamageDead;
            s.known = true; s.weapon = weapon; s.state = state; s.magazine = magazine; s.damage = damage; s.has_damage = damage_at != 0;
        }
    }

    // Spawned / killed / headshot events from the zombies in the scene, compared with the previous tick. The first tick of a scene is only the baseline.
    struct EnemySeen { bool alive; bool head; };
    void track_enemies(const std::function<void(const char*, Value)>& emit) {
        if (!player_object(1)) { enemies_known_ = false; enemies_seen_.clear(); return; }   // no scene: the next one starts from a fresh baseline
        std::map<uint32_t, EnemySeen> now;
        for_each_actor([&](uint32_t id, uint32_t address) {
            const auto zombie = read_zombie(address);
            if (!zombie) return true;
            const EnemySeen state{zombie->hp > 0.f, memory_.read_f32(address + kEnemyHead).value_or(0.f) > 0.5f};
            now[id] = state;
            if (!enemies_known_) return true;
            const auto before = enemies_seen_.find(id);
            const Value where{{"id", id}, {"x", double(zombie->x)}, {"y", double(zombie->y)}, {"z", double(zombie->z)}};
            if (before == enemies_seen_.end()) { Value payload = where; payload["hp"] = double(zombie->hp); emit("on_enemy_spawned", std::move(payload)); return true; }
            if (before->second.head && !state.head) emit("on_enemy_headshot", where);
            if (before->second.alive && !state.alive) emit("on_enemy_killed", where);
            return true;
        });
        enemies_seen_ = std::move(now);
        enemies_known_ = true;
    }

    // A switch is on while any mod has it on; each mod's own setting is kept so that switching one mod off leaves the others' alone.
    void set_switch(std::set<std::string>& owners, bool& effective, bool on) {
        std::lock_guard lock(mutex_);
        if (on) owners.insert(OwnerScope::current()); else owners.erase(OwnerScope::current());
        effective = !owners.empty();
    }

    static int slot_of(const Value& args) {
        if (!args.is_object() || !args.contains("slot") || !args["slot"].is_number_integer()) throw ApiError("the player slot must be 1 or 2: uc.player(1)");
        const long long slot = args["slot"].get<long long>();
        if (slot < 1 || slot > 2) throw ApiError("the player slot must be 1 or 2");
        return int(slot);
    }
    static uint32_t weapon_record(const Value& args) {
        if (!args.is_object() || !args.contains("id") || !args["id"].is_number_integer()) throw ApiError("the weapon id must be a whole number from 0 to 19: uc.weapon(0)");
        const long long id = args["id"].get<long long>();
        if (id < 0 || id >= kWeaponCount) throw ApiError("the weapon id must be from 0 to 19");
        return kWeaponTable + uint32_t(id) * kWeaponStride;
    }
    // A weapon record field a mod set: written at once and written again whenever the game's own value differs (the game fills the table at boot
    // and at mission starts, after a mod's on_load has run).
    void override_word(uint32_t address, uint32_t word) {
        std::lock_guard lock(mutex_);
        const auto existing = weapon_overrides_.find(address);
        const auto original = existing != weapon_overrides_.end() ? std::optional<uint32_t>(existing->second.original) : memory_.read_u32(address);
        if (!original || !memory_.write_u32(address, word)) throw ApiError("game memory is not writable right now");
        auto& entry = weapon_overrides_[address];
        entry.original = *original;   // what the game had before any mod touched the field
        const std::string& owner = OwnerScope::current();
        entry.stack.erase(std::remove_if(entry.stack.begin(), entry.stack.end(), [&](const auto& e) { return e.first == owner; }), entry.stack.end());
        entry.stack.emplace_back(owner, word);   // the latest setter is in force; when it goes, the one before it is
    }
    void write_float_direct(uint32_t address, float value) {   // a plain write (not kept: the game owns these fields)
        if (!memory_.write_f32(address, value)) throw ApiError("game memory is not writable right now");
    }
    void write_float(uint32_t address, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof word);
        override_word(address, word);
    }
    // The live interval of every player whose weapon has a rate set by a mod (the game fills it in at equip time from the record).
    void refresh_intervals() {
        std::lock_guard lock(mutex_);
        refresh_intervals_locked();
    }
    void refresh_intervals_locked() {
        for (int slot = 1; slot <= 2; ++slot) {
            const auto object = player_object(slot);
            if (!object) continue;
            const uint32_t id = memory_.read_u32(*object + kWeaponId).value_or(kWeaponCount);
            if (id >= kWeaponCount) continue;
            const uint32_t rate_address = kWeaponTable + id * kWeaponStride + kRecordFireRate;
            const auto override_entry = weapon_overrides_.find(rate_address);
            if (override_entry == weapon_overrides_.end()) continue;   // a weapon no mod changed keeps the game's own interval
            const float rate = memory_.read_f32(rate_address).value_or(0.f);
            if (rate > 0 && memory_.read_f32(*object + kWeaponInterval) != 1.f / rate) memory_.write_f32(*object + kWeaponInterval, 1.f / rate);
        }
    }
    void reapply_weapon_overrides() {
        for (const auto& [address, entry] : weapon_overrides_) if (memory_.read_u32(address) != entry.stack.back().second) memory_.write_u32(address, entry.stack.back().second);
    }
    static uint32_t count_of(const Value& v, const char* what) {
        if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > kMaxMagazine) throw ApiError(std::string(what) + " must be a whole number from 0 to 65535");
        return uint32_t(v.get<long long>());
    }
    static bool flag_of(const Value& v, const char* what) {
        if (!v.is_boolean()) throw ApiError(std::string(what) + " must be true or false");
        return v.get<bool>();
    }

    std::optional<uint32_t> player_object(int slot) const {
        const auto pointer = memory_.read_u32(kPlayerTable + 4u * uint32_t(slot - 1));
        if (!pointer || !*pointer || !memory_.valid(*pointer, kPlayerSize)) return std::nullopt;
        return *pointer;
    }
    uint32_t require_player(int slot) const {
        const auto object = player_object(slot);
        if (!object) throw ApiError("player " + std::to_string(slot) + " is not in the game right now (check uc.player(" + std::to_string(slot) + ").present first)");
        return *object;
    }
    std::optional<uint32_t> scene() const {
        const auto pointer = memory_.read_u32(kScenePointer);
        if (!pointer || !*pointer || !memory_.valid(*pointer, kSceneSize)) return std::nullopt;
        return *pointer;
    }
    uint32_t damage_address(int slot) const {
        const auto s = scene();
        if (!s) throw ApiError("there is no scene right now (the game is in a menu or loading)");
        return *s + kDamage + uint32_t(slot - 1) * kDamageStride;
    }
    uint32_t checked_key(uint32_t key) const {
        if (key >= kReserveKeys) throw ApiError("the held weapon has an ammo key outside the game's table (" + std::to_string(key) + ")");
        return key;
    }
    uint16_t read_u16(uint32_t address) const {
        const auto v = memory_.read_u16(address);
        if (!v) throw ApiError("game memory is not readable right now");
        return *v;
    }
    uint32_t read(uint32_t address) const {
        const auto v = memory_.read_u32(address);
        if (!v) throw ApiError("game memory is not readable right now");
        return *v;
    }
    float read_float(uint32_t address) const {
        const auto v = memory_.read_f32(address);
        if (!v) throw ApiError("game memory is not readable right now");
        return *v;
    }
    void write(uint32_t address, uint32_t value) {
        if (!memory_.write_u32(address, value)) throw ApiError("game memory is not writable right now");
    }

    Value camera_vector(int what) const {
        float m[12];
        for (uint32_t i = 0; i < 12; ++i) m[i] = read_float(kCameraView + 4 * i);
        const auto row = [&](int r) { return std::array<double, 3>{m[4 * r], m[4 * r + 1], m[4 * r + 2]}; };
        for (int r = 0; r < 3; ++r) {
            const auto v = row(r);
            const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!std::isfinite(length) || length < 0.9 || length > 1.1) throw ApiError("there is no camera right now (the game is in a menu or loading)");   // an all-zero or broken matrix
        }
        std::array<double, 3> result{};
        if (what == 0) {
            for (int i = 0; i < 3; ++i) result[size_t(i)] = -(double(m[i]) * m[3] + double(m[4 + i]) * m[7] + double(m[8 + i]) * m[11]);
        } else {
            const auto axis = what == 1 ? row(2) : what == 2 ? row(0) : row(1);
            const double sign = what == 1 ? -1.0 : 1.0;   // forward is minus the third row
            for (int i = 0; i < 3; ++i) result[size_t(i)] = sign * axis[size_t(i)];
        }
        return Value::array({result[0], result[1], result[2]});
    }

    // Every actor the game keeps: the 200-entry table, then the actors with ids of 200 and up that are only in the linked list from player 1 (object+8 holds
    // the next object + 8). visit(id, address) returns false to stop.
    template <class F> void for_each_actor(F&& visit) const {
        std::vector<uint32_t> seen;
        const auto consider = [&](uint32_t id, uint32_t address) {
            if (!address || !memory_.valid(address, 0x1060) || std::find(seen.begin(), seen.end(), address) != seen.end()) return true;
            seen.push_back(address);
            return visit(id, address);
        };
        for (uint32_t i = 0; i < 200; ++i) {
            const auto id = memory_.read_u32(kActorTable + i * 8), pointer = memory_.read_u32(kActorTable + i * 8 + 4);
            if (id && pointer && !consider(*id, *pointer)) return;
        }
        if (const auto first = player_object(1)) {
            std::vector<uint32_t> visited{*first};
            for (uint32_t object = *first; visited.size() < 512;) {
                const auto link = memory_.read_u32(object + 8);
                if (!link || *link < 8) break;
                object = *link - 8;
                if (std::find(visited.begin(), visited.end(), object) != visited.end() || !memory_.valid(object, 12)) break;
                visited.push_back(object);
                if (const auto id = memory_.read_u32(object)) if (!consider(*id, object)) return;
            }
        }
    }

    // A zombie with a sane position and health, or nullopt.
    struct Zombie { float x, y, z, hp; };
    std::optional<Zombie> read_zombie(uint32_t address) const {
        if (memory_.read_u32(address + kActorClass) != kZombieClass) return std::nullopt;
        const auto x = memory_.read_f32(address + kEnemyPosition), y = memory_.read_f32(address + kEnemyPosition + 4), z = memory_.read_f32(address + kEnemyPosition + 8);
        const auto hp = memory_.read_f32(address + kEnemyHealth);
        if (!x || !y || !z || !hp || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z) || !std::isfinite(*hp)) return std::nullopt;
        if (std::abs(*x) > 1e6f || std::abs(*y) > 1e6f || std::abs(*z) > 1e6f) return std::nullopt;
        return Zombie{*x, *y, *z, *hp};
    }

    Value list_enemies() const {
        Value list = Value::array();
        for_each_actor([&](uint32_t id, uint32_t address) {
            if (list.size() >= 512) return false;
            if (const auto zombie = read_zombie(address))
                list.push_back({{"id", id}, {"class", kZombieClass}, {"x", double(zombie->x)}, {"y", double(zombie->y)}, {"z", double(zombie->z)}, {"hp", double(zombie->hp)}});
            return true;
        });
        return list;
    }

    // The address of the zombie with this id (the id enemies.list() gives).
    uint32_t find_enemy(const Value& args) const {
        if (!args.is_object() || !args.contains("id") || !args["id"].is_number_integer() || args["id"].get<long long>() < 0 || args["id"].get<long long>() > 0xFFFFFFFFll)
            throw ApiError("the enemy id must be a whole number: uc.enemy(10)");
        const uint32_t wanted = uint32_t(args["id"].get<long long>());
        uint32_t found = 0;
        for_each_actor([&](uint32_t id, uint32_t address) {
            if (id != wanted || !read_zombie(address)) return true;
            found = address;
            return false;
        });
        if (!found) throw ApiError("there is no enemy with id " + std::to_string(wanted) + " right now (ids come from uc.enemies.list(); only zombies are supported)");
        return found;
    }

    GuestMemory& memory_;
    struct WeaponOverride { uint32_t original = 0; std::vector<std::pair<std::string, uint32_t>> stack; };   // stack: (owner, word) in the order set; never empty while the entry exists
    std::map<uint32_t, EnemySeen> enemies_seen_;
    bool enemies_known_ = false;
    std::mutex mutex_;   // guards everything below: mods call in from the game thread and the menu thread
    SlotState state_[2];
    std::set<std::string> invulnerable_by_[2], infinite_ammo_by_[2];
    std::map<uint32_t, WeaponOverride> weapon_overrides_;   // record field address -> the word a mod set
};
}  // namespace

std::shared_ptr<GameApiTicker> register_player_weapon_api(ApiRegistry& api, GuestMemory& memory) {
    auto implementation = std::make_shared<PlayerWeaponApi>(memory);
    implementation->register_members(api);
    return implementation;
}

}  // namespace uc::mods
