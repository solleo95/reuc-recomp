#pragma once
#include "api_registry.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>

namespace uc::mods {

// Guest (Wii) memory seen from mods: MEM1 0x80000000..0x817FFFFF and MEM2 0x90000000..0x93FFFFFF only, values big-endian.
// Every access is range-checked as a whole; a range that is not entirely inside one region is refused (nullopt / false).
class GuestMemory {
public:
    explicit GuestMemory(uint8_t* base) : base_(base) {}   // base: runtime/guest_mem.h guest_mem_base()

    bool valid(uint32_t address, uint32_t length) const {
        const uint64_t end = uint64_t(address) + length;
        return length > 0 && ((address >= 0x80000000u && end <= 0x81800000ull) || (address >= 0x90000000u && end <= 0x94000000ull));
    }
    bool read_bytes(uint32_t address, void* destination, uint32_t length) const {
        if (!valid(address, length)) return false;
        std::memcpy(destination, host(address), length);
        return true;
    }
    bool write_bytes(uint32_t address, const void* source, uint32_t length) {
        if (!valid(address, length)) return false;
        std::memcpy(host(address), source, length);
        return true;
    }

    std::optional<uint8_t> read_u8(uint32_t a) const { return read<uint8_t>(a); }
    std::optional<uint16_t> read_u16(uint32_t a) const { return read<uint16_t>(a); }
    std::optional<uint32_t> read_u32(uint32_t a) const { return read<uint32_t>(a); }
    std::optional<int8_t> read_s8(uint32_t a) const { return read<int8_t>(a); }
    std::optional<int16_t> read_s16(uint32_t a) const { return read<int16_t>(a); }
    std::optional<int32_t> read_s32(uint32_t a) const { return read<int32_t>(a); }
    std::optional<float> read_f32(uint32_t a) const { return read<float>(a); }
    std::optional<double> read_f64(uint32_t a) const { return read<double>(a); }
    bool write_u8(uint32_t a, uint8_t v) { return write(a, v); }
    bool write_u16(uint32_t a, uint16_t v) { return write(a, v); }
    bool write_u32(uint32_t a, uint32_t v) { return write(a, v); }
    bool write_s8(uint32_t a, int8_t v) { return write(a, v); }
    bool write_s16(uint32_t a, int16_t v) { return write(a, v); }
    bool write_s32(uint32_t a, int32_t v) { return write(a, v); }
    bool write_f32(uint32_t a, float v) { return write(a, v); }
    bool write_f64(uint32_t a, double v) { return write(a, v); }

private:
    uint8_t* host(uint32_t address) const { return base_ + (address & 0x3FFFFFFFu); }
    template <class T> std::optional<T> read(uint32_t address) const {
        if (!valid(address, sizeof(T))) return std::nullopt;
        uint8_t swapped[sizeof(T)];
        const uint8_t* source = host(address);
        for (size_t i = 0; i < sizeof(T); ++i) swapped[i] = source[sizeof(T) - 1 - i];
        T value;
        std::memcpy(&value, swapped, sizeof(T));
        return value;
    }
    template <class T> bool write(uint32_t address, T value) {
        if (!valid(address, sizeof(T))) return false;
        uint8_t raw[sizeof(T)];
        std::memcpy(raw, &value, sizeof(T));
        uint8_t* target = host(address);
        for (size_t i = 0; i < sizeof(T); ++i) target[i] = raw[sizeof(T) - 1 - i];
        return true;
    }
    uint8_t* base_;
};

// Names the project knows for guest addresses.
struct SymbolTable {
    std::function<std::optional<uint32_t>(std::string_view name)> by_name;
    std::function<std::string(uint32_t address)> name_of;   // the symbol at exactly this address, or empty
};
SymbolTable runtime_symbols();   // project/symbols.txt as loaded by runtime/trace.h symbols_load

// mem.* and symbols.*
void register_low_level_api(ApiRegistry&, GuestMemory&, const SymbolTable&);

// The per-step half of the game API: applies the switches mods set (player.invulnerable, player.infinite_ammo) and turns changes of the game
// state into events (on_weapon_fired, on_reload, on_damage_taken). The host runs tick() once per game step before the mods' on_step handlers.
using GameEventSink = std::function<void(std::string_view event, const Value& payload)>;
class GameApiTicker {
public:
    virtual ~GameApiTicker() = default;
    virtual void tick(const GameEventSink& emit) = 0;
    // Takes back everything the mod set (switches off, weapon values restored): called when the mod is switched off, its scripts are reloaded or it is shut down.
    virtual void drop_owner(const std::string& owner) = 0;
};

// Everything under a registry domain of the schema that is implemented. The returned ticker shares state with the registered members.
std::shared_ptr<GameApiTicker> register_game_api(ApiRegistry&, GuestMemory&, const SymbolTable&);
// The player, camera and enemies members (game_api_player.cpp).
std::shared_ptr<GameApiTicker> register_player_weapon_api(ApiRegistry&, GuestMemory&);

}  // namespace uc::mods
