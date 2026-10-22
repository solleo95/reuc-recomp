#include "check.h"
#include "dvdfs.h"
#include "overlay.h"
#include "registry.h"
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace uc;

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

static std::string read_entry(const DvdFs& fs_, int entry) {
    std::string bytes(fs_.length(entry), '\0');
    CHECK(fs_.read(entry, bytes.data(), uint32_t(bytes.size()), 0) == int64_t(bytes.size()));
    return bytes;
}

static DvdFs disc(const fs::path& root) {
    put(root / "DATA" / "a.bin", "abc");
    put(root / "DATA" / "keep.bin", "keep");
    DvdFs dvd;
    CHECK(dvd.open_root(root));
    return dvd;
}

static void overlay_replaces_file() {
    const auto root = fresh("ov_disc1"), files = fresh("ov_files1");
    auto dvd = disc(root);
    const int before = dvd.entry_of("DATA/a.bin");
    const size_t count = dvd.entry_count();
    put(files / "data" / "A.BIN", "hello");   // other letter case than the disc
    CHECK(dvd.overlay(files) == 1);
    CHECK(dvd.entry_of("DATA/a.bin") == before && dvd.entry_count() == count);   // entry numbers do not move
    CHECK(dvd.length(before) == 5 && read_entry(dvd, before) == "hello");
    CHECK(read_entry(dvd, dvd.entry_of("DATA/keep.bin")) == "keep");
}

static void overlay_adds_file_and_dirs() {
    const auto root = fresh("ov_disc2"), files = fresh("ov_files2");
    auto dvd = disc(root);
    put(files / "DATA" / "new" / "deep" / "b.bin", "bee");
    CHECK(dvd.overlay(files) == 1);
    const int file = dvd.entry_of("DATA/new/deep/b.bin");
    CHECK(file > 0 && !dvd.is_dir(file) && read_entry(dvd, file) == "bee");
    CHECK(dvd.is_dir(dvd.entry_of("DATA/new")) && dvd.is_dir(dvd.entry_of("DATA/new/deep")));
    CHECK(dvd.path_of(file) == "DATA/new/deep/b.bin");
}

static void overlay_order() {
    const auto root = fresh("ov_disc3"), mods = fresh("ov_mods3");
    auto dvd = disc(root);
    for (const char* id : {"first", "second"}) {
        put(mods / id / "mod.json", std::string(R"({"id":")") + id + R"(","name":"n","version":"1","api":"1.0"})");
        put(mods / id / "files" / "DATA" / "a.bin", id);
    }
    nlohmann::ordered_json section = {{"enabled", {"first", "second"}}, {"order", {"first", "second"}}};
    const auto registry = mods::ModRegistry::discover(mods, section, {1, 0}, "RBUE08");
    CHECK(mods::apply_mod_overlays(dvd, registry.active()) == 2);
    CHECK(read_entry(dvd, dvd.entry_of("DATA/a.bin")) == "second");   // the later mod wins
}

static void overlay_ignores_escapes() {
    const auto root = fresh("ov_disc4"), files = fresh("ov_files4"), outside = fresh("ov_outside4");
    auto dvd = disc(root);
    put(outside / "secret.bin", "secret");
    const std::string command = "mklink /J \"" + (files / "escape").string() + "\" \"" + outside.string() + "\" > nul 2>&1";
    if (std::system(command.c_str()) != 0 || !fs::exists(files / "escape")) { std::puts("skip: cannot create a junction"); return; }
    put(files / "DATA" / "ok.bin", "ok");
    CHECK(dvd.overlay(files) == 1);
    CHECK(dvd.entry_of("escape/secret.bin") < 0 && dvd.entry_of("DATA/ok.bin") > 0);
}

int main() {
    overlay_replaces_file(); overlay_adds_file_and_dirs(); overlay_order(); overlay_ignores_escapes();
    std::puts("ok");
}
