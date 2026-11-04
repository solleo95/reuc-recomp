#include "coop_config.h"
#include <algorithm>
#include <stdexcept>
namespace uc {
namespace {
using Json=nlohmann::ordered_json;
[[noreturn]] void invalid(std::string message) {
    constexpr std::string_view prefix="config: ";
    if(message.starts_with(prefix)) message.erase(0,prefix.size());   // the inner parsers name themselves ("config: gamepad: ..."): one "config:" is enough
    throw std::invalid_argument("config: player2: "+message);
}
}
std::optional<std::string> validate_coop(const CoopSettings& s) {
    if(auto error=validate_bindings(s.bindings)) return error;
    return validate_gamepad(s.gamepad);
}
CoopSettings coop_from_config(const ConfigDocument& document) {
    CoopSettings s;
    const auto at=document.properties.find("player2");
    if(at==document.properties.end()) return s;
    const Json& object=*at;
    if(!object.is_object()) invalid("must be an object");
    try {
        for(auto i=object.begin();i!=object.end();++i)
            if(i.key()!="bindings" && i.key()!="gamepad") invalid("unknown field "+i.key());
        if(object.contains("bindings")) s.bindings=bindings_from_json(object["bindings"],s.bindings);
        if(object.contains("gamepad")) s.gamepad=gamepad_from_json(object["gamepad"]);
    } catch(const Json::exception& error) { invalid(error.what()); }
    catch(const std::invalid_argument& error) { invalid(error.what()); }
    if(auto error=validate_coop(s)) invalid(*error);
    return s;
}
void store_coop(ConfigDocument& document,const CoopSettings& s) {
    if(auto error=validate_coop(s)) invalid(*error);
    document.properties["player2"]={{"bindings",bindings_to_json(s.bindings)},{"gamepad",gamepad_to_json(s.gamepad)}};
}
}
