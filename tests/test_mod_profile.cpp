#include "check.h"
#include "profile.h"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace uc::mods;

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

static std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static ModEntry entry(const std::string& id, bool gameplay) {
    ModEntry e;
    e.manifest.id = e.manifest.name = id;
    e.manifest.touches_gameplay = gameplay;
    e.state = ModState::enabled;
    return e;
}

static std::string tree_hash(const fs::path& dir) {   // names and contents of every file below dir, in path order
    std::string all;
    std::vector<fs::path> files;
    for (const auto& item : fs::recursive_directory_iterator(dir)) if (item.is_regular_file()) files.push_back(item.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) all += f.lexically_relative(dir).generic_string() + "=" + slurp(f) + "\n";
    return all;
}

static void vanilla_without_gameplay_mods() {
    const auto exe = fresh("profile_vanilla");
    put(exe / "saves" / "slot1.bin", "vanilla");
    const auto cosmetic = entry("textures", false);
    const auto none = resolve_save_root(exe, false, {});
    CHECK(none.root == exe / "saves" && none.gameplay_mods_allowed && none.warning.empty());
    const auto cosmetic_only = resolve_save_root(exe, false, {&cosmetic});
    CHECK(cosmetic_only.root == exe / "saves" && cosmetic_only.gameplay_mods_allowed);
    CHECK(!fs::exists(exe / "saves-modded"));
}

static void explicit_root_wins() {
    const auto exe = fresh("profile_explicit");
    put(exe / "saves" / "slot1.bin", "vanilla");
    const auto gameplay = entry("god", true);
    const auto choice = resolve_save_root(exe, true, {&gameplay});
    CHECK(choice.root.empty() && choice.gameplay_mods_allowed && choice.warning.empty());   // the caller's own root (the harness) is used as it is
    CHECK(!fs::exists(exe / "saves-modded"));
}

static void modded_copy_created_once_and_vanilla_untouched() {
    const auto exe = fresh("profile_modded");
    put(exe / "saves" / "slot1.bin", "vanilla one");
    put(exe / "saves" / "nested" / "slot2.bin", "vanilla two");
    const auto before = tree_hash(exe / "saves");
    const auto gameplay = entry("god", true);
    const auto first = resolve_save_root(exe, false, {&gameplay});
    CHECK(first.root == exe / "saves-modded" && first.gameplay_mods_allowed && first.warning.empty());
    CHECK(slurp(first.root / "slot1.bin") == "vanilla one" && slurp(first.root / "nested" / "slot2.bin") == "vanilla two");
    put(first.root / "slot1.bin", "changed while modded");   // the game writes here
    put(exe / "saves" / "slot3.bin", "new vanilla progress");
    const auto second = resolve_save_root(exe, false, {&gameplay});
    CHECK(second.root == first.root && slurp(second.root / "slot1.bin") == "changed while modded");   // not copied again
    CHECK(!fs::exists(second.root / "slot3.bin"));                                                      // and not refreshed from the vanilla saves
    fs::remove(exe / "saves" / "slot3.bin");
    CHECK(tree_hash(exe / "saves") == before);                                                           // the vanilla saves were never written
    const auto cosmetic = entry("textures", false);
    CHECK(resolve_save_root(exe, false, {&cosmetic}).root == exe / "saves" && fs::exists(exe / "saves-modded"));   // back to vanilla, the modded folder stays
}

static void missing_vanilla_creates_empty_modded() {
    const auto exe = fresh("profile_nosaves");
    const auto gameplay = entry("god", true);
    const auto choice = resolve_save_root(exe, false, {&gameplay});
    CHECK(choice.root == exe / "saves-modded" && fs::is_directory(choice.root) && choice.gameplay_mods_allowed);
    CHECK(fs::is_empty(choice.root) && !fs::exists(exe / "saves"));
}

static void copy_failure_falls_back_without_gameplay_mods() {
    const auto exe = fresh("profile_fail");
    put(exe / "saves" / "slot1.bin", "vanilla");
    put(exe / "saves-modded", "a file where the folder should be");   // the copy cannot be made
    const auto gameplay = entry("god", true);
    const auto choice = resolve_save_root(exe, false, {&gameplay});
    CHECK(choice.root == exe / "saves" && !choice.gameplay_mods_allowed && !choice.warning.empty());   // never let the gameplay mods touch the real saves
    CHECK(slurp(exe / "saves" / "slot1.bin") == "vanilla" && fs::is_regular_file(exe / "saves-modded"));
    CHECK(!fs::exists(exe / "saves-modded.partial"));
}

static void leftover_partial_copy_is_replaced() {
    const auto exe = fresh("profile_partial");
    put(exe / "saves" / "slot1.bin", "vanilla");
    put(exe / "saves-modded.partial" / "garbage.bin", "from a crashed copy");
    const auto gameplay = entry("god", true);
    const auto choice = resolve_save_root(exe, false, {&gameplay});
    CHECK(choice.root == exe / "saves-modded" && choice.gameplay_mods_allowed);
    CHECK(slurp(choice.root / "slot1.bin") == "vanilla" && !fs::exists(choice.root / "garbage.bin") && !fs::exists(exe / "saves-modded.partial"));
}

// The warning names the folder, whatever its characters (path::string() throws outside the system code page).
static void warning_for_a_non_code_page_folder() {
    const auto exe = fresh("profile_names") / fs::path(u8"日本語");
    put(exe / "saves" / "slot1.bin", "vanilla");
    put(exe / "saves-modded", "a file where the folder should be");
    const auto gameplay = entry("god", true);
    const auto choice = resolve_save_root(exe, false, {&gameplay});
    CHECK(!choice.gameplay_mods_allowed && choice.warning.find(reinterpret_cast<const char*>(u8"日本語")) != std::string::npos);
}

int main() {
    warning_for_a_non_code_page_folder();
    vanilla_without_gameplay_mods(); explicit_root_wins(); modded_copy_created_once_and_vanilla_untouched(); missing_vanilla_creates_empty_modded();
    copy_failure_falls_back_without_gameplay_mods(); leftover_partial_copy_is_replaced();
    std::puts("ok");
}
