#include "check.h"
#include "coop_config.h"
using namespace uc;
using Json=nlohmann::ordered_json;
template<class F> static void rejects(F f) {
    bool caught=false; try {f();} catch(const std::invalid_argument&) {caught=true;} CHECK(caught);
}
static CoopSettings from_text(const char* text) { return coop_from_config(parse_config(text)); }
static void defaults_without_the_object() {
    CHECK(from_text("{}")==CoopSettings{});
    const CoopSettings defaults;
    CHECK(defaults.bindings==default_bindings());          // the second player's mouse starts as the first player's
    CHECK(defaults.gamepad==GamepadSettings{});            // and so does the gamepad
    CHECK(!validate_coop(defaults));
}
static void roundtrip_keeps_both_tables_and_leaves_player_one_alone() {
    CoopSettings settings;
    settings.bindings[size_t(Action::reload)]={{BindingKind::physical,{PhysicalKind::mouse,4},{}}};
    settings.gamepad.bindings[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::pad_button,10},{}}};
    settings.gamepad.aim.speed=2.5f;
    CHECK(!validate_coop(settings));
    auto document=parse_config(R"({"gamepad":{"model":"xbox"},"graphics":{"future":1}})");
    store_coop(document,settings);
    const auto again=parse_config(serialize_config(document));
    CHECK(coop_from_config(again)==settings);
    CHECK(gamepad_from_config(again).model==PadModel::xbox && gamepad_from_config(again).bindings==default_pad_bindings());   // player one's "gamepad" object is untouched
    CHECK(again.properties["graphics"]["future"]==1 && again.properties.contains("player2"));
}
static void a_partial_object_keeps_the_rest() {
    const auto settings=from_text(R"({"player2":{"gamepad":{"model":"playstation"}}})");
    CHECK(settings.gamepad.model==PadModel::playstation && settings.bindings==default_bindings());
}
static void bad_objects_are_refused() {
    rejects([]{from_text(R"({"player2":5})");});
    rejects([]{from_text(R"({"player2":{"unknown":1}})");});
    rejects([]{from_text(R"({"player2":{"gamepad":{"model":"toaster"}}})");});
    rejects([]{from_text(R"({"player2":{"bindings":{"fire":[{"kind":"pad_button","code":0}]}}})");});   // the keyboard-and-mouse table holds no gamepad input
}
static void an_invalid_set_is_not_stored() {
    CoopSettings bad;
    bad.bindings[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::pad_button,0},{}}};
    CHECK(validate_coop(bad));
    auto document=parse_config(R"({"graphics":{"future":1}})");
    const auto before=document.properties.dump();
    rejects([&]{store_coop(document,bad);});
    CHECK(document.properties.dump()==before);
}
int main() {
    defaults_without_the_object(); roundtrip_keeps_both_tables_and_leaves_player_one_alone(); a_partial_object_keeps_the_rest();
    bad_objects_are_refused(); an_invalid_set_is_not_stored();
}
