#include "gamepad_config.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
using Json=nlohmann::ordered_json;
[[noreturn]] void invalid(const std::string& message) {throw std::invalid_argument("config: gamepad: "+message);}
void fields(const Json& value,std::initializer_list<std::string_view> allowed,const std::string& where) {
    if(!value.is_object())invalid(where+" must be an object");
    for(auto i=value.begin();i!=value.end();++i)
        if(std::find(allowed.begin(),allowed.end(),i.key())==allowed.end())invalid("unknown field "+where+i.key());
}
template<class T,size_t N> T choice(const Json& object,const char* key,T current,const std::array<const char*,N>& names) {
    if(!object.contains(key))return current;
    if(!object[key].is_string())invalid(std::string(key)+" must be a string");
    const auto text=object[key].get<std::string>();
    for(size_t i=0;i<N;++i)if(text==names[i])return T(i);
    invalid(std::string(key)+" has an unknown value");
}
constexpr std::array<const char*,3> kTypes{"auto","keyboard_mouse","gamepad"},kRoles{"none","left","right"},kGyroModes{"off","always","hold"};
constexpr std::array<const char*,5> kModels{"auto","xbox","playstation","nintendo","generic"};
float number(const Json& object,const char* key,float current) {
    if(!object.contains(key))return current;
    if(!object[key].is_number())invalid(std::string(key)+" must be a number");
    return float(object[key].get<double>());
}
bool flag(const Json& object,const char* key,bool current) {
    if(!object.contains(key))return current;
    if(!object[key].is_boolean())invalid(std::string(key)+" must be true or false");
    return object[key].get<bool>();
}
bool in(float value,float low,float high) {return std::isfinite(value) && value>=low && value<=high;}
bool pad_input(PhysicalInput input) {
    return (input.kind==PhysicalKind::pad_button && input.code<32) || (input.kind==PhysicalKind::pad_axis && input.code<16);
}
double rounded(float value) {return std::round(double(value)*1e6)/1e6;}   // 0.15f is written as 0.15, not 0.15000000596046448
}
std::optional<std::string> validate_gamepad(const GamepadSettings& s) {
    if(unsigned(s.type)>unsigned(ControlType::gamepad))return "unknown control type";
    if(unsigned(s.model)>unsigned(PadModel::generic))return "unknown model";
    if(unsigned(s.aim_stick)>unsigned(StickRole::right) || unsigned(s.camera_stick)>unsigned(StickRole::right))return "unknown stick role";
    if(s.aim_stick!=StickRole::none && s.aim_stick==s.camera_stick)return "aim_stick and camera_stick must differ";
    if(!in(s.aim.dead_zone,0,.5f))return "dead_zone must be 0..0.5";
    if(!in(s.aim.speed,.2f,4))return "aim_speed must be 0.2..4";
    if(!in(s.camera_dead_zone,0,.5f))return "camera_dead_zone must be 0..0.5";
    if(unsigned(s.gyro.mode)>unsigned(GyroMode::hold))return "unknown gyro mode";
    if(!in(s.gyro.sensitivity_x,.1f,10) || !in(s.gyro.sensitivity_y,.1f,10))return "gyro sensitivity must be 0.1..10";
    if(!in(s.gyro.dead_zone,0,.05f))return "gyro dead_zone must be 0..0.05";
    if(!pad_input(s.gyro_hold))return "gyro hold must be a gamepad button or axis";
    if(auto error=validate_pad_bindings(s.bindings))return *error;
    return {};
}
GamepadSettings gamepad_from_config(const ConfigDocument& document) {
    GamepadSettings s;
    const auto at=document.properties.find("gamepad");
    if(at==document.properties.end())return s;
    const Json& g=*at;
    try {
        fields(g,{"type","model","aim_stick","camera_stick","dead_zone","aim_speed","camera_dead_zone","gyro","bindings"},"");
        s.type=choice(g,"type",s.type,kTypes);
        s.model=choice(g,"model",s.model,kModels);
        s.aim_stick=choice(g,"aim_stick",s.aim_stick,kRoles);
        s.camera_stick=choice(g,"camera_stick",s.camera_stick,kRoles);
        s.aim.dead_zone=number(g,"dead_zone",s.aim.dead_zone);
        s.aim.speed=number(g,"aim_speed",s.aim.speed);
        s.camera_dead_zone=number(g,"camera_dead_zone",s.camera_dead_zone);
        if(g.contains("gyro")) {
            const Json& y=g["gyro"];
            fields(y,{"mode","sensitivity_x","sensitivity_y","dead_zone","invert_x","invert_y","hold"},"gyro.");
            s.gyro.mode=choice(y,"mode",s.gyro.mode,kGyroModes);
            s.gyro.sensitivity_x=number(y,"sensitivity_x",s.gyro.sensitivity_x);
            s.gyro.sensitivity_y=number(y,"sensitivity_y",s.gyro.sensitivity_y);
            s.gyro.dead_zone=number(y,"dead_zone",s.gyro.dead_zone);
            s.gyro.invert_x=flag(y,"invert_x",s.gyro.invert_x);
            s.gyro.invert_y=flag(y,"invert_y",s.gyro.invert_y);
            if(y.contains("hold"))s.gyro_hold=physical_input_from_json(y["hold"]);
        }
        if(g.contains("bindings"))s.bindings=bindings_from_json(g["bindings"],s.bindings);
    } catch(const Json::exception& error) {invalid(error.what());}
    if(auto error=validate_gamepad(s))invalid(*error);
    return s;
}
void store_gamepad(ConfigDocument& document,const GamepadSettings& s) {
    if(auto error=validate_gamepad(s))invalid(*error);
    Json g=Json::object();
    g["type"]=kTypes[size_t(s.type)]; g["model"]=kModels[size_t(s.model)];
    g["aim_stick"]=kRoles[size_t(s.aim_stick)]; g["camera_stick"]=kRoles[size_t(s.camera_stick)];
    g["dead_zone"]=rounded(s.aim.dead_zone); g["aim_speed"]=rounded(s.aim.speed); g["camera_dead_zone"]=rounded(s.camera_dead_zone);
    g["gyro"]={{"mode",kGyroModes[size_t(s.gyro.mode)]},{"sensitivity_x",rounded(s.gyro.sensitivity_x)},{"sensitivity_y",rounded(s.gyro.sensitivity_y)},
        {"dead_zone",rounded(s.gyro.dead_zone)},{"invert_x",s.gyro.invert_x},{"invert_y",s.gyro.invert_y},{"hold",physical_input_json(s.gyro_hold)}};
    g["bindings"]=bindings_to_json(s.bindings);
    document.properties["gamepad"]=std::move(g);
}
}
