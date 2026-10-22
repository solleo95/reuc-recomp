#include "game_api.h"
#include "mod_guest_call.h"
#include <cmath>

namespace uc::mods {
namespace {
constexpr uint32_t kPlayerTable = 0x806C3AD8;
constexpr uint32_t kPlayerSize = 0x7A0;
// The loadout of a player object (found in fn_80067980 and confirmed on a live train chapter): 16 slots, each with a magazine (+0x6C4), a weapon id
// (+0x704, 0xFFFFFFFF for an empty slot) and an ammo key (+0x744), and the number of the slot in use at +0x784.
constexpr uint32_t kLoadoutSlots = 16, kLoadoutMagazine = 0x6C4, kLoadoutWeapon = 0x704, kLoadoutKey = 0x744, kLoadoutCurrent = 0x784;
constexpr uint32_t kSelectSlot = 0x80067980;   // fn_80067980(player, slot): makes the slot current and equips its weapon
constexpr uint32_t kWeaponTable = 0x8046A86C, kWeaponStride = 168, kWeaponCount = 20, kRecordCapacity = 0x14;
constexpr uint32_t kEmpty = 0xFFFFFFFFu;

long long whole(const Value& v, const char* what, long long low, long long high) {
    if (!v.is_number() || (!v.is_number_integer() && v.get<double>() != std::floor(v.get<double>()))) throw ApiError(std::string(what) + " must be a whole number");
    const long long n = v.is_number_integer() ? v.get<long long>() : static_cast<long long>(v.get<double>());
    if (n < low || n > high) throw ApiError(std::string(what) + " must be from " + std::to_string(low) + " to " + std::to_string(high));
    return n;
}
const Value* optional_arg(const Value& args, const char* key) { return args.is_object() && args.contains(key) && !args[key].is_null() ? &args[key] : nullptr; }
const Value& required_arg(const Value& args, const char* key) {
    if (!args.is_object() || !args.contains(key)) throw ApiError(std::string("missing argument '") + key + "'");
    return args[key];
}

class GuestCalls {
public:
    explicit GuestCalls(GuestMemory& memory) : memory_(memory) {}

    Value call(const Value& args) {
        const uint32_t address = uint32_t(whole(required_arg(args, "address"), "address", 0, 0xFFFFFFFFll));
        std::vector<uint32_t> registers;
        if (const Value* list = optional_arg(args, "args")) {
            if (!list->is_array() || list->size() > kMaxGuestCallArgs) throw ApiError("'args' must be a list of at most 8 whole numbers");
            for (const auto& item : *list) registers.push_back(uint32_t(whole(item, "an argument", -2147483648ll, 0xFFFFFFFFll)));
        }
        return Value(guest_call(address, registers));
    }

    // Puts a weapon into the first free slot of the player's loadout (or finds it again if the player already has it). The game's own switching
    // (and select_weapon) then reaches it. The ammo key picks the spare ammo (0 is an infinite reserve, like the handgun).
    Value give_weapon(const Value& args) {
        const uint32_t player = player_object(whole(required_arg(args, "slot"), "slot", 1, 2));
        const long long id = whole(required_arg(args, "id"), "the weapon id", 0, kWeaponCount - 1);
        const long long key = optional_arg(args, "key") ? whole(*optional_arg(args, "key"), "the ammo key", 0, 20) : 0;
        uint32_t free_slot = kEmpty;
        for (uint32_t i = 0; i < kLoadoutSlots; ++i) {
            const uint32_t weapon = read(player + kLoadoutWeapon + 4 * i);
            if (weapon == uint32_t(id)) return Value(i);
            if (weapon == kEmpty && free_slot == kEmpty) free_slot = i;
        }
        if (free_slot == kEmpty) throw ApiError("the player's loadout is full (16 weapons)");
        const long long magazine = optional_arg(args, "magazine") ? whole(*optional_arg(args, "magazine"), "the magazine", 0, 999) : read(kWeaponTable + uint32_t(id) * kWeaponStride + kRecordCapacity);
        write(player + kLoadoutWeapon + 4 * free_slot, uint32_t(id));
        write(player + kLoadoutKey + 4 * free_slot, uint32_t(key));
        write(player + kLoadoutMagazine + 4 * free_slot, uint32_t(magazine));
        return Value(free_slot);
    }

    // Makes a loadout slot the one in use: runs the game's own switch function, so the weapon object, the magazine and the HUD follow.
    Value select_weapon(const Value& args) {
        const uint32_t player = player_object(whole(required_arg(args, "slot"), "slot", 1, 2));
        const uint32_t index = uint32_t(whole(required_arg(args, "index"), "index", 0, kLoadoutSlots - 1));
        if (read(player + kLoadoutWeapon + 4 * index) == kEmpty) throw ApiError("that loadout slot is empty (give_weapon returns the slot of a weapon)");
        guest_call(kSelectSlot, {player, index});
        return Value(read(player + kLoadoutCurrent) == index);
    }

private:
    uint32_t player_object(long long slot) const {
        const auto pointer = memory_.read_u32(kPlayerTable + 4u * uint32_t(slot - 1));
        if (!pointer || !*pointer || !memory_.valid(*pointer, kPlayerSize)) throw ApiError("player " + std::to_string(slot) + " is not in the game right now (check uc.player(" + std::to_string(slot) + ").present first)");
        return *pointer;
    }
    uint32_t read(uint32_t address) const {
        const auto v = memory_.read_u32(address);
        if (!v) throw ApiError("game memory is not readable right now");
        return *v;
    }
    void write(uint32_t address, uint32_t value) {
        if (!memory_.write_u32(address, value)) throw ApiError("game memory is not writable right now");
    }
    GuestMemory& memory_;
};
}  // namespace

void register_guest_calls_api(ApiRegistry& api, GuestMemory& memory) {
    const auto self = std::make_shared<GuestCalls>(memory);
    const auto add = [&](const char* path, std::vector<std::string> names, Value (GuestCalls::*body)(const Value&)) {
        Member member;
        member.path = path;
        member.arg_names = std::move(names);
        member.call = [self, body](const Value& args) { return ((*self).*body)(args); };
        member.writes = true;
        api.add(std::move(member));
    };
    add("guest.call", {"address", "args"}, &GuestCalls::call);
    add("game.give_weapon", {"slot", "id", "key", "magazine"}, &GuestCalls::give_weapon);
    add("game.select_weapon", {"slot", "index"}, &GuestCalls::select_weapon);
}

}  // namespace uc::mods
