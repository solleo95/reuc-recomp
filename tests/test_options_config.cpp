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
    CHECK(uc::options_from_config(uc::parse_config(R"({"graphics":{}})")).crosshair_scale==100);   // absent: the game's size
    options.crosshair_scale=0;uc::store_options(document,options);
    CHECK(document.properties["graphics"]["crosshair_scale"]==0 && uc::options_from_config(uc::parse_config(uc::serialize_config(document))).crosshair_scale==0);
    options.crosshair_scale=100;uc::store_options(document,options);
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
        {   // ray tracing keys: the player's option is path tracing, "ray_tracing" (a switch) and "rt_quality" (low, medium, high, ultra); the per-effect keys of the old ray tracing are dropped
            const auto defaults=uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).effects;
            CHECK(!defaults.path_tracing && defaults.rt_quality==uc::EffectLevel::high && !defaults.ray_tracing && defaults.rt_reflections==uc::EffectLevel::off && !defaults.ray_reconstruction);
            auto rt_doc=uc::parse_config(R"({"game":"S:/Games/Test/game","graphics":{"rt_shadows":"high","ray_reconstruction":true,"path_tracing":true,"rt_lighting":"ultra"}})");   // a config written by an older build
            const auto migrated=uc::options_from_config(rt_doc);
            CHECK(!migrated.effects.path_tracing && migrated.effects.rt_shadows==uc::EffectLevel::off && !migrated.effects.ray_reconstruction);   // its old keys mean nothing now
            auto rt_options=migrated;
            rt_options.effects.path_tracing=true; rt_options.effects.rt_quality=uc::EffectLevel::ultra;
            uc::store_options(rt_doc,rt_options);
            const auto written=uc::serialize_config(rt_doc);
            CHECK(uc::options_from_config(rt_doc).effects==rt_options.effects && written.find("\"ray_tracing\": true")!=std::string::npos && written.find("\"rt_quality\": \"ultra\"")!=std::string::npos);
            for(const char* old_key:{"rt_shadows","rt_lighting","ray_reconstruction","path_tracing","rt_local_shadows","rt_reflections","rt_ambient_occlusion"}) CHECK(written.find(std::string("\"")+old_key+"\"")==std::string::npos);   // and are removed from the file
            for(const char* text:{R"({"game":"S:/Games/Test/game","graphics":{"rt_quality":"extreme"}})",R"({"game":"S:/Games/Test/game","graphics":{"rt_quality":"off"}})",R"({"game":"S:/Games/Test/game","graphics":{"rt_quality":1}})",R"({"game":"S:/Games/Test/game","graphics":{"ray_tracing":"on"}})"}) {
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
    {   // DLSS 5: the "dlss5" object holds every setting of the table; absent keys keep the defaults, a wrong type or range is refused, unknown keys of a newer file survive a save
        auto document=uc::parse_config(R"({"game":"S:/Games/Test/game","dlss5":{"hook":"present","preset":"h","passes":3,"intensity":0.5,"resolution_scale":0.75,"character_mask":true,"future":9}})");
        auto options=uc::options_from_config(document);
        CHECK(options.dlss5.enabled() && options.dlss5.hook==uc::Dlss5Hook::present && options.dlss5.preset==uc::Dlss5Preset::h && options.dlss5.passes==3 && options.dlss5.intensity==0.5 && options.dlss5.resolution_scale==0.75 && options.dlss5.character_mask);
        CHECK(options.dlss5.tone==1 && options.dlss5.model==uc::Dlss5Model::a && options.dlss5.encoding==uc::Dlss5Encoding::automatic);
        CHECK(!uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game"})")).dlss5.enabled());
        options.dlss5.exposure_value=uc::Dlss5ExposureValue::inverse; options.dlss5.exposure=2; options.dlss5.encoding=uc::Dlss5Encoding::hdr10; options.dlss5.diffuse_white_nits=1000; options.dlss5.scaling_domain=uc::Dlss5Domain::signed_pq;
        uc::store_options(document,options);
        const auto written=uc::serialize_config(document);
        CHECK(uc::options_from_config(uc::parse_config(written)).dlss5==options.dlss5);
        CHECK(written.find("\"encoding\": \"hdr10\"")!=std::string::npos && written.find("\"passes\": 3")!=std::string::npos && written.find("\"hook\": \"present\"")!=std::string::npos && written.find("\"character_mask\": true")!=std::string::npos);
        CHECK(document.properties["dlss5"]["future"]==9);
        for(const char* body:{R"("hook":1)",R"("hook":"hud")",R"("character_mask":1)",R"("character_mask":"true")",R"("preset":1)",R"("preset":"a")",R"("passes":0)",R"("passes":5)",R"("passes":2.5)",R"("passes":"2")",R"("intensity":2.5)",R"("intensity":"1")",
            R"("resolution_scale":0.4)",R"("encoding":null)",R"("exposure":100)",R"("diffuse_white_nits":10)",R"("block_hudless":null)"}) {
            bool bad=false;try {uc::options_from_config(uc::parse_config(std::string(R"({"game":"S:/Games/Test/game","dlss5":{)")+body+"}}"));}catch(const std::invalid_argument&){bad=true;}
            CHECK(bad);
        }
        {   // a section of the first DLSS 5 build ("enabled", "stage", other values) loads as defaults and is replaced by the next save
            auto old=uc::parse_config(R"({"game":"S:/Games/Test/game","dlss5":{"enabled":true,"preset":"b","scaling_domain":"encoded","stage":"upscaled"}})");
            auto loaded=uc::options_from_config(old);
            CHECK(loaded.dlss5==uc::Dlss5Settings{});
            uc::store_options(old,loaded);
            const auto text=uc::serialize_config(old);
            CHECK(text.find("\"enabled\"")==std::string::npos && text.find("\"stage\"")==std::string::npos && text.find("\"hook\": \"off\"")!=std::string::npos);
            CHECK(uc::options_from_config(uc::parse_config(text)).dlss5==uc::Dlss5Settings{});
        }
        bool not_object=false;try {uc::options_from_config(uc::parse_config(R"({"game":"S:/Games/Test/game","dlss5":true})"));}catch(const std::invalid_argument&){not_object=true;}
        CHECK(not_object);
        // an out-of-range value in the options is refused by the writer, and the document keeps its old content
        auto bad_options=options; bad_options.dlss5.passes=9;
        const auto before=uc::serialize_config(document); bool refused=false;
        try {uc::store_options(document,bad_options);}catch(const std::invalid_argument&){refused=true;}
        CHECK(refused && uc::serialize_config(document)==before);
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
        R"({"graphics":{"frame_generation_multiplier":1}})",R"({"graphics":{"frame_generation_multiplier":7}})",R"({"graphics":{"frame_generation_multiplier":2.5}})",
        R"({"graphics":{"crosshair_scale":101}})",R"({"graphics":{"crosshair_scale":-1}})",R"({"graphics":{"crosshair_scale":50.5}})"}) {
        bool caught=false;try {uc::options_from_config(uc::parse_config(text));}catch(const std::invalid_argument&){caught=true;}CHECK(caught);
    }
    return 0;
}
