#include "check.h"
#include "manifest.h"
#include <nlohmann/json.hpp>
#include <string>

using namespace uc::mods;

static const char* kMinimal = R"({"id":"my-mod","name":"My Mod","version":"1.0.0","api":"1.0"})";

static ManifestResult with(const std::string& extra) {   // minimal manifest plus extra members
    std::string text = kMinimal;
    text.pop_back();
    return parse_manifest(text + "," + extra + "}");
}

static void valid_minimal() {
    const auto r = parse_manifest(kMinimal);
    CHECK(r.manifest && r.errors.empty());
    CHECK(r.manifest->id == "my-mod" && r.manifest->version == "1.0.0");
    CHECK(!r.manifest->touches_gameplay && r.manifest->settings.empty() && r.manifest->script.empty());
}

static void bad_id() {
    for (const char* id : {"Bad Id", "enabled", "allow_native", "native_hash", "order", "", "-lead"}) {
        const auto r = parse_manifest(std::string(R"({"id":")") + id + R"(","name":"n","version":"1","api":"1.0"})");
        CHECK(!r.manifest && !r.errors.empty());
    }
    const auto too_long = parse_manifest(std::string(R"({"id":")") + std::string(49, 'a') + R"(","name":"n","version":"1","api":"1.0"})");
    CHECK(!too_long.manifest);
    const auto longest = parse_manifest(std::string(R"({"id":")") + std::string(48, 'a') + R"(","name":"n","version":"1","api":"1.0"})");
    CHECK(longest.manifest);
}

static void upper_case_id() {   // an id written with capitals is the same mod as its lower-case form, also in depends
    const auto r = parse_manifest(R"({"id":"FFRU","name":"n","version":"1","api":"1.0","depends":[{"id":"Base-Mod"}]})");
    CHECK(r.manifest && r.manifest->id == "ffru" && r.manifest->depends.at(0).id == "base-mod");
    CHECK(!parse_manifest(R"({"id":"ENABLED","name":"n","version":"1","api":"1.0"})").manifest);   // reserved names stay reserved in any case
}

static void invalid_json() {
    const auto r = parse_manifest("{ not json");
    CHECK(!r.manifest && r.errors.size() == 1);
    CHECK(!parse_manifest("[]").manifest);
}

static void api_format() {
    const auto r = parse_manifest(kMinimal);
    CHECK(r.manifest->api.major == 1 && r.manifest->api.minor == 0);
    const auto newer = parse_manifest(R"({"id":"a","name":"n","version":"1","api":"2.13"})");
    CHECK(newer.manifest && newer.manifest->api.major == 2 && newer.manifest->api.minor == 13);
    for (const char* api : {"1", "x", "1.", "1.x", "-1.0", "1.0.0"}) {
        CHECK(!parse_manifest(std::string(R"({"id":"a","name":"n","version":"1","api":")") + api + R"("})").manifest);
    }
}

static void settings_types() {
    const auto r = with(R"("settings":[
        {"key":"on","type":"bool","default":true,"label":{"en":"On"}},
        {"key":"count","type":"int","default":3,"min":0,"max":10,"step":1,"label":{"en":"Count","ru":"N"},"tooltip":{"en":"tip"}},
        {"key":"scale","type":"number","default":1.5,"min":0.5,"max":4,"label":{"en":"Scale"},"restart_required":true},
        {"key":"mode","type":"choice","default":"b","choices":["a","b"],"label":{"en":"Mode"}},
        {"key":"name","type":"text","default":"x","label":{"en":"Name"}}])");
    CHECK(r.manifest && r.errors.empty());
    const auto& s = r.manifest->settings;
    CHECK(s.size() == 5);
    CHECK(s[0].type == SettingType::boolean && s[0].default_value == true);
    CHECK(s[1].type == SettingType::integer && *s[1].max == 10 && s[1].label.at("ru") == "N" && s[1].tooltip.at("en") == "tip");
    CHECK(s[2].type == SettingType::number && s[2].restart_required);
    CHECK(s[3].type == SettingType::choice && s[3].choices.size() == 2);
    CHECK(s[4].type == SettingType::text);

    const auto bad_default = with(R"("settings":[{"key":"on","type":"bool","default":"yes","label":{"en":"On"}}])");
    CHECK(!bad_default.manifest && bad_default.errors.size() == 1 && bad_default.errors[0].find("on") != std::string::npos);
    CHECK(!with(R"("settings":[{"key":"n","type":"int","default":1,"min":5,"max":2,"label":{"en":"N"}}])").manifest);
    CHECK(!with(R"("settings":[{"key":"m","type":"choice","default":"z","choices":["a"],"label":{"en":"M"}}])").manifest);
    CHECK(!with(R"("settings":[{"key":"n","type":"bool","default":true}])").manifest);   // no label.en
    CHECK(!with(R"("settings":[{"key":"n","type":"bool","default":true,"label":{"en":"A"}},{"key":"n","type":"bool","default":true,"label":{"en":"B"}}])").manifest);
    CHECK(!with(R"("settings":[{"key":"n","type":"weird","default":1,"label":{"en":"A"}}])").manifest);
}

static void script_and_native_paths() {
    CHECK(with(R"("script":"scripts/main.lua")").manifest);
    for (const char* path : {"../x.lua", "scripts/../../x.lua", "/x.lua", "C:/x.lua", "C:\\x.lua", "scripts\\main.lua", ""}) {
        const auto r = with(std::string(R"("script":)") + nlohmann::json(path).dump());   // dump() escapes the backslashes: the path itself is what is rejected
        CHECK(!r.manifest && r.errors.size() == 1 && r.errors[0].find("script") != std::string::npos);
    }
    CHECK(with(R"("native":"native/mod.dll")").manifest);
    CHECK(!with(R"("native":"../mod.dll")").manifest);
    CHECK(!with(R"("native":"native/mod.exe")").manifest);
}

static void script_extension() {
    for (const char* ext : {"lua", "js", "mjs"}) CHECK(with(std::string(R"("script":"s/main.)") + ext + "\"").manifest);
    for (const char* ext : {"py", "txt", ""}) CHECK(!with(std::string(R"("script":"s/main.)") + ext + "\"").manifest);
}

static void depends_and_names() {
    const auto r = with(R"("depends":[{"id":"core","min_version":"1.2"},{"id":"other"}],"game":["RBUE08"],"touches_gameplay":true,"names":{"ru":"Мой мод"})");
    CHECK(r.manifest && r.manifest->depends.size() == 2 && r.manifest->depends[0].min_version == "1.2" && r.manifest->depends[1].min_version.empty());
    CHECK(r.manifest->touches_gameplay && r.manifest->game.size() == 1 && r.manifest->names.at("ru") == "Мой мод");
    CHECK(!with(R"("depends":[{"id":"Bad Id"}])").manifest);
}

static void audio_loops_parse() {
    const auto r = with(R"("audio":{"00_Title":{"loop_start":1000,"loop_end":50000},"VO_01":{"loop_start":0,"loop_end":2147483648}})");
    CHECK(r.manifest && r.errors.empty() && r.manifest->audio.size() == 2);
    const auto title = r.manifest->audio.find("00_title");   // stems are kept lower-case
    CHECK(title != r.manifest->audio.end() && title->second.loop_start == 1000u && title->second.loop_end == 50000u);
    CHECK(r.manifest->audio.at("vo_01").loop_end == 2147483648u);
}

static void audio_loop_start_not_before_end_is_an_error() {
    CHECK(!with(R"("audio":{"a":{"loop_start":5,"loop_end":5}})").manifest);
    CHECK(!with(R"("audio":{"a":{"loop_start":9,"loop_end":5}})").manifest);
    CHECK(!with(R"("audio":{"a":{"loop_start":0,"loop_end":2147483649}})").manifest);
    CHECK(!with(R"("audio":{"a":{"loop_start":-1,"loop_end":5}})").manifest);
    CHECK(!with(R"("audio":{"a":{"loop_start":1.5,"loop_end":5}})").manifest);
    CHECK(!with(R"("audio":{"a":{"loop_start":1}})").manifest);       // both keys are needed
    CHECK(!with(R"("audio":{"a":5})").manifest && !with(R"("audio":[])").manifest);
    const auto r = with(R"("audio":{"a":{"loop_start":9,"loop_end":5}})");
    CHECK(r.errors.size() == 1 && r.errors[0].find("audio.a") != std::string::npos);   // the error names the key
}

static void audio_stem_with_a_path_is_an_error() {
    for (const char* stem : {"../x", "a/b", "a b", "", "a.ogg"})
        CHECK(!with(std::string(R"("audio":{")") + stem + R"(":{"loop_start":0,"loop_end":5}})").manifest);
}

static void audio_key_is_optional() {
    const auto r = parse_manifest(kMinimal);
    CHECK(r.manifest && r.manifest->audio.empty());
}

int main() {
    valid_minimal(); bad_id(); upper_case_id(); invalid_json(); api_format(); settings_types(); script_and_native_paths(); script_extension(); depends_and_names();
    audio_loops_parse(); audio_loop_start_not_before_end_is_an_error(); audio_stem_with_a_path_is_an_error(); audio_key_is_optional();
    std::puts("ok");
}
