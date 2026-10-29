#include "check.h"
#include "wiimote_config.h"
using namespace uc;
template<class F> static void rejects(F f) {
    bool caught=false; try {f();} catch(const std::exception&) {caught=true;} CHECK(caught);
}
static WiimoteSettings from_text(const char* text) { return wiimote_from_config(parse_config(text)); }
static void defaults_without_the_object() {
    CHECK(from_text("{}")==WiimoteSettings{});
    const WiimoteSettings defaults;
    CHECK(defaults.enabled && defaults.smoothing==.35f && defaults.swing_threshold==1.5f && defaults.stick_dead_zone==.10f);
    CHECK(defaults.bindings==default_wii_bindings() && !validate_wiimote(defaults));
}
static void default_layout_follows_the_original() {
    const auto table=default_wii_bindings();
    const auto has=[&](Action action,uint16_t code) {
        for(const auto& b:table[size_t(action)]) if(b.kind==BindingKind::physical && b.input==PhysicalInput{PhysicalKind::wii_button,code}) return true;
        return false;
    };
    CHECK(has(Action::fire,7) && has(Action::interact,8) && has(Action::pause,10) && has(Action::knife,11));   // B, A, Home, nunchuk Z
    CHECK(has(Action::confirm,8) && has(Action::back,7));
    CHECK(has(Action::weapon_next,4) && has(Action::weapon_previous,9));   // plus, minus
}
static void roundtrip_keeps_every_field() {
    WiimoteSettings settings;
    settings.enabled=false; settings.smoothing=.6f; settings.swing_threshold=2.25f; settings.stick_dead_zone=.2f;
    settings.bindings[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::wii_button,5},{{PhysicalKind::wii_button,6}}}};
    settings.bindings[size_t(Action::reload)].clear();
    CHECK(!validate_wiimote(settings));
    auto document=parse_config(R"({"graphics":{"future":1}})");
    store_wiimote(document,settings);
    CHECK(document.properties["graphics"]["future"]==1);                    // the rest of the file is left alone
    CHECK(document.properties["wiimote"]["bindings"]["fire"][0]["kind"]=="wii_button");
    CHECK(wiimote_from_config(parse_config(serialize_config(document)))==settings);
}
static void partial_object_keeps_the_other_defaults() {
    const auto s=from_text(R"({"wiimote":{"smoothing":0.5}})");
    CHECK(s.smoothing==.5f && s.enabled && s.swing_threshold==1.5f && s.bindings==default_wii_bindings());
}
static void bad_objects_are_refused() {
    for(auto text:{R"({"wiimote":1})",R"({"wiimote":{"colour":1}})",R"({"wiimote":{"enabled":"yes"}})",R"({"wiimote":{"smoothing":0.95}})",R"({"wiimote":{"smoothing":-0.1}})",
        R"({"wiimote":{"swing_threshold":0.2}})",R"({"wiimote":{"swing_threshold":9}})",R"({"wiimote":{"stick_dead_zone":0.9}})",R"({"wiimote":{"smoothing":"high"}})",
        R"({"wiimote":{"bindings":{"fire":[{"kind":"key","code":9}]}}})",R"({"wiimote":{"bindings":{"fire":[{"kind":"wii_button","code":13}]}}})",
        R"({"wiimote":{"bindings":{"nothing":[]}}})"})
        rejects([&]{ from_text(text); });
    WiimoteSettings bad; bad.smoothing=2;
    ConfigDocument document; document.properties=nlohmann::ordered_json::object();
    rejects([&]{ store_wiimote(document,bad); });
    CHECK(!document.properties.contains("wiimote"));                        // a refused store leaves the document alone
}
int main() {
    defaults_without_the_object();
    default_layout_follows_the_original();
    roundtrip_keeps_every_field();
    partial_object_keeps_the_other_defaults();
    bad_objects_are_refused();
}
