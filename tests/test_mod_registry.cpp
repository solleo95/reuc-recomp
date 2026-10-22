#include "check.h"
#include "registry.h"
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using namespace uc::mods;
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

// A mod folder named `id` with the given extra manifest members.
static void mod(const fs::path& dir, const std::string& id, const std::string& extra = "", const std::string& api = "1.0") {
    put(dir / id / "mod.json", R"({"id":")" + id + R"(","name":")" + id + R"(","version":"1.0","api":")" + api + "\"" + (extra.empty() ? "" : "," + extra) + "}");
}

static const ApiVersion kHost{1, 3};

static ordered_json section(std::initializer_list<const char*> enabled) {
    ordered_json s = ordered_json::object();
    s["enabled"] = ordered_json::array();
    for (const char* id : enabled) s["enabled"].push_back(id);
    return s;
}

static const ModEntry* find(const ModRegistry& r, const std::string& id) {
    for (const auto& e : r.entries()) if (e.manifest.id == id) return &e;
    return nullptr;
}

static std::vector<std::string> active_ids(const ModRegistry& r) {
    std::vector<std::string> ids;
    for (const auto* e : r.active()) ids.push_back(e->manifest.id);
    return ids;
}

static void discover_mixed() {
    const auto dir = fresh("reg_mixed");
    mod(dir, "good");
    put(dir / "broken" / "mod.json", "{ nope");
    put(dir / "zdup_folder" / "mod.json", R"({"id":"good","name":"x","version":"1","api":"1.0"})");   // same id as "good": the folder found later loses
    const auto r = ModRegistry::discover(dir, section({"good"}), kHost, "RBUE08");
    CHECK(r.entries().size() == 3);
    CHECK(find(r, "good")->state == ModState::enabled);
    const ModEntry* broken = nullptr;
    const ModEntry* duplicate = nullptr;
    for (const auto& e : r.entries()) { if (e.folder_name == "broken") broken = &e; if (e.folder_name == "zdup_folder") duplicate = &e; }
    CHECK(broken && broken->state == ModState::error && !broken->messages.empty());
    CHECK(duplicate && duplicate->state == ModState::error && duplicate->messages[0].find("duplicate id") != std::string::npos);
    CHECK(active_ids(r) == std::vector<std::string>{"good"});   // the broken ones do not stop the good one
}

static void enabled_list_respected() {
    const auto dir = fresh("reg_enabled");
    mod(dir, "a"); mod(dir, "b"); mod(dir, "c");
    const auto r = ModRegistry::discover(dir, section({"b", "ghost"}), kHost, "RBUE08");
    CHECK(find(r, "a")->state == ModState::disabled && find(r, "b")->state == ModState::enabled && find(r, "c")->state == ModState::disabled);
    CHECK(r.entries().size() == 3);
}

static void order_follows_saved_list() {
    const auto dir = fresh("reg_order");
    mod(dir, "a"); mod(dir, "b"); mod(dir, "c"); mod(dir, "d");
    auto s = section({"c", "a"});
    s["order"] = {"c", "a", "b"};   // d is new: appended after the saved ones
    const auto r = ModRegistry::discover(dir, s, kHost, "RBUE08");
    std::vector<std::string> order;
    for (const auto& e : r.entries()) order.push_back(e.manifest.id);
    CHECK((order == std::vector<std::string>{"c", "a", "b", "d"}));
}

static void incompatible_api_and_game() {
    const auto dir = fresh("reg_api");
    mod(dir, "newer_major", "", "2.0"); mod(dir, "newer_minor", "", "1.5"); mod(dir, "older_ok", "", "1.0"); mod(dir, "same_minor", "", "1.3");
    mod(dir, "wrong_game", R"("game":["SOMEGAME"])"); mod(dir, "right_game", R"("game":["RBUE08"])");
    const auto r = ModRegistry::discover(dir, section({"newer_major", "newer_minor", "older_ok", "same_minor", "wrong_game", "right_game"}), kHost, "RBUE08");
    CHECK(find(r, "newer_major")->state == ModState::incompatible && find(r, "newer_minor")->state == ModState::incompatible);
    CHECK(find(r, "older_ok")->state == ModState::enabled && find(r, "same_minor")->state == ModState::enabled);
    CHECK(find(r, "wrong_game")->state == ModState::incompatible && find(r, "right_game")->state == ModState::enabled);
}

