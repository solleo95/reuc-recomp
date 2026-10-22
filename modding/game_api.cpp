#include "game_api.h"
#include "input_api.h"
#include "trace.h"
#include <cmath>

namespace uc::mods {
namespace {
constexpr uint32_t kMaxBytes = 4096;

const Value& need(const Value& args, const char* key) {
    if (!args.is_object() || !args.contains(key)) throw ApiError(std::string("missing argument '") + key + "'");
    return args[key];
}

long long integer_arg(const Value& args, const char* key, long long low, long long high) {
    const Value& v = need(args, key);
    if (!v.is_number() || (!v.is_number_integer() && v.get<double>() != std::floor(v.get<double>()))) throw ApiError(std::string("'") + key + "' must be an integer");
    const long long value = v.is_number_integer() ? v.get<long long>() : static_cast<long long>(v.get<double>());
    if (value < low || value > high) throw ApiError(std::string("'") + key + "' must be between " + std::to_string(low) + " and " + std::to_string(high));
    return value;
}

uint32_t address_arg(const Value& args) { return uint32_t(integer_arg(args, "address", 0, 0xFFFFFFFFll)); }

double number_arg(const Value& args, const char* key) {
    const Value& v = need(args, key);
    if (!v.is_number()) throw ApiError(std::string("'") + key + "' must be a number");
    return v.get<double>();
}

[[noreturn]] void bad_address(uint32_t address) {
    char text[160];
    std::snprintf(text, sizeof text, "address 0x%08X is outside guest memory (MEM1 0x80000000-0x817FFFFF, MEM2 0x90000000-0x93FFFFFF) or the access runs past its end", address);
    throw ApiError(text);
}

template <class T> Value take(const std::optional<T>& value, uint32_t address) {
    if (!value) bad_address(address);
    return Value(*value);
}

void add_function(ApiRegistry& api, std::string path, std::vector<std::string> names, std::function<Value(const Value&)> body) {
    Member member;
    member.path = std::move(path);
    member.arg_names = std::move(names);
    member.call = std::move(body);
    member.writes = member.path.rfind("mem.write_", 0) == 0;
    api.add(std::move(member));
}
}  // namespace

void register_low_level_api(ApiRegistry& api, GuestMemory& mem, const SymbolTable& symbols) {
    GuestMemory* m = &mem;
#define UC_READ(NAME) add_function(api, "mem.read_" #NAME, {"address"}, [m](const Value& a) { const auto address = address_arg(a); return take(m->read_##NAME(address), address); })
    UC_READ(u8); UC_READ(u16); UC_READ(u32); UC_READ(s8); UC_READ(s16); UC_READ(s32); UC_READ(f32); UC_READ(f64);
#undef UC_READ
#define UC_WRITE_INT(NAME, TYPE, LOW, HIGH) add_function(api, "mem.write_" #NAME, {"address", "value"}, [m](const Value& a) { \
        const auto address = address_arg(a); const auto value = integer_arg(a, "value", LOW, HIGH); \
        if (!m->write_##NAME(address, TYPE(value))) bad_address(address); return Value(); })
    UC_WRITE_INT(u8, uint8_t, 0, 255); UC_WRITE_INT(u16, uint16_t, 0, 65535); UC_WRITE_INT(u32, uint32_t, 0, 0xFFFFFFFFll);
    UC_WRITE_INT(s8, int8_t, -128, 127); UC_WRITE_INT(s16, int16_t, -32768, 32767); UC_WRITE_INT(s32, int32_t, -2147483648ll, 2147483647ll);
#undef UC_WRITE_INT
    add_function(api, "mem.write_f32", {"address", "value"}, [m](const Value& a) {
        const auto address = address_arg(a);
        if (!m->write_f32(address, float(number_arg(a, "value")))) bad_address(address);
        return Value(); });
    add_function(api, "mem.write_f64", {"address", "value"}, [m](const Value& a) {
        const auto address = address_arg(a);
        if (!m->write_f64(address, number_arg(a, "value"))) bad_address(address);
        return Value(); });
    add_function(api, "mem.read_bytes", {"address", "length"}, [m](const Value& a) {
        const auto address = address_arg(a);
        const auto length = uint32_t(integer_arg(a, "length", 1, kMaxBytes));
        std::vector<uint8_t> bytes(length);
        if (!m->read_bytes(address, bytes.data(), length)) bad_address(address);
        return Value(bytes); });
    add_function(api, "mem.write_bytes", {"address", "bytes"}, [m](const Value& a) {
        const auto address = address_arg(a);
        const Value& list = need(a, "bytes");
        if (!list.is_array() || list.empty() || list.size() > kMaxBytes) throw ApiError("'bytes' must be a list of 1 to 4096 numbers");
        std::vector<uint8_t> bytes;
        for (const auto& item : list) {
            if (!item.is_number_integer() || item.get<long long>() < 0 || item.get<long long>() > 255) throw ApiError("'bytes' entries must be integers from 0 to 255");
            bytes.push_back(uint8_t(item.get<long long>()));
        }
        if (!m->write_bytes(address, bytes.data(), uint32_t(bytes.size()))) bad_address(address);
        return Value(); });

    add_function(api, "symbols.lookup", {"name"}, [symbols](const Value& a) {
        const Value& name = need(a, "name");
        if (!name.is_string()) throw ApiError("'name' must be a string");
        const auto address = symbols.by_name(name.get<std::string>());
        return address ? Value(*address) : Value(nullptr); });
    add_function(api, "symbols.name_of", {"address"}, [symbols](const Value& a) {
        const auto name = symbols.name_of(address_arg(a));
        return name.empty() ? Value(nullptr) : Value(name); });
}

SymbolTable runtime_symbols() {
    SymbolTable table;
    table.by_name = [](std::string_view name) { return uc::symbol_address(std::string(name)); };
    table.name_of = [](uint32_t address) {
        const auto name = uc::symbol_name(address);
        return name.find('+') == std::string::npos ? name : std::string();   // "name+0x1C" means the address is inside, not at, the symbol
    };
    return table;
}

namespace {
// One ticker for the parts of the game API that keep per-step state or per-mod state.
class CompositeTicker : public GameApiTicker {
public:
    explicit CompositeTicker(std::vector<std::shared_ptr<GameApiTicker>> parts) : parts_(std::move(parts)) {}
    void tick(const GameEventSink& emit) override { for (const auto& part : parts_) part->tick(emit); }
    void drop_owner(const std::string& owner) override { for (const auto& part : parts_) part->drop_owner(owner); }
private:
    std::vector<std::shared_ptr<GameApiTicker>> parts_;
};
}  // namespace

std::shared_ptr<GameApiTicker> register_game_api(ApiRegistry& api, GuestMemory& mem, const SymbolTable& symbols) {
    register_low_level_api(api, mem, symbols);
    auto player = register_player_weapon_api(api, mem);
    register_guest_calls_api(api, mem);
    auto input = register_input_api(api);
    return std::make_shared<CompositeTicker>(std::vector<std::shared_ptr<GameApiTicker>>{player, input});
}

}  // namespace uc::mods
