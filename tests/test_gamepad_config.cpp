#include "check.h"
#include "gamepad_config.h"
using namespace uc;
using Json=nlohmann::ordered_json;
template<class F> static void rejects(F f) {
    bool caught=false; try {f();} catch(const std::exception&) {caught=true;} CHECK(caught);
}
static GamepadSettings from_text(const char* text) { return gamepad_from_config(parse_config(text)); }
static void defaults_without_the_object() {
    CHECK(from_text("{}")==GamepadSettings{});
    CHECK(from_text(R"({"input":{"version":1}})")==GamepadSettings{});
    const GamepadSettings defaults;
    CHECK(defaults.type==ControlType::automatic && defaults.model==PadModel::automatic);
    CHECK(defaults.aim_stick==StickRole::left && defaults.camera_stick==StickRole::right);
    CHECK(defaults.aim.dead_zone==.15f && defaults.camera_dead_zone==.10f && defaults.gyro.mode==GyroMode::off);
    CHECK((defaults.gyro_hold==PhysicalInput{PhysicalKind::pad_axis,9}) && defaults.bindings==default_pad_bindings());
    CHECK(!validate_gamepad(defaults));
}
static void roundtrip_keeps_every_field() {
    GamepadSettings settings;
    settings.type=ControlType::gamepad; settings.model=PadModel::playstation;
    settings.aim_stick=StickRole::right; settings.camera_stick=StickRole::left;
    settings.aim.dead_zone=.2f; settings.aim.speed=2.5f; settings.camera_dead_zone=.05f;
    settings.gyro.mode=GyroMode::hold; settings.gyro.sensitivity_x=1.5f; settings.gyro.sensitivity_y=.5f; settings.gyro.dead_zone=.01f;
    settings.gyro.invert_x=true; settings.gyro.invert_y=true; settings.gyro_hold={PhysicalKind::pad_button,9};
    settings.bindings[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::pad_button,10},{{PhysicalKind::pad_axis,9}}}};
    settings.bindings[size_t(Action::camera_up)]={{BindingKind::physical,{PhysicalKind::pad_button,11},{}}};
    settings.bindings[size_t(Action::reload)].clear();
    CHECK(!validate_gamepad(settings));
    auto document=parse_config(R"({"graphics":{"future":1}})");
    store_gamepad(document,settings);
    const auto text=serialize_config(document);
    const auto again=parse_config(text);
    CHECK(gamepad_from_config(again)==settings && again.properties["graphics"]["future"]==1);
    CHECK(again.input==default_bindings());   // the keyboard table is not touched by the gamepad object
    CHECK(Json::parse(text)["gamepad"]["bindings"].contains("recenter"));
}
static void a_partial_object_keeps_the_rest() {
    const auto settings=from_text(R"({"gamepad":{"model":"xbox","bindings":{"fire":[{"kind":"pad_button","code":10}]}}})");
    CHECK(settings.model==PadModel::xbox && settings.type==ControlType::automatic);
    CHECK(settings.bindings[size_t(Action::fire)].size()==1 && settings.bindings[size_t(Action::fire)][0].input.code==10);
    CHECK(settings.bindings[size_t(Action::interact)]==default_pad_bindings()[size_t(Action::interact)]);
}
static void bad_files_are_refused_not_fatal() {
    for(auto text:{
        R"({"gamepad":null})",R"({"gamepad":[]})",R"({"gamepad":{"future":1}})",R"({"gamepad":{"gyro":{"future":1}}})",
        R"({"gamepad":{"type":"mouse"}})",R"({"gamepad":{"type":3}})",R"({"gamepad":{"model":"sega"}})",
        R"({"gamepad":{"aim_stick":"left","camera_stick":"left"}})",R"({"gamepad":{"aim_stick":"up"}})",
        R"({"gamepad":{"dead_zone":0.6}})",R"({"gamepad":{"dead_zone":-0.1}})",R"({"gamepad":{"dead_zone":"big"}})",
        R"({"gamepad":{"aim_speed":0.1}})",R"({"gamepad":{"aim_speed":4.5}})",R"({"gamepad":{"camera_dead_zone":0.51}})",
        R"({"gamepad":{"gyro":{"mode":"sometimes"}}})",R"({"gamepad":{"gyro":{"sensitivity_x":0.05}}})",R"({"gamepad":{"gyro":{"sensitivity_y":11}}})",
        R"({"gamepad":{"gyro":{"dead_zone":0.06}}})",R"({"gamepad":{"gyro":{"invert_x":"yes"}}})",
        R"({"gamepad":{"gyro":{"hold":{"kind":"key","code":9}}}})",R"({"gamepad":{"gyro":{"hold":{"kind":"pad_axis","code":16}}}})",
        R"({"gamepad":{"bindings":{"unknown":[]}}})",R"({"gamepad":{"bindings":{"fire":[{"kind":"key","code":9}]}}})",
        R"({"gamepad":{"bindings":{"fire":[{"kind":"mouse","code":1}]}}})",R"({"gamepad":{"bindings":{"fire":[{"kind":"pad_button","code":32}]}}})",
        R"({"gamepad":{"bindings":{"fire":[{"kind":"pad_button","code":1},{"kind":"pad_button","code":1}]}}})",
        R"({"gamepad":{"bindings":{"fire":[{"kind":"pad_button","code":1,"extra":1}]}}})",R"({"gamepad":{"bindings":{"fire":null}}})"})
        rejects([&]{from_text(text);});
    GamepadSettings bad; bad.aim_stick=bad.camera_stick=StickRole::right; CHECK(validate_gamepad(bad));
    bad=GamepadSettings{}; bad.aim.speed=std::numeric_limits<float>::quiet_NaN(); CHECK(validate_gamepad(bad));
    bad=GamepadSettings{}; bad.bindings[size_t(Action::fire)].push_back({BindingKind::physical,{PhysicalKind::key,9},{}}); CHECK(validate_gamepad(bad));
    auto document=parse_config("{}"); rejects([&]{store_gamepad(document,bad);});
    CHECK(!document.properties.contains("gamepad"));   // a refused store leaves the document alone
}
int main() { defaults_without_the_object(); roundtrip_keeps_every_field(); a_partial_object_keeps_the_rest(); bad_files_are_refused_not_fatal(); }
