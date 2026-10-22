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
    TextBase base = [](const std::string& lang) -> std::optional<TextBaseFile> {
        if (lang == "us" || lang == "en") return TextBaseFile{make_txtres(sample()), ""};
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
    CHECK(Rig::records(ok.root, "us")[0].lines[0].size() == 2048 && Rig::records(ok.root, "us")[1].lines.size() == 32);   // at the limits: accepted
    CHECK(ok.problems.at("e").size() == 2 && ok.problems.at("e")[0].find("longer") != std::string::npos);   // ... with a note each: far beyond what the original file has
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

// A folder of the cache that was cut short (disk full, antivirus) is rebuilt instead of reused.
static void a_damaged_cache_folder_is_rebuilt() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"1":"v1"})");
    const auto first = rig.build();
    const fs::path file = first.root / "Data" / "message" / "mes_us.txtres";
    const auto whole = read_bytes(file);
    fs::resize_file(file, whole.size() / 2);
    const auto again = rig.build();
    CHECK(again.root == first.root && read_bytes(file) == whole);   // the same key, repaired
    std::string error;
    CHECK(read_txtres(read_bytes(file), error).has_value());
}

// The game feeds some texts to printf: the placeholders of a record are part of its contract.
static void placeholders_must_match_the_original() {
    Rig rig;
    const auto& mod = rig.add("one");
    write(mod.root / "text" / "us.json", R"({"7":"Sieben","1":"50%% done %s","2":"100% damage","12":"ok"})");   // 7 needs its %s; 1 has none in the original; 2 has "% d"
    const auto build = rig.build();
    const auto us = Rig::records(build.root, "us");
    CHECK(us[2].lines[0] == "Seven %s");           // refused: the original keeps its text
    CHECK(us[0].lines[0] == "Enter");               // refused: a %s the original does not have
    CHECK(us[1].lines[0] == "first");               // refused: "% d" is a printf conversion
    CHECK(us[3].lines[0] == "ok");                  // no placeholders on either side
    const auto& problems = build.problems.at("one");
    const auto has = [&](const char* id, const char* word) { for (const auto& p : problems) if (p.find(id) != std::string::npos && p.find(word) != std::string::npos) return true; return false; };
    CHECK(problems.size() == 3 && has("\"7\"", "placeholder") && has("\"1\"", "placeholder") && has("\"2\"", "placeholder"));
    write(mod.root / "text" / "us.json", R"({"7":"Sieben %s","1":"50%% done","2":"a %%s b %d"})");   // %% is a percent sign; 2 gets a %d it did not have
    const auto fixed = rig.build();
    const auto us2 = Rig::records(fixed.root, "us");
    CHECK(us2[2].lines[0] == "Sieben %s" && us2[0].lines[0] == "50%% done" && us2[1].lines[0] == "first");
    CHECK(fixed.problems.at("one").size() == 1);
}

static void unknown_ids_are_summarised_keys_truncated_and_problems_capped() {
    Rig rig;
    const auto& mod = rig.add("one");
    std::string json = "{";
    for (int i = 0; i < 10; ++i) json += (i ? "," : "") + std::string("\"") + std::to_string(500 + i) + "\":\"x\"";   // ten ids the file does not have
    json += "}";
    write(mod.root / "text" / "us.json", json);
    const auto summary = rig.build();
    CHECK(summary.problems.at("one").size() == 1 && summary.problems.at("one")[0].find("10 ids") != std::string::npos && summary.problems.at("one")[0].find("500") != std::string::npos);
    CHECK(summary.problems.at("one")[0].size() < 200);   // not one line per id
    Rig loud;
    const auto& noisy = loud.add("noisy");
    std::string bad = "{\"!" + std::string(100, 'k') + "\":\"x\"";   // a key far too long to print whole (it sorts first: "!" comes before digits)
    for (int i = 0; i < 30; ++i) bad += ",\"" + std::to_string(i + 20) + "\":5";   // thirty values that are not text
    write(noisy.root / "text" / "us.json", bad + "}");
    const auto capped = loud.build();
    const auto& lines = capped.problems.at("noisy");
    CHECK(lines.size() == 21 && lines.back().find("more") != std::string::npos);   // twenty problems and a note that more exist
    CHECK(lines[0].size() < 160 && lines[0].find(std::string(33, 'k')) == std::string::npos);
}

static void an_earlier_mods_all_json_unknown_id_is_reported_too() {
    Rig rig;
    write(rig.add("a").root / "text" / "all.json", R"({"555":"a"})");
    write(rig.add("b").root / "text" / "all.json", R"({"555":"b"})");   // the same id: the second setter must not hide the first
    const auto build = rig.build();
    CHECK(build.problems.count("a") == 1 && build.problems.count("b") == 1);
    Rig partial;   // English is damaged: the languages that could be patched decide
    write(partial.add("one").root / "text" / "all.json", R"({"555":"x"})");
    partial.base = [](const std::string& lang) -> std::optional<TextBaseFile> {
        if (lang == "us") return TextBaseFile{make_txtres(sample()), ""};
        if (lang == "en") return TextBaseFile{std::vector<uint8_t>(50, 1), ""};
        return std::nullopt;
    };
    const auto result = partial.build();
    const auto& problems = result.problems.at("one");
    const auto has = [&](const char* word) { for (const auto& p : problems) if (p.find(word) != std::string::npos) return true; return false; };
    CHECK(has("555") && has("mes_en"));   // the unknown id (unknown in the one language that worked) and the damaged file
}

