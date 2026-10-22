// player.*, camera.*, enemies.*: the game's structures as found for the NTSC-U release (RBUE08). Every address and offset here was confirmed
// with the test harness; see docs/spikes/G22-autoplay-memory.md and docs/spikes/M1-weapon-data.md.
#include "game_api.h"
#include <algorithm>
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
constexpr uint32_t kZombieClass = 0x803F2B60, kEnemyPosition = 0xF48, kEnemyHealth = 0x3A4, kActorClass = 0x10;
constexpr uint32_t kMaxMagazine = 65535;

// The weapon table: 20 records of 168 bytes, found through the record accessor 8006C8C8 (id clamped to 0..19, 8046A86C + id * 168).
constexpr uint32_t kWeaponTable = 0x8046A86C, kWeaponStride = 168, kWeaponCount = 20;
constexpr uint32_t kRecordFireMode = 0x10, kRecordCapacity = 0x14, kRecordDamage = 0x24, kRecordReloadTime = 0x38;
constexpr uint32_t kModeAuto = 0, kModeSemi = 1, kModeBurst = 3;   // values seen in the table: submachine gun 0, handgun/shotgun/others 1, 3 on record 3
constexpr long long kMaxCapacity = 999;

struct SlotState {
    bool invulnerable = false, infinite_ammo = false;
    bool known = false, has_damage = false;
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
        // fire_rate (record +0x0C: 1 for single shots, 10 to 15 for automatic weapons) is not exposed: on the handgun in automatic mode
        // the values 10 and 30 give the same cadence (docs/spikes/M1-weapon-data.md), so what it controls is not understood.
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
        for (auto it = weapon_overrides_.begin(); it != weapon_overrides_.end();) {
            if (it->second.owner != owner) { ++it; continue; }
            memory_.write_u32(it->first, it->second.original);
            it = weapon_overrides_.erase(it);
        }
    }

private:
    void tick_locked(std::vector<std::pair<const char*, Value>>& events) {
        const auto emit = [&](const char* event, Value payload) { events.emplace_back(event, std::move(payload)); };
        reapply_weapon_overrides();
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
            }
            if (s.invulnerable && damage_at && memory_.write_f32(damage_at, 0.f)) damage = 0.f;
            if (s.infinite_ammo) {
                // The largest magazine seen since this weapon was equipped is the refill target (the game's capacity table is not exposed yet).
                if (!s.known || weapon != s.weapon) s.max_magazine = magazine; else s.max_magazine = std::max(s.max_magazine, magazine);
                if (magazine < s.max_magazine && memory_.write_u32(*object + kMagazine, s.max_magazine)) magazine = s.max_magazine;
            }
            s.known = true; s.weapon = weapon; s.state = state; s.magazine = magazine; s.damage = damage; s.has_damage = damage_at != 0;
        }
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
        weapon_overrides_[address] = {word, *original, OwnerScope::current()};   // the last mod to set a field owns it; the game's own value is what gets restored
    }
    void write_float(uint32_t address, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof word);
        override_word(address, word);
    }
    void reapply_weapon_overrides() {
        for (const auto& [address, entry] : weapon_overrides_) if (memory_.read_u32(address) != entry.word) memory_.write_u32(address, entry.word);
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

    // Zombies from the actor table, plus actors with ids of 200 and up, which are only in the actor list (object+8 holds the next object + 8).
    Value list_enemies() const {
        Value list = Value::array();
        std::vector<uint32_t> seen;
        const auto consider = [&](uint32_t id, uint32_t address) {
            if (!address || list.size() >= 512 || !memory_.valid(address, 0x1060) || std::find(seen.begin(), seen.end(), address) != seen.end()) return;
            seen.push_back(address);
            if (memory_.read_u32(address + kActorClass) != kZombieClass) return;
            const auto x = memory_.read_f32(address + kEnemyPosition), y = memory_.read_f32(address + kEnemyPosition + 4), z = memory_.read_f32(address + kEnemyPosition + 8);
            const auto hp = memory_.read_f32(address + kEnemyHealth);
            if (!x || !y || !z || !hp || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z) || !std::isfinite(*hp)) return;
            if (std::abs(*x) > 1e6f || std::abs(*y) > 1e6f || std::abs(*z) > 1e6f) return;
            list.push_back({{"id", id}, {"class", kZombieClass}, {"x", double(*x)}, {"y", double(*y)}, {"z", double(*z)}, {"hp", double(*hp)}});
        };
        for (uint32_t i = 0; i < 200; ++i) {
            const auto id = memory_.read_u32(kActorTable + i * 8), pointer = memory_.read_u32(kActorTable + i * 8 + 4);
            if (id && pointer) consider(*id, *pointer);
        }
        if (const auto first = player_object(1)) {
            std::vector<uint32_t> visited{*first};
            for (uint32_t object = *first; visited.size() < 512;) {
                const auto link = memory_.read_u32(object + 8);
                if (!link || *link < 8) break;
                object = *link - 8;
                if (std::find(visited.begin(), visited.end(), object) != visited.end() || !memory_.valid(object, 12)) break;
                visited.push_back(object);
                if (const auto id = memory_.read_u32(object)) consider(*id, object);
            }
        }
        return list;
    }

    GuestMemory& memory_;
    struct WeaponOverride { uint32_t word, original; std::string owner; };   // original: what the game had before any mod touched the field
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
