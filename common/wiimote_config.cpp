#include "wiimote_config.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
using Json=nlohmann::ordered_json;
[[noreturn]] void invalid(const std::string& message) {throw std::invalid_argument("config: wiimote: "+message);}
float number(const Json& object,const char* key,float current) {
    if(!object.contains(key))return current;
    if(!object[key].is_number())invalid(std::string(key)+" must be a number");
    return float(object[key].get<double>());
}
bool in(float value,float low,float high) {return std::isfinite(value) && value>=low && value<=high;}
double rounded(float value) {return std::round(double(value)*1e6)/1e6;}   // 0.35f is written as 0.35, not 0.3499999940395355
}
std::optional<std::string> validate_wiimote(const WiimoteSettings& s) {
    if(!in(s.smoothing,0,.9f))return "smoothing must be 0..0.9";
    if(!in(s.swing_threshold,.5f,4))return "swing_threshold must be 0.5..4";
    if(!in(s.stick_dead_zone,0,.5f))return "stick_dead_zone must be 0..0.5";
    if(auto error=validate_wii_bindings(s.bindings))return *error;
    return {};
}
WiimoteSettings wiimote_from_config(const ConfigDocument& document) {
    WiimoteSettings s;
    const auto at=document.properties.find("wiimote");
    if(at==document.properties.end())return s;
    const Json& w=*at;
    try {
        if(!w.is_object())invalid("wiimote must be an object");
        for(auto i=w.begin();i!=w.end();++i) {
            static constexpr std::string_view allowed[]={"enabled","smoothing","swing_threshold","stick_dead_zone","bindings"};
            if(std::find(std::begin(allowed),std::end(allowed),i.key())==std::end(allowed))invalid("unknown field "+i.key());
        }
        if(w.contains("enabled")) {
            if(!w["enabled"].is_boolean())invalid("enabled must be true or false");
            s.enabled=w["enabled"].get<bool>();
        }
        s.smoothing=number(w,"smoothing",s.smoothing);
        s.swing_threshold=number(w,"swing_threshold",s.swing_threshold);
        s.stick_dead_zone=number(w,"stick_dead_zone",s.stick_dead_zone);
        if(w.contains("bindings"))s.bindings=bindings_from_json(w["bindings"],s.bindings);
    } catch(const Json::exception& error) {invalid(error.what());}
    if(auto error=validate_wiimote(s))invalid(*error);
    return s;
}
void store_wiimote(ConfigDocument& document,const WiimoteSettings& s) {
    if(auto error=validate_wiimote(s))invalid(*error);
    Json w=Json::object();
    w["enabled"]=s.enabled; w["smoothing"]=rounded(s.smoothing); w["swing_threshold"]=rounded(s.swing_threshold); w["stick_dead_zone"]=rounded(s.stick_dead_zone);
    w["bindings"]=bindings_to_json(s.bindings);
    document.properties["wiimote"]=std::move(w);
}
}