static void a_damaged_base_a_mod_ships_is_blamed_on_that_mod() {
    Rig rig;
    write(rig.add("words").root / "text" / "us.json", R"({"1":"x"})");
    rig.add("shipper");
    rig.base = [](const std::string& lang) -> std::optional<TextBaseFile> {
        if (lang == "us") return TextBaseFile{std::vector<uint8_t>(100, 7), "shipper"};
        return std::nullopt;
    };
    const auto build = rig.build();
    CHECK(build.problems.count("shipper") == 1 && build.problems.at("shipper")[0].find("mes_us") != std::string::npos);
    CHECK(build.problems.count("words") == 1);
}

static void too_many_entries_and_leading_zeros_and_line_endings() {
    Rig rig;
    const auto& mod = rig.add("one");
    std::string many = "{";
    for (int i = 0; i < 100001; ++i) many += (i ? ",\"" : "\"") + std::to_string(1000 + i) + "\":\"\"";
    write(mod.root / "text" / "en.json", many + "}");
    write(mod.root / "text" / "us.json", R"({"007":"zero","7":"Seven %s now","1":"a\r\nb"})");
    const auto build = rig.build();
    const auto& problems = build.problems.at("one");
    const auto has = [&](const char* file, const char* word) { for (const auto& p : problems) if (p.find(file) != std::string::npos && p.find(word) != std::string::npos) return true; return false; };
    CHECK(has("en.json", "100000") && has("us.json", "007"));
    const auto us = Rig::records(build.root, "us");
    CHECK(us[2].lines[0] == "Seven %s now" && (us[0].lines == std::vector<std::string>{"a", "b"}));   // no stray CR; "007" did not override "7"
    CHECK(!fs::exists(build.root / "Data" / "message" / "mes_en.txtres"));
}

static void two_mods_setting_one_id_are_a_conflict_and_long_lines_are_noted() {
    Rig rig;
    write(rig.add("modone").root / "text" / "us.json", R"({"12":"from one","1":"x"})");
    write(rig.add("modtwo").root / "text" / "us.json", R"({"12":"from two"})");
    const auto build = rig.build();
    CHECK(build.conflicts.size() == 1 && build.conflicts[0].find("12") != std::string::npos && build.conflicts[0].find("modone") != std::string::npos && build.conflicts[0].find("modtwo") != std::string::npos);
    CHECK(Rig::records(build.root, "us")[3].lines[0] == "from two");
    Rig same;   // all.json and us.json of one mod are not a conflict
    write(same.add("solo").root / "text" / "all.json", R"({"12":"x"})");
    write(same.mods[0].root / "text" / "us.json", R"({"12":"y"})");
    CHECK(same.build().conflicts.empty());
    Rig wide;   // the longest original line here is 8 bytes: a longer one is applied but noted
    write(wide.add("one").root / "text" / "us.json", R"({"1":"a line that is much longer than any original one"})");
    const auto noted = wide.build();
    CHECK(Rig::records(noted.root, "us")[0].lines[0].size() > 20);
    CHECK(noted.problems.at("one").size() == 1 && noted.problems.at("one")[0].find("longer") != std::string::npos);
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
    const auto none = build_text_mods(rig.cache, rig.active(), [](const std::string&) { return std::optional<TextBaseFile>(); });
    CHECK(none.root.empty() && none.problems.at("one").size() == 1 && none.problems.at("one")[0].find("no text file") != std::string::npos);
    const auto damaged = build_text_mods(rig.cache, rig.active(), [](const std::string&) { return std::optional<TextBaseFile>(TextBaseFile{std::vector<uint8_t>(100, 7), ""}); });
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
    a_damaged_cache_folder_is_rebuilt();
    placeholders_must_match_the_original();
    unknown_ids_are_summarised_keys_truncated_and_problems_capped();
    an_earlier_mods_all_json_unknown_id_is_reported_too();
    a_damaged_base_a_mod_ships_is_blamed_on_that_mod();
    too_many_entries_and_leading_zeros_and_line_endings();
    two_mods_setting_one_id_are_a_conflict_and_long_lines_are_noted();
    no_text_means_empty_root();
    missing_base_is_a_problem();
    all_json_unknown_id_is_reported_once_when_no_language_has_it();
    prune_keeps_only_the_given_key();
    std::puts("test_text_mods: ok");
    return 0;
}
