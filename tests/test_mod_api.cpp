#include "check.h"
#include "api_registry.h"
#include "game_api.h"
#include "guest_mem.h"
#include <fstream>
#include <set>

using namespace uc::mods;
using nlohmann::json;

static SymbolTable fake_symbols() {
    SymbolTable symbols;
    symbols.by_name = [](std::string_view name) -> std::optional<uint32_t> { return name == "OSInit" ? std::optional<uint32_t>(0x80258AE8) : std::nullopt; };
    symbols.name_of = [](uint32_t address) -> std::string { return address == 0x80258AE8 ? "OSInit" : ""; };
    return symbols;
}

static json call(const ApiRegistry& api, const std::string& path, json args = json::object()) {
    const auto* member = api.find(path);
    CHECK(member && member->call);
    return member->call(args);
}

static bool throws_api_error(const ApiRegistry& api, const std::string& path, json args) {
    try { call(api, path, args); } catch (const ApiError&) { return true; }
    return false;
}

static void read_write_big_endian() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    CHECK(mem.write_u32(0x80000010, 0x11223344));
    const uint8_t* raw = uc::guest_mem_base() + 0x10;
    CHECK(raw[0] == 0x11 && raw[1] == 0x22 && raw[2] == 0x33 && raw[3] == 0x44);
    CHECK(mem.read_u32(0x80000010) == 0x11223344u && mem.read_u16(0x80000010) == 0x1122 && mem.read_u8(0x80000013) == 0x44);
    CHECK(mem.write_u16(0x90000020, 0xBEEF) && mem.read_u16(0x90000020) == 0xBEEF);   // MEM2
}

static void bounds() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    CHECK(!mem.read_u32(0x7FFFFFFC) && !mem.read_u32(0) && !mem.read_u32(0xFFFFFFFE));
    CHECK(mem.read_u32(0x817FFFFC) && !mem.read_u32(0x817FFFFD) && !mem.read_u32(0x81800000));   // MEM1 ends at 0x81800000
    CHECK(mem.read_u32(0x93FFFFFC) && !mem.read_u32(0x93FFFFFD) && !mem.read_u32(0x94000000));   // MEM2 ends at 0x94000000
    CHECK(!mem.read_u32(0x8FFFFFFC) && !mem.read_u32(0x81800000));   // the gap between them
    CHECK(mem.read_u32(0x80000001));   // unaligned reads are fine
    CHECK(!mem.write_u32(0x817FFFFD, 1) && !mem.write_u8(0x95000000, 1));
    uint8_t buffer[8];
    CHECK(mem.read_bytes(0x80000000, buffer, 8) && !mem.read_bytes(0x817FFFFC, buffer, 8) && !mem.read_bytes(0xFFFFFFFC, buffer, 8));   // length wrap-around
}

static void float_roundtrip() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    CHECK(mem.write_f32(0x80000100, 1.5f) && mem.read_f32(0x80000100) == 1.5f);
    CHECK(mem.read_u32(0x80000100) == 0x3FC00000u);   // 1.5f, big-endian
    CHECK(mem.write_f64(0x80000108, -2.25) && mem.read_f64(0x80000108) == -2.25);
}

static void api_mem_functions() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    ApiRegistry api;
    register_low_level_api(api, mem, fake_symbols());
    call(api, "mem.write_u32", {{"address", 0x80000200}, {"value", 0xDEADBEEFull}});
    CHECK(call(api, "mem.read_u32", {{"address", 0x80000200}}) == 0xDEADBEEFull);
    CHECK(call(api, "mem.read_s32", {{"address", 0x80000200}}) == -559038737);
    CHECK(call(api, "mem.read_s8", {{"address", 0x80000200}}) == -34 && call(api, "mem.read_u8", {{"address", 0x80000200}}) == 0xDE);
    call(api, "mem.write_s16", {{"address", 0x80000210}, {"value", -2}});
    CHECK(call(api, "mem.read_u16", {{"address", 0x80000210}}) == 0xFFFE && call(api, "mem.read_s16", {{"address", 0x80000210}}) == -2);
    call(api, "mem.write_f32", {{"address", 0x80000220}, {"value", 0.5}});
    CHECK(call(api, "mem.read_f32", {{"address", 0x80000220}}) == 0.5);
    call(api, "mem.write_bytes", {{"address", 0x80000230}, {"bytes", {1, 2, 3, 255}}});
    CHECK(call(api, "mem.read_bytes", {{"address", 0x80000230}, {"length", 4}}) == json({1, 2, 3, 255}));
}

