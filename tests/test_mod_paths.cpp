#include "check.h"
#include "mod_paths.h"
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace uc::mods;

static fs::path fresh(const std::u8string& name) {   // scratch folder inside the test's working directory (the build tree)
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static bool inside(const fs::path& path, const fs::path& root) {
    const auto p = fs::weakly_canonical(path), r = fs::weakly_canonical(root);
    return std::mismatch(r.begin(), r.end(), p.begin(), p.end()).first == r.end();
}

static void confine_ok() {
    const auto root = fresh(u8"mod_paths_ok");
    const auto path = confine(root, "scripts/main.lua");
    CHECK(path && inside(*path, root) && path->filename() == "main.lua");
}

static void confine_rejects() {
    const auto root = fresh(u8"mod_paths_rej");
    for (const char* bad : {"../a", "a/../../b", "C:\\x", "C:/x", "\\\\server\\share\\x", "/x", "a:stream", "", "a//b"})
        CHECK(!confine(root, bad));
}

static void confine_symlink() {
    const auto root = fresh(u8"mod_paths_link"), outside = fresh(u8"mod_paths_link_outside");
    write(outside / "secret.txt", "x");
    const std::string command = "mklink /J \"" + (root / "escape").string() + "\" \"" + outside.string() + "\" > nul 2>&1";
    if (std::system(command.c_str()) != 0 || !fs::exists(root / "escape")) { std::puts("skip: cannot create a junction"); return; }
    CHECK(!confine(root, "escape/secret.txt"));
    CHECK(confine(root, "plain/file.txt"));
}

static void find_folders() {
    const auto dir = fresh(u8"mod_paths_find");
    write(dir / "a" / "mod.json", "{\"id\":\"a\"}");
    fs::create_directories(dir / "b");
    write(dir / "c", "file");
    write(dir / ".hidden" / "mod.json", "{}");
    const auto found = find_mod_folders(dir);
    CHECK(found.size() == 1 && found[0].folder_name == "a" && found[0].manifest_text == "{\"id\":\"a\"}" && found[0].root == dir / "a");
    CHECK(find_mod_folders(dir / "missing").empty());
}

static void find_folders_unreadable_manifest() {
    const auto dir = fresh(u8"mod_paths_unreadable");
    fs::create_directories(dir / "a" / "mod.json");   // a directory where the manifest should be
    const auto found = find_mod_folders(dir);
    CHECK(found.size() == 1 && found[0].manifest_text.empty());
}

static void non_ascii_root() {
    const auto root = fresh(u8"mod_paths_\u041c\u043e\u0434\u044b");
    write(root / u8"\u043c\u043e\u0434" / "mod.json", "{}");
    const auto found = find_mod_folders(root);
    CHECK(found.size() == 1 && found[0].manifest_text == "{}");
    const auto path = confine(root, "scripts/\xD0\xBF.lua");   // UTF-8 name
    CHECK(path && path->filename() == fs::path(u8"\u043f.lua"));
    write(*path, "hello");
    const auto text = read_file(*path);
    CHECK(text && *text == "hello");
    CHECK(!read_file(root / "missing.txt"));
}

int main() {
    confine_ok(); confine_rejects(); confine_symlink(); find_folders(); find_folders_unreadable_manifest(); non_ascii_root();
    std::puts("ok");
}
