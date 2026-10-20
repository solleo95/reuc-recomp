#include "check.h"
#include "options_config.h"
int main() {
    auto document=uc::parse_config(R"({"graphics":{"render_scale":2,"window_mode":"borderless","future_dlss":7},"skip_intro":true,"game":"S:/Games/Test/game"})");
    auto options=uc::options_from_config(document);
    CHECK(options.prompt_style==uc::PromptStyle::monochrome);
    CHECK(options.bloom);   // absent: the game's own look
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
    CHECK(!options.skip_tutorials);   // absent: tutorials stay
    options.skip_tutorials=true;uc::store_options(document,options);
    CHECK(document.properties["skip_tutorials"]==true && uc::options_from_config(uc::parse_config(uc::serialize_config(document))).skip_tutorials);
    options.skip_tutorials=false;uc::store_options(document,options);
    options.bloom=false;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["bloom"]==false && !uc::options_from_config(uc::parse_config(uc::serialize_config(document))).bloom);
    options.bloom=true;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["bloom"]==true && uc::options_from_config(uc::parse_config(uc::serialize_config(document))).bloom);
    CHECK(!uc::options_from_config(document).fps_limit);   // absent: the previous behaviour
    options.fps_limit=165;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["fps_limit"]==165 && uc::options_from_config(uc::parse_config(uc::serialize_config(document))).fps_limit==165u);
    options.fps_limit=0;uc::store_options(document,options);   // unlimited is stored, not dropped
    CHECK(document.properties["graphics"]["fps_limit"]==0 && uc::options_from_config(uc::parse_config(uc::serialize_config(document))).fps_limit==0u);
    options.fps_limit.reset();uc::store_options(document,options);
    CHECK(!document.properties["graphics"].contains("fps_limit"));
    CHECK(options.upscaler==uc::UpscalerKind::off && !options.frame_generation);   // absent: no upscaling
    {
        auto c=uc::options_from_config(uc::parse_config(R"({"graphics":{"upscaler":"fsr","upscale_mode":"balanced","sharpening":true,"sharpness":0.01,"frame_generation":true,"frame_generation_multiplier":2}})"));
        CHECK(c.upscaler==uc::UpscalerKind::fsr && c.upscale_mode==uc::UpscaleMode::balanced && c.sharpening && c.sharpness==0.01 && c.frame_generation && c.frame_generation_multiplier==2);
        options.upscaler=uc::UpscalerKind::dlss;options.upscale_mode=uc::UpscaleMode::ultra_performance;options.sharpening=true;options.sharpness=0.42;
        options.frame_generation=true;options.frame_generation_multiplier=4;uc::store_options(document,options);
        CHECK(document.properties["graphics"]["upscaler"]=="dlss" && document.properties["graphics"]["upscale_mode"]=="ultra_performance");
        auto back=uc::options_from_config(uc::parse_config(uc::serialize_config(document)));
        CHECK(back.upscaler==uc::UpscalerKind::dlss && back.upscale_mode==uc::UpscaleMode::ultra_performance && back.sharpening && back.sharpness==0.42 && back.frame_generation && back.frame_generation_multiplier==4);
        options.upscaler=uc::UpscalerKind::blit;   // test-only kind is never written
        bool rejected=false;try{uc::store_options(document,options);}catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);
        options.upscaler=uc::UpscalerKind::off;options.upscale_mode=uc::UpscaleMode::quality;options.sharpening=false;options.sharpness=0.5;
        options.frame_generation=false;options.frame_generation_multiplier=2;uc::store_options(document,options);
    }
    const auto before=uc::serialize_config(document);
    options.prompt_style=uc::PromptStyle(99);
    bool invalid_style=false;try{uc::store_options(document,options);}catch(const std::invalid_argument&){invalid_style=true;}
    CHECK(invalid_style && uc::serialize_config(document)==before);
    CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"upscaler":"smaa"}})")).upscaler==uc::UpscalerKind::smaa);
    {   // graphics.anisotropy
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"anisotropy":4}})")).effects.anisotropy==4);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects.anisotropy==16);
        CHECK(!uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects.per_pixel_lighting);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"per_pixel_lighting":true}})")).effects.per_pixel_lighting);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"ambient_occlusion":"high"}})")).effects.ambient_occlusion==uc::EffectLevel::high);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects.ambient_occlusion==uc::EffectLevel::off);
        auto level_doc=uc::parse_config(R"({"game":"S:/Games/Test/game"})");auto level_options=uc::options_from_config(level_doc);level_options.effects.ambient_occlusion=uc::EffectLevel::medium;uc::store_options(level_doc,level_options);
        CHECK(uc::options_from_config(level_doc).effects.ambient_occlusion==uc::EffectLevel::medium && uc::serialize_config(level_doc).find("\"ambient_occlusion\": \"medium\"")!=std::string::npos);
        for(const char* text:{R"({"game":"S:/Games/Test/game","graphics":{"ambient_occlusion":"extreme"}})",R"({"game":"S:/Games/Test/game","graphics":{"ambient_occlusion":2}})"}) {
            bool bad_level=false;try {uc::options_from_config(uc::parse_config(text));}catch(const std::invalid_argument&){bad_level=true;}
            CHECK(bad_level);
        }
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"contact_shadows":true}})")).effects.contact_shadows);
        CHECK(!uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects.contact_shadows);
        bool contact_not_boolean=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"contact_shadows":"on"}})"));}catch(const std::invalid_argument&){contact_not_boolean=true;}
        CHECK(contact_not_boolean);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"motion_blur":"ultra"}})")).effects.motion_blur==uc::EffectLevel::ultra);
        CHECK(uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects.motion_blur==uc::EffectLevel::off);
        auto blur_doc=uc::parse_config(R"({"game":"S:/Games/Test/game"})");auto blur_options=uc::options_from_config(blur_doc);blur_options.effects.motion_blur=uc::EffectLevel::low;uc::store_options(blur_doc,blur_options);
        CHECK(uc::options_from_config(blur_doc).effects.motion_blur==uc::EffectLevel::low && uc::serialize_config(blur_doc).find("\"motion_blur\": \"low\"")!=std::string::npos);
        bool bad_blur=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"motion_blur":"extreme"}})"));}catch(const std::invalid_argument&){bad_blur=true;}
        CHECK(bad_blur);
        {   // ray tracing keys
            const auto defaults=uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects;
            CHECK(!defaults.ray_tracing && defaults.rt_reflections==uc::EffectLevel::off && defaults.rt_lighting==uc::EffectLevel::off && defaults.rt_shadows==uc::EffectLevel::off
                && defaults.rt_local_shadows==uc::EffectLevel::off && defaults.rt_ambient_occlusion==uc::EffectLevel::off);
            auto rt_doc=uc::parse_config(R"({"game":"S:/Games/Test/game"})");auto rt_options=uc::options_from_config(rt_doc);
            CHECK(!defaults.ray_reconstruction && !defaults.path_tracing);   // off by default
            auto& e=rt_options.effects;e.ray_reconstruction=true;e.path_tracing=true;e.ray_tracing=true;e.rt_reflections=uc::EffectLevel::low;e.rt_lighting=uc::EffectLevel::medium;e.rt_shadows=uc::EffectLevel::high;e.rt_local_shadows=uc::EffectLevel::ultra;e.rt_ambient_occlusion=uc::EffectLevel::low;
            uc::store_options(rt_doc,rt_options);
            CHECK(uc::options_from_config(rt_doc).effects==e && uc::serialize_config(rt_doc).find("\"rt_local_shadows\": \"ultra\"")!=std::string::npos && uc::serialize_config(rt_doc).find("\"ray_reconstruction\": true")!=std::string::npos && uc::serialize_config(rt_doc).find("\"path_tracing\": true")!=std::string::npos);
            for(const char* text:{R"({"game":"S:/Games/Test/game","graphics":{"rt_shadows":"extreme"}})",R"({"game":"S:/Games/Test/game","graphics":{"rt_reflections":1}})",R"({"game":"S:/Games/Test/game","graphics":{"ray_tracing":"on"}})"}) {
                bool bad=false;try {uc::options_from_config(uc::parse_config(text));}catch(const std::invalid_argument&){bad=true;}
                CHECK(bad);
            }
        }
        bool not_boolean=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"per_pixel_lighting":1}})"));}catch(const std::invalid_argument&){not_boolean=true;}
        CHECK(not_boolean);
        auto stored=uc::parse_config(R"({"game":"S:/Games/Test/game"})");auto fresh=uc::options_from_config(stored);fresh.effects.anisotropy=2;uc::store_options(stored,fresh);
        CHECK(uc::options_from_config(stored).effects.anisotropy==2 && uc::serialize_config(stored).find("\"anisotropy\": 2")!=std::string::npos);
        bool caught=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"anisotropy":5}})"));}catch(const std::invalid_argument& error){caught=std::string(error.what()).find("graphics.anisotropy must be 1, 2, 4, 8 or 16")!=std::string::npos;}
        CHECK(caught);
    }
    {   // a number that wraps to a valid level in 32 bits is still invalid
        bool wrapped=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"anisotropy":4294967312}})"));}catch(const std::invalid_argument&){wrapped=true;}
        CHECK(wrapped);
    }
    for(auto text:{R"({"skip_intro":1})",R"({"game":null})",R"({"game":"a\u0000b"})",R"({"graphics":{"render_scale":8}})",
        R"({"graphics":{"render_scale":-1}})",R"({"graphics":{"width":-1}})",R"({"graphics":{"window_mode":"bad"}})",
        R"({"graphics":{"bloom":"off"}})",R"({"graphics":{"bloom":0}})",R"({"graphics":{"bloom":null}})",R"({"graphics":{"prompt_style":"bad"}})",R"({"graphics":{"prompt_style":2}})",R"({"graphics":{"prompt_style":null}})",R"({"graphics":{"fps_limit":-1}})",R"({"graphics":{"fps_limit":10}})",
        R"({"graphics":{"fps_limit":2000}})",R"({"graphics":{"fps_limit":"fast"}})",R"({"graphics":{"fps_limit":144.5}})",
        R"({"graphics":{"upscaler":"blit"}})",R"({"graphics":{"upscaler":1}})",R"({"graphics":{"upscale_mode":"dlaa"}})",R"({"graphics":{"sharpness":0}})",
        R"({"graphics":{"sharpness":1.5}})",R"({"graphics":{"sharpening":1}})",R"({"graphics":{"frame_generation":"on"}})",
        R"({"graphics":{"frame_generation_multiplier":1}})",R"({"graphics":{"frame_generation_multiplier":7}})",R"({"graphics":{"frame_generation_multiplier":2.5}})"}) {
        bool caught=false;try {uc::options_from_config(uc::parse_config(text));}catch(const std::invalid_argument&){caught=true;}CHECK(caught);
    }
    return 0;
}
