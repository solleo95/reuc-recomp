#include "check.h"
#include "dvdfs.h"
#include "overlay.h"
#include "registry.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

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

// Mods are switched on and off while the game runs: taking the overlays back restores the disc's own files, files a mod added disappear, and putting
// them on again reuses the same entry numbers (a number the game already holds never points somewhere else).
static void overlays_can_be_taken_back_and_reapplied() {
    const auto root = fresh("ov_disc5"), files = fresh("ov_files5"), other = fresh("ov_files5b");
    auto dvd = disc(root);
    const int original = dvd.entry_of("DATA/a.bin");
    put(files / "DATA" / "a.bin", "replaced!");
    put(files / "DATA" / "extra" / "x.bin", "xx");
    CHECK(dvd.overlay(files) == 2);
    const int added = dvd.entry_of("DATA/extra/x.bin"), folder = dvd.entry_of("DATA/extra");
    CHECK(added > 0 && folder > 0 && dvd.length(original) == 9);
    dvd.clear_overlays();
    CHECK(dvd.entry_of("DATA/extra/x.bin") == -1 && dvd.entry_of("DATA/extra") == -1);           // gone
    CHECK(dvd.length(added) == 0 && dvd.read(added, nullptr, 0, 0) == -1 && dvd.path_of(added).empty());   // a number the game still holds is harmless
    CHECK(dvd.length(original) == 3 && read_entry(dvd, original) == "abc");                       // the disc's own file is back
    put(other / "DATA" / "extra" / "x.bin", "different");
    CHECK(dvd.overlay(files) == 2 && dvd.overlay(other) == 1);
    CHECK(dvd.entry_of("DATA/extra/x.bin") == added && dvd.entry_of("DATA/extra") == folder);     // the same numbers again
    CHECK(read_entry(dvd, added) == "different" && read_entry(dvd, original) == "replaced!");
    dvd.clear_overlays();
    dvd.clear_overlays();                                                                          // harmless twice
    CHECK(read_entry(dvd, original) == "abc" && dvd.entry_of("DATA/extra/x.bin") == -1);
}

// The disc tree is read by the game's threads while the menu thread changes the overlays.
static void overlays_change_while_the_game_reads() {
    const auto root = fresh("ov_disc6"), files = fresh("ov_files6");
    auto dvd = disc(root);
    put(files / "DATA" / "a.bin", "replaced!");
    put(files / "DATA" / "more" / "m.bin", "mm");
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        char buffer[16];
        const int a = dvd.entry_of("DATA/a.bin");
        while (!stop) {
            const int64_t n = dvd.read(a, buffer, sizeof buffer, 0);
            if (n != 3 && n != 9) { std::puts("torn read"); std::abort(); }   // always one whole version of the file
            dvd.entry_of("DATA/more/m.bin");
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    for (int i = 0; i < 300; ++i) { dvd.overlay(files); dvd.clear_overlays(); }
    stop = true;
    reader.join();
}

// set_overlays swaps in one step: a reader never sees the disc's own file in between, and a path that comes back as another kind gets a new number.
static void set_overlays_is_atomic() {
    const auto root = fresh("ov_disc7"), files = fresh("ov_files7"), other = fresh("ov_files7b");
    auto dvd = disc(root);
    put(files / "DATA" / "a.bin", "replaced!");
    put(other / "DATA" / "a.bin", "other-one");
    CHECK(dvd.set_overlays({files}) == 1);
    const int a = dvd.entry_of("DATA/a.bin");
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        char buffer[16];
        while (!stop) {
            if (dvd.read(a, buffer, sizeof buffer, 0) != 9) { std::puts("saw the disc file or a torn read"); std::abort(); }
            std::this_thread::sleep_for(std::chrono::microseconds(50));   // a real reader is not a tight loop (SRW locks do not promise writers a turn against one)
        }
    });
    for (int i = 0; i < 300; ++i) dvd.set_overlays({i % 2 ? files : other});
    stop = true;
    reader.join();
    dvd.set_overlays({});
    CHECK(read_entry(dvd, a) == "abc");
    put(files / "DATA" / "kind" / "x.bin", "x");
    dvd.set_overlays({files});
    const int folder = dvd.entry_of("DATA/kind");
    CHECK(dvd.is_dir(folder));
    dvd.set_overlays({});
    fs::remove_all(files / "DATA" / "kind");
    put(files / "DATA" / "kind", "now a file");
    dvd.set_overlays({files});
    const int again = dvd.entry_of("DATA/kind");
    CHECK(again > 0 && again != folder && !dvd.is_dir(again));
}

int main() {
    set_overlays_is_atomic();
    overlays_can_be_taken_back_and_reapplied(); overlays_change_while_the_game_reads();
    overlay_replaces_file(); overlay_adds_file_and_dirs(); overlay_order(); overlay_ignores_escapes();
    std::puts("ok");
}
