#include "check.h"
#include "text/text_mods.h"
#include "text/txtres.h"
#include "txtres_fixture.h"
#include <fstream>
#include <iterator>
#include <random>

using namespace uc::mods;
using namespace txtres_fixture;
namespace fs = std::filesystem;

namespace {
fs::path temp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / (std::string("uc_text_mods_") + name + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::vector<Rec> sample() { return {{1, "", {"Enter"}}, {2, "VO_0000002", {"first", "second"}}, {7, "", {"Seven %s"}}, {12, "", {"Twelve"}}}; }

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
    std::vector<const ModEntry*> active() const { std::vector<const ModEntry*> v; for (const auto& m : mods) v.push_back(&m); return v; }
    TextBase base = [](const std::string& lang) -> std::optional<std::vector<uint8_t>> {
        if (lang == "us" || lang == "en") return make_txtres(sample());
        return std::nullopt;
    };
    TextBuild build() const { return build_text_mods(cache, active(), base); }
    static std::vector<TextRecord> records(const fs::path& root, const char* lang) {
        std::string error;
        const auto r = read_txtres(read_bytes(root / "Data" / "message" / (std::string("mes_") + lang + ".txtres")), error);
        CHECK(r.has_value());
        return *r;
    }
};
}  // namespace

static void single_mod_patches_the_language_file() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"Betreten","7":"Sieben %s"})");
    const auto build = rig.build();
    CHECK(!build.root.empty() && build.problems.empty());
    const auto us = Rig::records(build.root, "us");
    CHECK(us[0].lines == std::vector<std::string>{"Betreten"} && us[2].lines == std::vector<std::string>{"Sieben %s"} && us[3].lines == std::vector<std::string>{"Twelve"});
    CHECK(!fs::exists(build.root / "Data" / "message" / "mes_en.txtres"));   // only the languages with changes are written
}

static void all_json_then_language_json_then_later_mod() {
    Rig rig;
    const auto& a = rig.add("a");
    write(a.root / "text" / "all.json", R"({"1":"all-a","2":"two-a","12":"twelve-a"})");
    write(a.root / "text" / "us.json", R"({"2":"two-us-a"})");
    const auto& b = rig.add("b");
    write(b.root / "text" / "all.json", R"({"12":"twelve-b"})");
    const auto build = rig.build();
    const auto us = Rig::records(build.root, "us"), en = Rig::records(build.root, "en");
    CHECK(us[0].lines[0] == "all-a" && us[1].lines[0] == "two-us-a" && us[3].lines[0] == "twelve-b");   // language file beats all.json of the same mod; the later mod beats the earlier
    CHECK(en[0].lines[0] == "all-a" && en[1].lines[0] == "two-a" && en[3].lines[0] == "twelve-b");     // all.json reaches every language the game has
    CHECK(build.problems.empty());
}

static void line_breaks_split_into_lines() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"a\nb\nc","2":"single"})");
    const auto us = Rig::records(rig.build().root, "us");
    CHECK((us[0].lines == std::vector<std::string>{"a", "b", "c"}) && us[1].lines == std::vector<std::string>{"single"});
}

static void unknown_id_bad_json_and_bad_values_are_problems_not_fatal() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"fine","999":"nobody","x7":"bad key","7":5,"-3":"neg","12":null})");
    write(mod.root / "text" / "en.json", "{ not json");
    write(mod.root / "text" / "fr.json", R"({"1":"x"})");   // the game has no French file here
    write(mod.root / "text" / "klingon.json", R"({"1":"x"})");
    write(mod.root / "text" / "it.json", R"([1,2])");
    const auto build = rig.build();
    const auto& problems = build.problems.at("one");
    const auto has = [&](const char* a, const char* b) { for (const auto& p : problems) if (p.rfind("text:", 0) == 0 && p.find(a) != std::string::npos && p.find(b) != std::string::npos) return true; return false; };
    CHECK(has("us.json", "999") && has("us.json", "x7") && has("us.json", "\"7\"") && has("us.json", "-3") && has("us.json", "\"12\""));
    CHECK(has("en.json", "JSON") && has("fr.json", "no text file") && has("klingon.json", "language") && has("it.json", "object"));
    const auto us = Rig::records(build.root, "us");
    CHECK(us[0].lines[0] == "fine" && us[2].lines[0] == "Seven %s");   // the good id still applies
}