static void api_mem_rejects() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    ApiRegistry api;
    register_low_level_api(api, mem, fake_symbols());
    CHECK(throws_api_error(api, "mem.read_u32", {{"address", 0x10}}));                   // outside guest memory
    CHECK(throws_api_error(api, "mem.read_u32", {{"address", 0x817FFFFD}}));             // straddles the end
    CHECK(throws_api_error(api, "mem.write_u32", {{"address", 0x10}, {"value", 1}}));
    CHECK(throws_api_error(api, "mem.read_u32", {{"address", -1}}));                     // not a guest address
    CHECK(throws_api_error(api, "mem.read_u32", {{"address", "x"}}));                    // wrong type
    CHECK(throws_api_error(api, "mem.read_u32", {{"address", 1.5}}));
    CHECK(throws_api_error(api, "mem.read_u32", json::object()));                        // missing argument
    CHECK(throws_api_error(api, "mem.write_u8", {{"address", 0x80000000}, {"value", 256}}));
    CHECK(throws_api_error(api, "mem.write_s8", {{"address", 0x80000000}, {"value", -129}}));
    CHECK(throws_api_error(api, "mem.write_u32", {{"address", 0x80000000}, {"value", 4294967296ull}}));
    CHECK(throws_api_error(api, "mem.read_bytes", {{"address", 0x80000000}, {"length", 4097}}));
    CHECK(throws_api_error(api, "mem.write_bytes", {{"address", 0x80000000}, {"bytes", {1, 300}}}));
}

static void symbols_lookup() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    ApiRegistry api;
    register_low_level_api(api, mem, fake_symbols());
    CHECK(call(api, "symbols.lookup", {{"name", "OSInit"}}) == 0x80258AE8);
    CHECK(call(api, "symbols.lookup", {{"name", "nothing"}}).is_null());
    CHECK(call(api, "symbols.name_of", {{"address", 0x80258AE8}}) == "OSInit");
    CHECK(call(api, "symbols.name_of", {{"address", 0x80258AEC}}).is_null());
}

static void registry_rejects_duplicates() {
    ApiRegistry api;
    api.add({"a.b", {}, nullptr, nullptr, [](const json&) { return json(1); }});
    bool threw = false;
    try { api.add({"a.b", {}, nullptr, nullptr, nullptr}); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw && api.find("a.b") && !api.find("a.c") && api.paths() == std::vector<std::string>{"a.b"});
}

// Every member the schema marks implemented under a registry domain is registered, and nothing is registered that the schema does not list.
static void schema_consistency() {
    uc::guest_mem_init();
    GuestMemory mem(uc::guest_mem_base());
    ApiRegistry api;
    register_game_api(api, mem, fake_symbols());
    std::ifstream file(UC_API_SCHEMA);
    CHECK(file);
    const auto schema = json::parse(file);
    std::set<std::string> expected;
    for (const auto& domain : schema["domains"]) {
        if (domain["provider"] != "registry") continue;
        for (const auto& m : domain["members"]) {
            const std::string path = domain["name"].get<std::string>() + "." + m["name"].get<std::string>();
            if (m["status"] == "planned") { CHECK(!api.find(path)); continue; }
            expected.insert(path);
            const auto* member = api.find(path);
            if (!member) { std::fprintf(stderr, "schema member %s is %s but not registered\n", path.c_str(), m["status"].get<std::string>().c_str()); CHECK(false); }
            if (m["kind"] == "function") {
                CHECK(member->call);
                std::vector<std::string> names;
                for (const auto& a : m["args"]) names.push_back(a["name"]);
                CHECK(member->arg_names == names);
            } else {
                const std::string access = m["access"];
                CHECK(bool(member->get) == (access != "w") && bool(member->set) == (access != "r"));
            }
        }
    }
    for (const auto& path : api.paths()) if (!expected.count(path)) { std::fprintf(stderr, "registered member %s is not implemented in the schema\n", path.c_str()); CHECK(false); }
}

int main() {
    read_write_big_endian(); bounds(); float_roundtrip(); api_mem_functions(); api_mem_rejects(); symbols_lookup(); registry_rejects_duplicates(); schema_consistency();
    std::puts("ok");
}
