#include "check.h"
#include "localization.h"
#include <set>
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
    CHECK(text("ru","play")=="ИГРАТЬ");
    CHECK(text("ja","play")=="プレイ");
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