static void dependencies() {
    const auto dir = fresh("reg_deps");
    put(dir / "core" / "mod.json", R"({"id":"core","name":"core","version":"1.2.0","api":"1.0"})");
    mod(dir, "needs_missing", R"("depends":[{"id":"nothere"}])");
    mod(dir, "needs_disabled", R"("depends":[{"id":"core"}])");
    mod(dir, "needs_new_core", R"("depends":[{"id":"core","min_version":"1.10"}])");
    mod(dir, "needs_ok_core", R"("depends":[{"id":"core","min_version":"1.2"}])");
    auto r = ModRegistry::discover(dir, section({"needs_missing", "needs_disabled", "needs_new_core", "needs_ok_core"}), kHost, "RBUE08");
    CHECK(find(r, "needs_missing")->state == ModState::missing_dependency);
    CHECK(find(r, "needs_disabled")->state == ModState::dependency_disabled);   // core exists but is not enabled
    CHECK(find(r, "needs_new_core")->state == ModState::dependency_disabled);
    CHECK(find(r, "needs_ok_core")->state == ModState::dependency_disabled);
    r = ModRegistry::discover(dir, section({"core", "needs_new_core", "needs_ok_core"}), kHost, "RBUE08");
    CHECK(find(r, "core")->state == ModState::enabled);
    CHECK(find(r, "needs_new_core")->state == ModState::missing_dependency);   // 1.2.0 < 1.10
    CHECK(find(r, "needs_ok_core")->state == ModState::enabled);
    CHECK(r.set_enabled("core", false));
    CHECK(find(r, "needs_ok_core")->state == ModState::dependency_disabled);
}

static void dependency_cycle() {
    const auto dir = fresh("reg_cycle");
    mod(dir, "a", R"("depends":[{"id":"b"}])"); mod(dir, "b", R"("depends":[{"id":"a"}])"); mod(dir, "c");
    const auto r = ModRegistry::discover(dir, section({"a", "b", "c"}), kHost, "RBUE08");
    CHECK(find(r, "a")->state == ModState::error && find(r, "b")->state == ModState::error);
    CHECK(find(r, "a")->messages[0].find("cycle") != std::string::npos);
    CHECK(active_ids(r) == std::vector<std::string>{"c"});
}

static void dependency_order() {
    const auto dir = fresh("reg_depord");
    mod(dir, "a"); mod(dir, "b", R"("depends":[{"id":"a"}])");
    auto s = section({"b", "a"});
    s["order"] = {"b", "a"};
    const auto r = ModRegistry::discover(dir, s, kHost, "RBUE08");
    CHECK((active_ids(r) == std::vector<std::string>{"a", "b"}));
}

static void conflicts() {
    const auto dir = fresh("reg_conflicts");
    mod(dir, "one"); mod(dir, "two"); mod(dir, "three");
    put(dir / "one" / "files" / "DATA" / "x.bin", "1");
    put(dir / "two" / "files" / "data" / "X.BIN", "2");
    put(dir / "one" / "textures" / "tex1_8x8_00_1.dds", "1");
    put(dir / "three" / "textures" / "tex1_8x8_00_1.dds", "3");
    put(dir / "three" / "files" / "other.bin", "3");
    auto s = section({"one", "two", "three"});
    s["order"] = {"one", "two", "three"};
    const auto r = ModRegistry::discover(dir, s, kHost, "RBUE08");
    const auto found = r.conflicts();
    CHECK(found.size() == 2);
    for (const auto& c : found) {
        if (c.path == "files/data/x.bin") CHECK((c.mod_ids == std::vector<std::string>{"one", "two"}));
        else { CHECK(c.path == "textures/tex1_8x8_00_1.dds"); CHECK((c.mod_ids == std::vector<std::string>{"one", "three"})); }
    }
}

static void to_config_roundtrip() {
    const auto dir = fresh("reg_config");
    mod(dir, "a"); mod(dir, "b"); mod(dir, "c");
    ordered_json s = section({"a"});
    s["a"] = {{"settings", {{"volume", 3}}}};   // a mod's own section must survive
    auto r = ModRegistry::discover(dir, s, kHost, "RBUE08");
    CHECK(r.set_enabled("c", true));
    CHECK(r.move("c", -1));
    r.set_allow_native(true);
    r.set_native_hash("a", "abc123");
    const auto saved = r.to_config();
    CHECK(saved["a"]["settings"]["volume"] == 3 && saved["allow_native"] == true && saved["native_hash"]["a"] == "abc123");
    const auto again = ModRegistry::discover(dir, saved, kHost, "RBUE08");
    CHECK(again.entries().size() == 3);
    for (size_t i = 0; i < 3; ++i) CHECK(again.entries()[i].manifest.id == r.entries()[i].manifest.id && again.entries()[i].state == r.entries()[i].state);
    CHECK(again.allow_native() && again.native_hash("a") == "abc123" && again.native_hash("b").empty());
    CHECK(!r.move(r.entries().front().manifest.id, -1) && !r.move(r.entries().back().manifest.id, 1));   // no step past either end
    CHECK(!r.set_enabled("nothere", true));
}

static void reserved_keys_not_mods() {
    const auto dir = fresh("reg_reserved");
    for (const char* id : {"enabled", "allow_native", "native_hash", "order"}) put(dir / id / "mod.json", std::string(R"({"id":")") + id + R"(","name":"x","version":"1","api":"1.0"})");
    const auto r = ModRegistry::discover(dir, ordered_json::object(), kHost, "RBUE08");
    CHECK(r.entries().size() == 4);
    for (const auto& e : r.entries()) CHECK(e.state == ModState::error);
}

int main() {
    discover_mixed(); enabled_list_respected(); order_follows_saved_list(); incompatible_api_and_game(); dependencies(); dependency_cycle();
    dependency_order(); conflicts(); to_config_roundtrip(); reserved_keys_not_mods();
    std::puts("ok");
}
