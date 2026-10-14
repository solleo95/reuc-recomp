#pragma once
#include "input_config.h"
#include <span>
#include <string_view>
namespace uc::launcher {
struct Language {std::string_view code,name;};
std::span<const Language> languages();
std::string resolve_language(std::string_view locale);
std::string system_language();
std::string config_language(const ConfigDocument&);
void store_language(ConfigDocument&,std::string_view);
std::span<const std::string_view> translation_keys();
std::string_view text(std::string_view language,std::string_view key);
}
