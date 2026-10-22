#include "check.h"
#include "settings.h"

using namespace uc::mods;
using nlohmann::json;

static std::vector<SettingDecl> decls() {
    const auto parsed = parse_manifest(R"({"id":"m","name":"m","version":"1","api":"1.0","settings":[
        {"key":"on","type":"bool","default":true,"label":{"en":"On"}},
        {"key":"count","type":"int","default":4,"min":0,"max":100,"step":5,"label":{"en":"Count"}},
        {"key":"scale","type":"number","default":1.5,"min":0.5,"max":4,"label":{"en":"Scale"}},
        {"key":"mode","type":"choice","default":"b","choices":["a","b","c"],"label":{"en":"Mode"}},
        {"key":"name","type":"text","default":"x","label":{"en":"Name"}}]})");
    CHECK(parsed.manifest);
    return parsed.manifest->settings;
}

static void defaults_when_empty() {
    ModSettings s(decls(), json::object());
    CHECK(s.get("on") == true && s.get("count") == 4 && s.get("scale") == 1.5 && s.get("mode") == "b" && s.get("name") == "x");
    CHECK(s.warnings().empty() && !s.dirty());
    ModSettings from_null(decls(), json());   // no stored section at all
    CHECK(from_null.get("on") == true && from_null.warnings().empty());
}

static void stored_values_used() {
    ModSettings s(decls(), json{{"on", false}, {"count", 50}, {"scale", 2.0}, {"mode", "c"}, {"name", "hello"}});
    CHECK(s.get("on") == false && s.get("count") == 50 && s.get("scale") == 2.0 && s.get("mode") == "c" && s.get("name") == "hello");
    CHECK(s.warnings().empty());
}

static void wrong_type_falls_back() {
    ModSettings s(decls(), json{{"on", "yes"}});
    CHECK(s.get("on") == true && s.warnings().size() == 1 && s.warnings()[0].find("on") != std::string::npos);
    ModSettings text(decls(), json{{"count", "many"}, {"scale", true}, {"name", 5}});
    CHECK(text.get("count") == 4 && text.get("scale") == 1.5 && text.get("name") == "x" && text.warnings().size() == 3);
}

static void clamp_on_load() {
    ModSettings s(decls(), json{{"count", 500}, {"scale", -3.0}});
    CHECK(s.get("count") == 100 && s.get("scale") == 0.5);
    CHECK(s.warnings().size() == 2);   // one line per corrected value
}

static void int_snaps_to_step() {
    ModSettings s(decls(), json::object());
    CHECK(s.set("count", 47) && s.get("count") == 45);
    CHECK(s.set("count", 48) && s.get("count") == 50);
    CHECK(s.set("count", 2.0) && s.get("count") == 0);
    CHECK(s.get("count").is_number_integer());   // stays an integer, not 50.0
}

static void choice_must_be_listed() {
    ModSettings s(decls(), json{{"mode", "zzz"}});
    CHECK(s.get("mode") == "b" && s.warnings().size() == 1);
    CHECK(!s.set("mode", "zzz") && s.get("mode") == "b");
    CHECK(s.set("mode", "a") && s.get("mode") == "a");
}

static void set_validates_and_marks_dirty() {
    ModSettings s(decls(), json::object());
    CHECK(!s.dirty());
    CHECK(!s.set("nothere", 1) && !s.dirty());
    CHECK(!s.set("on", 1) && !s.dirty());   // wrong type: unchanged
    CHECK(s.set("on", true) && !s.dirty());   // same value: not a change
    CHECK(s.set("on", false) && s.dirty() && s.get("on") == false);
    s.clear_dirty();
    CHECK(!s.dirty());
    CHECK(s.set("scale", 99.0) && s.get("scale") == 4.0);   // clamped to max
}

static void take_changed_lists_each_changed_key_once() {
    ModSettings s(decls(), json::object());
    CHECK(s.take_changed().empty());
    s.set("on", true); s.set("count", 5); s.set("on", false); s.set("on", true); s.set("scale", 1.5);   // "on" and "scale" equal their defaults at first: only values that differ count
    CHECK((s.take_changed() == std::vector<std::string>{"count", "on"}) && s.take_changed().empty());
    s.set("mode", "zzz");   // refused: not a change either
    CHECK(s.take_changed().empty() && s.dirty());
}

static void to_json_lists_declared_keys() {
    ModSettings s(decls(), json{{"on", false}, {"ghost", 1}});
    const auto out = s.to_json();
    CHECK(out.size() == 5 && out["on"] == false && out["count"] == 4 && !out.contains("ghost"));   // unknown stored keys are not carried over
}

static void text_length_cap() {
    ModSettings s(decls(), json::object());
    CHECK(!s.set("name", std::string(1025, 'a')) && s.get("name") == "x");
    CHECK(s.set("name", std::string(1024, 'a')));
    ModSettings stored(decls(), json{{"name", std::string(2000, 'b')}});
    CHECK(stored.get("name") == "x" && stored.warnings().size() == 1);
}

static void unknown_key_is_null() {
    ModSettings s(decls(), json::object());
    CHECK(s.get("nothere").is_null());
}

// An integer setting without a range must not take a number that does not fit a 64-bit integer (llround is undefined for it).
static void huge_integer_is_rejected() {
    const auto parsed = parse_manifest(R"({"id":"m","name":"m","version":"1","api":"1.0","settings":[{"key":"n","type":"int","default":3,"label":{"en":"N"}}]})");
    CHECK(parsed.manifest);
    ModSettings s(parsed.manifest->settings, json{{"n", 1e300}});
    CHECK(s.get("n") == 3 && s.warnings().size() == 1);
    CHECK(!s.set("n", 1e19) && s.get("n") == 3);
    CHECK(s.set("n", 4e15) && s.get("n") == 4000000000000000ll);
}

int main() {
    huge_integer_is_rejected();
    defaults_when_empty(); stored_values_used(); wrong_type_falls_back(); clamp_on_load(); int_snaps_to_step(); choice_must_be_listed();
    set_validates_and_marks_dirty(); take_changed_lists_each_changed_key_once(); to_json_lists_declared_keys(); text_length_cap(); unknown_key_is_null();
    std::puts("ok");
}
