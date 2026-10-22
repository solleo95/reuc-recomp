#include "check.h"
#include "data_patch.h"
#include <fstream>

namespace fs = std::filesystem;
using namespace uc::mods;
using nlohmann::json;

namespace {
struct Rig {
    ApiRegistry api;
    std::vector<std::string> writes;   // "<path> <args> <value>" in the order they happened

    Rig() {
        const auto settable = [this](const char* path, const char* scope, bool writable = true) {
            Member member;
            member.path = path;
            member.scope = scope;
            member.get = [](const json&) { return json(0); };
            if (writable) member.set = [this, name = std::string(path)](const json& args, const json& value) {
                if (value.is_string() && value == "boom") throw ApiError("refused");
                writes.push_back(name + " " + args.dump() + " " + value.dump());
            };
            api.add(std::move(member));
        };
        settable("weapon.damage", "id");
        settable("weapon.capacity", "id");
        settable("player.magazine", "slot");
        settable("player.invulnerable", "slot");
        settable("camera.view", "", false);   // read-only
        Member global;
        global.path = "game.difficulty";
        global.set = [this](const json& args, const json& value) { writes.push_back("game.difficulty " + args.dump() + " " + value.dump()); };
        api.add(std::move(global));
    }
};

fs::path fresh(const char* name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir / "data");
    return dir;
}
void put(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
}  // namespace

static void applies_flat_values() {
    Rig rig;
    const auto root = fresh("patch_flat");
    put(root / "data" / "weapons.json", R"({"weapon.0.damage": 25.5, "weapon.2.capacity": 60, "player.1.invulnerable": true, "game.difficulty": "hard"})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.errors.empty() && result.applied == 4);
    CHECK(rig.writes.size() == 4);
    CHECK(rig.writes[0] == "weapon.damage {\"id\":0} 25.5" && rig.writes[1] == "weapon.capacity {\"id\":2} 60");
    CHECK(rig.writes[2] == "player.invulnerable {\"slot\":1} true" && rig.writes[3] == "game.difficulty {} \"hard\"");
}

static void unknown_path_reported_others_applied() {
    Rig rig;
    const auto root = fresh("patch_unknown");
    put(root / "data" / "a.json", R"({"weapon.0.damage": 5, "weapon.0.fire_rate": 9, "nothing.here": 1, "weapon.damage": 3, "weapon.x.damage": 1, "weapon.0.damage.extra": 1})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 1 && rig.writes.size() == 1);
    CHECK(result.errors.size() == 5);
    for (const auto& e : result.errors) CHECK(e.find("a.json") != std::string::npos);   // each message names the file
    const auto joined = [&] { std::string all; for (const auto& e : result.errors) all += e + "\n"; return all; }();
    CHECK(joined.find("weapon.0.fire_rate") != std::string::npos && joined.find("not available") != std::string::npos);   // planned or unknown members say so
    CHECK(joined.find("weapon.x.damage") != std::string::npos);                                                              // the entity must be a whole number
}

static void readonly_member_rejected() {
    Rig rig;
    const auto root = fresh("patch_readonly");
    put(root / "data" / "a.json", R"({"camera.view": [1, 2, 3], "weapon.0.damage": 7})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 1 && result.errors.size() == 1 && result.errors[0].find("read-only") != std::string::npos);
}

static void api_refusal_is_reported_not_thrown() {
    Rig rig;
    const auto root = fresh("patch_refused");
    put(root / "data" / "a.json", R"({"weapon.0.damage": "boom", "weapon.0.capacity": 12})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 1 && result.errors.size() == 1 && result.errors[0].find("refused") != std::string::npos);
}

static void files_in_name_order_later_wins() {
    Rig rig;
    const auto root = fresh("patch_order");
    put(root / "data" / "b.json", R"({"weapon.0.damage": 2})");
    put(root / "data" / "a.json", R"({"weapon.0.damage": 1})");
    put(root / "data" / "notes.txt", "ignored");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 2 && rig.writes.size() == 2 && rig.writes[0].back() == '1' && rig.writes[1].back() == '2');
}

static void invalid_json_file_reported_not_thrown() {
    Rig rig;
    const auto root = fresh("patch_invalid");
    put(root / "data" / "a.json", "{ not json");
    put(root / "data" / "b.json", R"({"weapon.0.damage": 4})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 1 && result.errors.size() == 1 && result.errors[0].find("a.json") != std::string::npos);
}

static void non_object_root_rejected() {
    Rig rig;
    const auto root = fresh("patch_array");
    put(root / "data" / "a.json", "[1, 2, 3]");
    put(root / "data" / "b.json", "42");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 0 && result.errors.size() == 2);
}

static void no_data_folder_is_fine() {
    Rig rig;
    const auto root = fs::current_path() / "patch_none";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 0 && result.errors.empty());
}

static void unreadable_entries_and_huge_files_are_skipped() {
    Rig rig;
    const auto root = fresh("patch_big");
    fs::create_directories(root / "data" / "folder.json");   // a directory with a .json name
    put(root / "data" / "huge.json", std::string(2 * 1024 * 1024, ' ') + "{}");
    put(root / "data" / "ok.json", R"({"weapon.0.damage": 9})");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.applied == 1 && result.errors.size() == 1 && result.errors[0].find("huge.json") != std::string::npos);
}

// A file name outside the system code page (path::string() throws for it) is reported under its UTF-8 name.
static void non_code_page_file_name_is_reported() {
    Rig rig;
    const auto root = fresh("patch_names");
    put(root / "data" / std::filesystem::path(u8"日本語.json"), "{not json");
    const auto result = apply_data_patches(root, rig.api);
    CHECK(result.errors.size() == 1 && result.errors[0].find(reinterpret_cast<const char*>(u8"日本語.json")) == 0);
}

int main() {
    non_code_page_file_name_is_reported();
    applies_flat_values(); unknown_path_reported_others_applied(); readonly_member_rejected(); api_refusal_is_reported_not_thrown(); files_in_name_order_later_wins();
    invalid_json_file_reported_not_thrown(); non_object_root_rejected(); no_data_folder_is_fine(); unreadable_entries_and_huge_files_are_skipped();
    std::puts("ok");
}
