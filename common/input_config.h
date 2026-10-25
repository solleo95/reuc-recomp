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
    "camera_up","camera_down","camera_left","camera_right"};
struct ConfigDocument {
    nlohmann::ordered_json properties=nlohmann::ordered_json::object();
    Bindings input=default_bindings();
    std::optional<std::string> original_bytes;
};
ConfigDocument parse_config(std::string_view);
ConfigDocument load_config(const std::filesystem::path&);
std::string serialize_config(const ConfigDocument&);
void save_config(const std::filesystem::path&,ConfigDocument&);
}
