#include "check.h"
#include "options_config.h"
int main() {
    auto document=uc::parse_config(R"({"graphics":{"render_scale":2,"window_mode":"borderless","future_dlss":7},"skip_intro":true,"game":"S:/Games/Test/game"})");
    auto options=uc::options_from_config(document);
    CHECK(options.prompt_style==uc::PromptStyle::monochrome);
    CHECK(options.render_scale==2 && options.skip_intro && options.window_mode==uc::WindowMode::borderless);
    auto override=uc::parse_options(std::array<std::string_view,2>{"--render-scale","1.5"},options);
    CHECK(std::get<uc::Options>(override).render_scale==1.5 && std::get<uc::Options>(override).skip_intro);
    options.render_scale=3; uc::store_options(document,options);
    CHECK(document.properties["graphics"]["future_dlss"]==7);
    CHECK(uc::options_from_config(uc::parse_config(uc::serialize_config(document))).render_scale==3);
    options.prompt_style=uc::PromptStyle::sculpted;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["prompt_style"]=="sculpted");
    CHECK(uc::options_from_config(uc::parse_config(uc::serialize_config(document))).prompt_style==uc::PromptStyle::sculpted);
    options.prompt_style=uc::PromptStyle::monochrome;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["prompt_style"]=="monochrome");
    const auto before=uc::serialize_config(document);
    options.prompt_style=uc::PromptStyle(99);
    bool invalid_style=false;try{uc::store_options(document,options);}catch(const std::invalid_argument&){invalid_style=true;}
    CHECK(invalid_style && uc::serialize_config(document)==before);
    for(auto text:{R"({"skip_intro":1})",R"({"game":null})",R"({"game":"a\u0000b"})",R"({"graphics":{"render_scale":8}})",
        R"({"graphics":{"render_scale":-1}})",R"({"graphics":{"width":-1}})",R"({"graphics":{"window_mode":"bad"}})",
        R"({"graphics":{"prompt_style":"bad"}})",R"({"graphics":{"prompt_style":2}})",R"({"graphics":{"prompt_style":null}})"}) {
        bool caught=false;try {uc::options_from_config(uc::parse_config(text));}catch(const std::invalid_argument&){caught=true;}CHECK(caught);
    }
    return 0;
}
