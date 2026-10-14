#include "localization.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc::launcher {
namespace {
constexpr Language supported[]={
    {"en","English"},{"ru","Русский"},{"de","Deutsch"},{"fr","Français"},
    {"es","Español"},{"it","Italiano"},{"pt-BR","Português (Brasil)"},
    {"pl","Polski"},{"uk","Українська"},{"tr","Türkçe"},
    {"ja","日本語"},{"ko","한국어"},{"zh-Hans","简体中文"}};
struct Entry {std::string_view key;std::array<std::string_view,13> value;};
constexpr Entry catalog[]={
#include "translations.inc"
};
bool supported_code(std::string_view code) {
    return code=="system" || std::ranges::any_of(supported,[&](auto item){return item.code==code;});
}
}
std::span<const Language> languages(){return supported;}
std::string resolve_language(std::string_view locale) {
    std::string tag(locale);for(auto& c:tag)c=c=='_' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
    auto primary=std::string_view(tag).substr(0,tag.find('-'));
    if(primary=="pt")return "pt-BR";
    if(primary=="zh")return "zh-Hans";
    for(auto language:supported)if(language.code==primary)return std::string(language.code);
    return "en";
}
std::string system_language() {
    ULONG count=0,size=0;
    if(!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,&count,nullptr,&size) || !size)return "en";
    std::wstring buffer(size,L'\0');
    if(!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,&count,buffer.data(),&size))return "en";
    const std::wstring_view first(buffer.c_str());
    return resolve_language(std::string(first.begin(),first.end()));
}
std::string config_language(const ConfigDocument& document) {
    const auto& root=document.properties;
    if(!root.contains("launcher"))return "system";
    const auto& settings=root["launcher"];
    if(!settings.is_object())throw std::invalid_argument("config: launcher must be an object");
    if(!settings.contains("language"))return "system";
    if(!settings["language"].is_string())throw std::invalid_argument("config: launcher.language must be a string");
    auto language=settings["language"].get<std::string>();
    if(!supported_code(language))throw std::invalid_argument("config: unsupported launcher language");
    return language;
}
void store_language(ConfigDocument& document,std::string_view language) {
    if(!supported_code(language))throw std::invalid_argument("config: unsupported launcher language");
    auto next=document;
    if(next.properties.contains("launcher") && !next.properties["launcher"].is_object())throw std::invalid_argument("config: launcher must be an object");
    next.properties["launcher"]["language"]=language;
    document.properties=std::move(next.properties);
}
std::span<const std::string_view> translation_keys() {
    static const auto keys=[] {std::array<std::string_view,std::size(catalog)> result{};
        for(size_t i=0;i<result.size();++i)result[i]=catalog[i].key;return result;}();
    return keys;
}
std::string_view text(std::string_view language,std::string_view key) {
    size_t index=0;for(size_t i=0;i<std::size(supported);++i)if(supported[i].code==language)index=i;
    for(const auto& entry:catalog)if(entry.key==key)return entry.value[index];
    throw std::logic_error("unknown launcher translation: "+std::string(key));
}
}
