#include "check.h"
#include "localization.h"
#include "dlss5_settings.h"
#include <set>
#include <string>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
int main() {
    using namespace uc::launcher;
    CHECK(languages().size()==13);
    CHECK(resolve_language("ru-RU")=="ru");
    CHECK(resolve_language("EN_us")=="en");
    CHECK(resolve_language("pt-PT")=="pt-BR");
    CHECK(resolve_language("zh-CN")=="zh-Hans");
    CHECK(resolve_language("ja-JP")=="ja");
    CHECK(resolve_language("ar-SA")=="en");
    ULONG count=0,length=4096;std::wstring preferred(length,L'\0');
    CHECK(GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,&count,preferred.data(),&length));
    std::wstring_view first(preferred.c_str());
    CHECK(system_language()==resolve_language(std::string(first.begin(),first.end())));
    std::printf("system launcher language: %s\n",system_language().c_str());
    auto document=uc::parse_config(R"({"launcher":{"future":42},"graphics":{"future":7}})");
    CHECK(config_language(document)=="system");
    for(const auto& language:languages()) {
        store_language(document,language.code);
        CHECK(config_language(uc::parse_config(uc::serialize_config(document)))==language.code);
        for(auto key:translation_keys())CHECK(!text(language.code,key).empty());
    }
    // Every DLSS 5 control has its label (and every named choice its text) in all 13 languages: the tab composes the keys from the settings table
    for(const auto& language:languages()) {
        CHECK(!text(language.code,"dlss5").empty() && !text(language.code,"dlss5_hint").empty() && !text(language.code,"dlss5_status_available").empty() && !text(language.code,"dlss5_status_active").empty() && !text(language.code,"dlss5_status_unavailable").empty());
        for(const auto& key:{"dlss5_section_scaling","dlss5_section_advanced","dlss5_section_debug","dlss5_open_debug","dlss5_reset","dlss5_off","dlss5_on"}) CHECK(!text(language.code,key).empty());
        for(const auto& field:uc::dlss5_fields()) {
            CHECK(!text(language.code,"dlss5_"+std::string(field.key)).empty());
            if(field.kind!=uc::Dlss5Kind::boolean) for(const auto& choice:field.choices) if(choice.label.empty()) CHECK(!text(language.code,"dlss5_"+std::string(choice.name)).empty());
        }
        for(const auto& choice:uc::dlss5_field("hook")->choices) CHECK(!text(language.code,"dlss5_hook_desc_"+std::string(choice.name)).empty());
    }
    CHECK(text("ru","dlss5_hook")=="Метод подключения" && text("en","dlss5_hook_desc_off")=="Off: Disabled");
    // The disc image unpacking texts exist in all 13 languages (an empty or missing one would throw or show nothing).
    for(const auto& language:languages())
        for(const auto& key:{"image_browse","image_selected","unpack_title","unpack_working","unpack_wit_missing","unpack_wrong_image","unpack_inside_game","unpack_no_space","unpack_failed","unpack_close"})
            CHECK(text(language.code,key).size()>3);
    CHECK(text("en","image_browse")=="Select a disc image…" && text("ru","unpack_title")=="Распаковка игры");
    CHECK(text("en","play")=="Singleplayer");
    CHECK(text("ru","play")=="Одиночная игра");
    CHECK(text("ja","play")=="シングルプレイ");
    // Checked = bloom on (a positive option with a strength slider), in the launcher and the in-game menu.
    CHECK(text("en","bloom")=="Bloom (glow around lights)" && text("en","bloom_strength")=="Bloom strength");
    CHECK(text("ru","bloom")=="Bloom (свечение вокруг источников света)" && text("ru","bloom_strength")=="Сила bloom");
    CHECK(text("ru","menu_main")=="Основное" && text("ru","menu_controls")=="Управление" && text("ru","other")=="Прочее");
    CHECK(text("en","launcher_only")=="Changed in the launcher" && text("ru","launcher_only")=="Меняется в лаунчере");
    CHECK(document.properties["launcher"]["future"]==42);
    CHECK(document.properties["graphics"]["future"]==7);
    store_language(document,"system");
    CHECK(config_language(document)=="system");
    auto before=uc::serialize_config(document);
    bool rejected=false;try{store_language(document,"bad");}catch(const std::invalid_argument&){rejected=true;}
    CHECK(rejected && before==uc::serialize_config(document));
    for(auto invalid:{R"({"launcher":5})",R"({"launcher":{"language":null}})",R"({"launcher":{"language":"bad"}})"}) {
        rejected=false;try{config_language(uc::parse_config(invalid));}catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);
    }
    return 0;
}
