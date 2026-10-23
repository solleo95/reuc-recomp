#include "check.h"
#include "arc/arc_mods.h"
#include "arc/u8.h"
#include "u8_fixture.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <random>

using namespace uc::mods;
using namespace u8_fixture;
namespace fs = std::filesystem;

namespace {
fs::path temp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / (std::string("uc_arc_mods_") + name + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool has(const std::vector<uint8_t>& file, const std::string& text) {
    const auto needle = bytes(text);
    return std::search(file.begin(), file.end(), needle.begin(), needle.end()) != file.end();
}

std::vector<File> disc_archive() { return {{"inner/x.bin", bytes("disc-x")}, {"inner/y.bin", bytes("disc-y")}, {"top.txt", bytes("disc-top")}}; }

struct Rig {
    fs::path base_dir = temp_dir("rig");
    fs::path cache = base_dir / "cache";
    std::vector<ModEntry> mods;
    ModEntry& add(const std::string& id) {
        ModEntry m;
        m.manifest.id = id;
        m.root = base_dir / id;
        m.state = ModState::enabled;
        fs::create_directories(m.root);
        mods.push_back(std::move(m));
        return mods.back();
    }
    void member(const std::string& mod, const std::string& archive_and_member, const std::string& text) { write_bytes(base_dir / mod / "files" / fs::u8path(archive_and_member), bytes(text)); }
    void whole(const std::string& mod, const std::string& archive, const std::vector<uint8_t>& data) { write_bytes(base_dir / mod / "files" / fs::u8path(archive), data); }
    std::vector<const ModEntry*> active() const { std::vector<const ModEntry*> v; for (const auto& m : mods) v.push_back(&m); return v; }
    ArcBase base = [](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (lower == "data/chara/a.arc") return make_u8(disc_archive());
        return std::nullopt;
    };
    ArcBuild build() const { return build_arc_mods(cache, active(), base); }
    static std::vector<uint8_t> result(const ArcBuild& built, const char* archive = "Data/chara/A.arc") { CHECK(!built.root.empty()); return read_bytes(built.root / fs::u8path(archive)); }
};
std::string all_problems(const ArcBuild& built) {
    std::string text;
    for (const auto& [mod, lines] : built.problems) for (const auto& line : lines) text += mod + ": " + line + "\n";
    return text;
}
}  // namespace

static void member_replaces_inside_the_disc_archive() {
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/A.arc!/inner/x.bin", "mod-x");
    const auto built = rig.build();
    CHECK(built.problems.empty() && built.conflicts.empty());
    const auto out = Rig::result(built);
    std::string error;
    const auto archive = U8Archive::parse(out, error);
    CHECK(archive && archive->files() == (std::vector<std::string>{"inner/x.bin", "inner/y.bin", "top.txt"}));
    CHECK(has(out, "mod-x") && !has(out, "disc-x") && has(out, "disc-y") && has(out, "disc-top"));
}

static void new_member_is_added_and_case_is_ignored() {
    Rig rig;
    rig.add("m");
    rig.member("m", "data/chara/a.arc!/Extra/new.bin", "fresh");   // another spelling of the archive; a new folder
    rig.member("m", "Data/chara/A.arc!/INNER/Y.BIN", "mod-y");     // the existing member in other letters
    const auto built = rig.build();
    CHECK(built.problems.empty());
    const auto out = Rig::result(built, "data/chara/a.arc");
    std::string error;
    const auto archive = U8Archive::parse(out, error);
    CHECK(archive && archive->contains("Extra/new.bin") && archive->files().size() == 4);
    CHECK(has(out, "fresh") && has(out, "mod-y") && !has(out, "disc-y"));
}

static void later_mod_wins_and_conflict_is_listed() {
    Rig rig;
    rig.add("a");
    rig.add("b");
    rig.member("a", "Data/chara/A.arc!/inner/x.bin", "from-a");
    rig.member("b", "Data/chara/A.arc!/inner/x.bin", "from-b");
    rig.member("a", "Data/chara/A.arc!/inner/y.bin", "only-a");
    const auto built = rig.build();
    const auto out = Rig::result(built);
    CHECK(has(out, "from-b") && !has(out, "from-a") && has(out, "only-a"));
    CHECK(built.conflicts.size() == 1);
    CHECK(built.conflicts[0].find("Data/chara/A.arc!/inner/x.bin") != std::string::npos && built.conflicts[0].find("a and b") != std::string::npos);
}

static void whole_archive_of_a_later_mod_resets_earlier_members() {
    Rig rig;
    rig.add("a");
    rig.add("b");
    rig.member("a", "Data/chara/A.arc!/inner/x.bin", "from-a");
    rig.whole("b", "Data/chara/A.arc", make_u8({{"inner/x.bin", bytes("b-base-x")}, {"other.bin", bytes("b-other")}}));
    rig.member("b", "Data/chara/A.arc!/inner/y.bin", "b-member-y");
    const auto out = Rig::result(rig.build());
    CHECK(!has(out, "from-a") && has(out, "b-base-x") && has(out, "b-member-y") && has(out, "b-other") && !has(out, "disc-x"));
}

static void members_of_a_later_mod_apply_on_an_earlier_whole_archive() {
    Rig rig;
    rig.add("a");
    rig.add("b");
    rig.whole("a", "Data/chara/A.arc", make_u8({{"inner/x.bin", bytes("a-base-x")}, {"other.bin", bytes("a-other")}}));
    rig.member("b", "Data/chara/A.arc!/other.bin", "b-other");
    const auto out = Rig::result(rig.build());
    CHECK(has(out, "a-base-x") && has(out, "b-other") && !has(out, "a-other") && !has(out, "disc-x"));
}

static void unknown_archive_is_a_problem_with_the_path() {
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/Nope.arc!/x.bin", "x");
    rig.member("m", "Data/chara/A.arc!/inner/x.bin", "ok");
    const auto built = rig.build();
    CHECK(all_problems(built).find("m: arc: Data/chara/Nope.arc: the game has no such archive") != std::string::npos);
    CHECK(has(Rig::result(built), "ok"));   // the other archive is still built
}

static void damaged_base_is_a_problem_and_no_file() {
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/B.arc!/x.bin", "x");
    rig.base = [](const std::string& path) -> std::optional<std::vector<uint8_t>> { return path == "Data/chara/B.arc" ? std::optional(bytes("not an archive at all, but long enough for a header....")) : std::nullopt; };
    const auto built = rig.build();
    CHECK(built.root.empty());
    CHECK(all_problems(built).find("arc: Data/chara/B.arc: cannot be used as an archive") != std::string::npos);
    Rig whole;
    whole.add("m");
    whole.whole("m", "Data/chara/A.arc", bytes("garbage that is not a U8 archive, 32+ bytes long.."));
    whole.member("m", "Data/chara/A.arc!/x.bin", "x");
    const auto bad = whole.build();
    CHECK(bad.root.empty() && all_problems(bad).find("m: arc: Data/chara/A.arc: the archive of this mod cannot be used") != std::string::npos);
}

static void bad_member_paths_are_problems_not_fatal() {
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/A.arc!/top.txt/below.bin", "x");              // a file used as a folder
    rig.member("m", "Data/chara/A.arc!/inner", "x");                          // a folder replaced by a file
    rig.member("m", "Data/chara/A.arc!/sub/deep.arc!/z.bin", "x");            // an archive inside an archive
    rig.member("m", "Data/chara/A.arc!/fine.bin", "fine");
    const auto built = rig.build();
    const std::string problems = all_problems(built);
    CHECK(problems.find("top.txt/below.bin") != std::string::npos && problems.find("'inner' is a folder") != std::string::npos && problems.find("archives inside archives") != std::string::npos);
    CHECK(has(Rig::result(built), "fine"));
}

static void growth_over_256_kib_warns_but_applies() {
    Rig rig;
    rig.add("m");
    write_bytes(rig.base_dir / "m" / "files" / "Data" / "chara" / "A.arc!" / "big.bin", std::vector<uint8_t>(300000, 7));
    write_bytes(rig.base_dir / "m" / "files" / "Data" / "chara" / "A.arc!" / "small.bin", std::vector<uint8_t>(1000, 7));
    const auto built = rig.build();
    CHECK(!built.root.empty() && all_problems(built).find("arc: Data/chara/A.arc: ") != std::string::npos && all_problems(built).find("KiB bigger than the original") != std::string::npos);
    Rig small;
    small.add("m");
    write_bytes(small.base_dir / "m" / "files" / "Data" / "chara" / "A.arc!" / "small.bin", std::vector<uint8_t>(200000, 7));
    CHECK(small.build().problems.empty());
}

static void same_input_reuses_the_folder_and_changed_input_makes_a_new_one() {
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/A.arc!/inner/x.bin", "one");
    const auto first = rig.build();
    const auto again = rig.build();
    CHECK(!first.root.empty() && first.root == again.root);
    rig.member("m", "Data/chara/A.arc!/inner/x.bin", "two");
    const auto changed = rig.build();
    CHECK(changed.root != first.root && fs::exists(first.root / "Data" / "chara" / "A.arc") && has(Rig::result(changed), "two") && has(Rig::result(first), "one"));
    fs::resize_file(changed.root / "Data" / "chara" / "A.arc", 10);   // a cut-short copy of an earlier run is rebuilt
    const auto repaired = rig.build();
    CHECK(repaired.root == changed.root && has(Rig::result(repaired), "two"));
}

static void no_members_means_empty_root() {
    Rig rig;
    rig.add("m");
    rig.whole("m", "Data/chara/A.arc", make_u8(disc_archive()));   // only a whole archive: the overlay handles it
    write_bytes(rig.base_dir / "m" / "files" / "Data" / "other.bin", bytes("x"));
    const auto built = rig.build();
    CHECK(built.root.empty() && built.problems.empty());
}

static void member_names_with_non_ascii_letters() {
    const std::string name = reinterpret_cast<const char*>(u8"Ünï/ñ.bin");   // UTF-8 folder and file names
    Rig rig;
    rig.add("m");
    rig.member("m", "Data/chara/A.arc!/" + name, "utf8");
    const auto built = rig.build();
    CHECK(built.problems.empty());
    std::string error;
    const auto archive = U8Archive::parse(Rig::result(built), error);
    CHECK(archive && archive->contains(name));
}

static void prune_keeps_only_the_given_key() {
    const fs::path cache = temp_dir("prune");
    fs::create_directories(cache / "_arc" / "keep");
    fs::create_directories(cache / "_arc" / "old");
    fs::create_directories(cache / "_arc" / "half.tmp");
    fs::create_directories(cache / "_text" / "untouched");
    prune_arc_cache(cache, cache / "_arc" / "keep");
    CHECK(fs::exists(cache / "_arc" / "keep") && !fs::exists(cache / "_arc" / "old") && !fs::exists(cache / "_arc" / "half.tmp") && fs::exists(cache / "_text" / "untouched"));
}

int main() {
    member_replaces_inside_the_disc_archive();
    new_member_is_added_and_case_is_ignored();
    later_mod_wins_and_conflict_is_listed();
    whole_archive_of_a_later_mod_resets_earlier_members();
    members_of_a_later_mod_apply_on_an_earlier_whole_archive();
    unknown_archive_is_a_problem_with_the_path();
    damaged_base_is_a_problem_and_no_file();
    bad_member_paths_are_problems_not_fatal();
    growth_over_256_kib_warns_but_applies();
    same_input_reuses_the_folder_and_changed_input_makes_a_new_one();
    no_members_means_empty_root();
    member_names_with_non_ascii_letters();
    prune_keeps_only_the_given_key();
    std::puts("test_arc_mods: ok");
    return 0;
}
