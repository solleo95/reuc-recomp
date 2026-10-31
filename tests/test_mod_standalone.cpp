#include "check.h"
#include "standalone.h"
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
static void mod(const fs::path& dir, const std::string& id, const std::string& extra = "") {
    put(dir / id / "mod.json", R"({"id":")" + id + R"(","name":")" + id + R"(","version":"1.0","api":"1.0")" + (extra.empty() ? "" : "," + extra) + "}");
}

static void empty_folder_and_missing_section() {
    StandaloneMods none(fresh("sa_none"), ordered_json::object());
    CHECK(none.registry().entries().empty());
    CHECK(none.section() == none.registry().to_config());
    StandaloneMods missing(fs::current_path() / "sa_no_such_folder", ordered_json());   // no mods folder, no section: an empty list
    CHECK(missing.registry().entries().empty());
}

static void enable_order_native_round_trip() {
    const auto dir = fresh("sa_round");
    for (const char* id : {"a", "b", "c"}) mod(dir, id);
    StandaloneMods first(dir, ordered_json::object());
    CHECK(first.registry().set_enabled("a", true));
    CHECK(first.registry().move("c", -1));
    first.registry().set_allow_native(true);
    StandaloneMods second(dir, first.section());
    CHECK(second.registry().wanted("a") && !second.registry().wanted("b"));
    CHECK(second.registry().allow_native());
    CHECK(second.registry().entries()[1].manifest.id == "c" && second.registry().entries()[2].manifest.id == "b");
}

static void declared_setting_edit_saves_and_validates() {
    const auto dir = fresh("sa_setting");
    mod(dir, "a", R"("settings":[{"key":"n","type":"int","default":5,"min":0,"max":10,"label":{"en":"N"}}])");
    StandaloneMods first(dir, ordered_json::object());
    auto* settings = first.settings("a");
    CHECK(settings && first.settings("a") == settings && settings->get("n") == 5);
    CHECK(settings->set("n", 99) && settings->get("n") == 10);   // clamped like the game's loader
    first.mark_dirty();
    CHECK(first.take_dirty() && !first.take_dirty());
    const auto saved = first.section();
    CHECK(saved["a"]["settings"]["n"] == 10);
    StandaloneMods second(dir, saved);
    CHECK(second.settings("a")->get("n") == 10);
    CHECK(second.settings("nobody") == nullptr);
}

static void unknown_mod_settings_survive() {
    const auto dir = fresh("sa_gone");
    mod(dir, "a");
    ordered_json stored = ordered_json::object();
    stored["gone"]["settings"]["x"] = 1;   // a mod that is not installed: its settings stay
    StandaloneMods mods(dir, stored);
    mods.registry().set_enabled("a", true);
    CHECK(mods.section()["gone"]["settings"]["x"] == 1);
}

static void empty_game_id_skips_version_check() {
    const auto dir = fresh("sa_game");
    mod(dir, "a", R"("game":["X"])");
    ordered_json on = ordered_json::object();
    on["enabled"] = {"a"};
    StandaloneMods any(dir, on, "");
    CHECK(any.registry().entries()[0].state == ModState::enabled);
    StandaloneMods other(dir, on, "Y");
    CHECK(other.registry().entries()[0].state == ModState::incompatible);
    StandaloneMods same(dir, on, "X");
    CHECK(same.registry().entries()[0].state == ModState::enabled);
}

static void merge_creates_missing_objects() {
    const auto merged = merge_mod_settings(ordered_json::object(), {{"m", ordered_json{{"k", 1}}}});
    CHECK(merged["m"]["settings"]["k"] == 1);
    ordered_json base = ordered_json::object();
    base["m"] = 7;   // not an object: replaced, the settings win
    CHECK(merge_mod_settings(base, {{"m", ordered_json{{"k", 2}}}})["m"]["settings"]["k"] == 2);
}

int main() {
    empty_folder_and_missing_section();
    enable_order_native_round_trip();
    declared_setting_edit_saves_and_validates();
    unknown_mod_settings_survive();
    empty_game_id_skips_version_check();
    merge_creates_missing_objects();
    return 0;
}
