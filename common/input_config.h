#pragma once
#include "input_bindings.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
namespace uc {
inline constexpr std::array<std::string_view,size_t(Action::count)> action_names={
    "interact","fire","weapon_previous","weapon_next","reload","grenade","knife",
    "pause","confirm","back","release_cursor","screenshot","qte_shake",
    "camera_up","camera_down","camera_left","camera_right","menu","recenter"};
struct ConfigDocument {
    nlohmann::ordered_json properties=nlohmann::ordered_json::object();
    Bindings input=default_bindings();
    std::optional<std::string> original_bytes;
};
// The "bindings" object (action name -> array of bindings) applied onto `base`: the actions it does not name keep their entries. Does not validate the table
// (the caller applies the rule of its own table); throws std::invalid_argument("config: ...") for a malformed object.
Bindings bindings_from_json(const nlohmann::ordered_json& object,Bindings base);
nlohmann::ordered_json bindings_to_json(const Bindings&);   // every action, in action_names order
nlohmann::ordered_json physical_input_json(PhysicalInput);
PhysicalInput physical_input_from_json(const nlohmann::ordered_json&);   // {"kind","code"}, any physical kind; the caller validates it for its device
ConfigDocument parse_config(std::string_view);
ConfigDocument load_config(const std::filesystem::path&);
std::string serialize_config(const ConfigDocument&);
void save_config(const std::filesystem::path&,ConfigDocument&);
}