static void limits() {
    Rig rig;
    const auto& mod = rig.add("one");
    std::string long_line(2049, 'x');
    std::string many = "l";
    for (int i = 0; i < 32; ++i) many += "\\nl";   // 33 lines
    write(mod.root / "text" / "us.json", R"({"1":")" + long_line + R"(","2":")" + many + R"(","7":"nul\u0000here","12":"ok"})");
    const auto build = rig.build();
    CHECK(build.problems.at("one").size() == 3);
    const auto us = Rig::records(build.root, "us");
    CHECK(us[0].lines[0] == "Enter" && us[2].lines[0] == "Seven %s" && us[3].lines[0] == "ok");
    Rig edge;
    const auto& e = edge.add("e");
    std::string longest(2048, 'y'), lines32 = "z";
    for (int i = 0; i < 31; ++i) lines32 += "\\nz";
    write(e.root / "text" / "us.json", R"({"1":")" + longest + R"(","2":")" + lines32 + R"("})");
    const auto ok = edge.build();
    CHECK(ok.problems.empty() && Rig::records(ok.root, "us")[0].lines[0].size() == 2048 && Rig::records(ok.root, "us")[1].lines.size() == 32);
}

static void same_input_reuses_the_folder_and_changed_input_makes_a_new_one() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"v1"})");
    const auto first = rig.build();
    const auto again = rig.build();
    CHECK(first.root == again.root && fs::exists(first.root / "Data" / "message" / "mes_us.txtres"));
    write(mod.root / "text" / "us.json", R"({"1":"v2"})");
    const auto changed = rig.build();
    CHECK(changed.root != first.root && Rig::records(first.root, "us")[0].lines[0] == "v1" && Rig::records(changed.root, "us")[0].lines[0] == "v2");   // the old version stays readable
    prune_text_cache(rig.cache, changed.root);
    CHECK(!fs::exists(first.root) && fs::exists(changed.root));
}

static void no_text_means_empty_root() {
    Rig rig;
    rig.add("plain");
    const auto& unknown_only = rig.add("lost");
    write(unknown_only.root / "text" / "us.json", R"({"4242":"nobody"})");   // every id unknown: nothing changes, nothing is written
    const auto build = rig.build();
    CHECK(build.root.empty() && build.problems.count("plain") == 0 && build.problems.at("lost").size() == 1);
    CHECK(build_text_mods(rig.cache, {}, rig.base).root.empty());
}

static void missing_base_is_a_problem() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"x"})");
    const auto none = build_text_mods(rig.cache, rig.active(), [](const std::string&) { return std::optional<std::vector<uint8_t>>(); });
    CHECK(none.root.empty() && none.problems.at("one").size() == 1 && none.problems.at("one")[0].find("no text file") != std::string::npos);
    const auto damaged = build_text_mods(rig.cache, rig.active(), [](const std::string&) { return std::optional<std::vector<uint8_t>>(std::vector<uint8_t>(100, 7)); });
    CHECK(damaged.root.empty() && damaged.problems.at("one").size() == 1);
}

static void all_json_unknown_id_is_reported_once_when_no_language_has_it() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "all.json", R"({"1":"yes","555":"nowhere"})");
    const auto build = rig.build();
    CHECK(build.problems.at("one").size() == 1 && build.problems.at("one")[0].find("555") != std::string::npos);
    CHECK(Rig::records(build.root, "en")[0].lines[0] == "yes");
}

static void prune_keeps_only_the_given_key() {
    const fs::path cache = temp_dir("prune");
    fs::create_directories(cache / "_text" / "aaa" / "Data");
    fs::create_directories(cache / "_text" / "bbb" / "Data");
    fs::create_directories(cache / "_text" / "ccc.tmp");
    fs::create_directories(cache / "other");
    prune_text_cache(cache, cache / "_text" / "bbb");
    CHECK(!fs::exists(cache / "_text" / "aaa") && fs::exists(cache / "_text" / "bbb") && !fs::exists(cache / "_text" / "ccc.tmp") && fs::exists(cache / "other"));
    prune_text_cache(cache / "never", {});   // nothing there: no error
    prune_text_cache(cache, {});             // no text this session: every version goes
    CHECK(!fs::exists(cache / "_text" / "bbb"));
}

int main() {
    single_mod_patches_the_language_file();
    all_json_then_language_json_then_later_mod();
    line_breaks_split_into_lines();
    unknown_id_bad_json_and_bad_values_are_problems_not_fatal();
    limits();
    same_input_reuses_the_folder_and_changed_input_makes_a_new_one();
    no_text_means_empty_root();
    missing_base_is_a_problem();
    all_json_unknown_id_is_reported_once_when_no_language_has_it();
    prune_keeps_only_the_given_key();
    std::puts("test_text_mods: ok");
    return 0;
}
