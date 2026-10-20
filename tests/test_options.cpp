#include "check.h"
#include "options.h"
#include "test_control.h"
#include <array>

template<size_t N> static auto parse(const std::array<std::string_view, N>& args) { return uc::parse_options(args); }
int main() {
    auto defaults = uc::parse_options({});
    CHECK(std::get<uc::Options>(defaults).width == 0 && std::get<uc::Options>(defaults).height==0 && !std::get<uc::Options>(defaults).headless);
    CHECK(!std::get<uc::Options>(defaults).skip_intro);
    CHECK(std::get<uc::Options>(defaults).render_scale==0);
    CHECK(std::get<uc::Options>(defaults).window_mode==uc::WindowMode::windowed);
    for(auto name:{"windowed","borderless","exclusive"}) {
        auto mode=parse(std::array<std::string_view,2>{"--window-mode",name});
        CHECK(std::holds_alternative<uc::Options>(mode));
        CHECK(std::get<uc::Options>(mode).window_mode==(std::string_view(name)=="windowed" ? uc::WindowMode::windowed : std::string_view(name)=="borderless" ? uc::WindowMode::borderless : uc::WindowMode::exclusive));
    }
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--window-mode","invalid"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,1>{"--window-mode"})));
    auto native_scale=parse(std::array<std::string_view,2>{"--render-scale","6"});
    CHECK(std::get<uc::Options>(native_scale).render_scale==6);
    auto fractional_scale=parse(std::array<std::string_view,2>{"--render-scale","1.5"});
    CHECK(std::get<uc::Options>(fractional_scale).render_scale==1.5);
    for(auto invalid:{"0","7","-1","nan","inf","0.49","auto"}) CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--render-scale",invalid})));
    auto skip=parse(std::array<std::string_view,1>{"--skip-intro"});
    CHECK(std::get<uc::Options>(skip).skip_intro);
    auto profile=parse(std::array<std::string_view,1>{"--profile"});
    CHECK(std::holds_alternative<uc::Options>(profile) && std::get<uc::Options>(profile).profile);
    CHECK(!std::get<uc::Options>(profile).capture_scene_passes);
    auto scene_capture=parse(std::array<std::string_view,1>{"--capture-scene-passes"});
    CHECK(std::get<uc::Options>(scene_capture).capture_scene_passes && !std::get<uc::Options>(scene_capture).profile);
    CHECK(!std::get<uc::Options>(defaults).unlock_60fps);
    auto gaps=parse(std::array<std::string_view,2>{"--gx-gaps","--audio-audit"});
    CHECK(std::get<uc::Options>(gaps).gx_gaps && std::get<uc::Options>(gaps).audio_audit);
    CHECK(!std::get<uc::Options>(defaults).gx_gaps && !std::get<uc::Options>(defaults).audio_audit);
    auto input_probe=parse(std::array<std::string_view,1>{"--input-probe"});
    CHECK(std::holds_alternative<uc::Options>(input_probe));
    CHECK(std::get<uc::Options>(input_probe).input_probe && !std::get<uc::Options>(defaults).input_probe);
    auto unlock=parse(std::array<std::string_view,1>{"--unlock-60fps"});
    CHECK(std::get<uc::Options>(unlock).unlock_60fps);
    CHECK(std::get<uc::Options>(defaults).game_hz==0 && std::get<uc::Options>(parse(std::array<std::string_view,2>{"--game-hz","120"})).game_hz==120);
    for (auto invalid : {"60","481","0","abc"}) CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--game-hz",invalid})));
    CHECK(!std::get<uc::Options>(defaults).fps_limit && std::get<uc::Options>(parse(std::array<std::string_view,2>{"--fps-limit","144"})).fps_limit==144u);
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--fps-limit","0"})).fps_limit==0u);   // 0 = unlimited
    for (auto invalid : {"19","1001","abc","-5"}) CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--fps-limit",invalid})));
    auto result = parse(std::array<std::string_view, 9>{"--width", "3840", "--height", "2160", "--seconds", "20", "--headless", "--validation", "--gx-audit"});
    auto& options = std::get<uc::Options>(result);
    CHECK(options.width == 3840 && options.height == 2160 && options.seconds == 20 && options.headless && options.validation && options.gx_audit);
    for (auto invalid : {"0", "-1", "4294967296", "NaN", "720junk"})
        CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view, 2>{"--width", invalid})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view, 1>{"--game"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view, 1>{"--seconds"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view, 1>{"--unknown"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view, 2>{"--seconds", "-20"})));
    auto path = parse(std::array<std::string_view, 2>{"--game", "S:/A B/game"});
    CHECK(std::get<uc::Options>(path).game == "S:/A B/game");
    auto config=parse(std::array<std::string_view,2>{"--config","S:/A B/config.json"});
    CHECK(std::get<uc::Options>(config).config=="S:/A B/config.json");
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,1>{"--config"})));
    auto unlimited = parse(std::array<std::string_view, 2>{"--seconds", "0"});
    CHECK(std::get<uc::Options>(unlimited).seconds == 0);
    auto controlled=parse(std::array<std::string_view,4>{"--test-control","control.txt","--save-root","test-saves"});
    CHECK(std::get<uc::Options>(controlled).test_control=="control.txt" && std::get<uc::Options>(controlled).save_root=="test-saves");
    // Test server: name, speed and capture directory; speed needs a server.
    CHECK(std::get<uc::Options>(defaults).test_server.empty() && std::get<uc::Options>(defaults).speed==1 && std::get<uc::Options>(defaults).capture_dir.empty());
    auto server=parse(std::array<std::string_view,7>{"--test-server","run_1-a","--speed","4","--capture-dir","S:/A B/shots","--skip-intro"});
    CHECK(std::holds_alternative<uc::Options>(server));
    CHECK(std::get<uc::Options>(server).test_server=="run_1-a" && std::get<uc::Options>(server).speed==4 && std::get<uc::Options>(server).capture_dir=="S:/A B/shots");
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,4>{"--test-server","a","--speed","32"})).speed==32);
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,4>{"--test-server","a","--speed","33"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,4>{"--test-server","a","--speed","0"})));
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--speed","2"})));
    CHECK(!std::get<uc::Options>(defaults).god);
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,3>{"--test-server","a","--god"})).god);
    CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,1>{"--god"})));   // a cheat needs the test server
    CHECK(!std::get<uc::Options>(defaults).no_vulkan_layers);   // layers (OBS, RTSS, overlays) stay on by default
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,1>{"--no-vulkan-layers"})).no_vulkan_layers);
    CHECK(!std::get<uc::Options>(defaults).skip_tutorials);
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,3>{"--test-server","a","--skip-tutorials"})).skip_tutorials);
    CHECK(std::get<uc::Options>(parse(std::array<std::string_view,1>{"--skip-tutorials"})).skip_tutorials);   // a player option too, not only for the test server
    for(auto bad:{"bad/name","","a b","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","\\\\x"}) CHECK(std::holds_alternative<std::string>(parse(std::array<std::string_view,2>{"--test-server",bad})));
    auto command=uc::parse_test_command("1 -0.5 0.25 2048 1");
    CHECK(command && command->sequence==1 && command->held==2048 && command->capture);
    auto mouse=uc::parse_test_command("v2 2 mouse 3 1 0 0 0");
    CHECK(mouse && mouse->sequence==2 && mouse->kind==uc::TestKind::mouse && mouse->code==3 && mouse->value==1);
    CHECK(uc::parse_test_command("v2 3 key 26 0 0 0 1"));
    CHECK(uc::parse_test_command("v2 4 wheel 2 3 0 0 0"));
    CHECK(uc::parse_test_command("v2 5 motion 0 0 -3 4 0"));
    CHECK(uc::parse_test_command("v2 6 focus 0 0 0 0 0"));
    for(auto invalid:{"v2 0 key 26 1 0 0 0","v2 -1 key 26 1 0 0 0","v2 1 key -1 1 0 0 0",
        "v2 1 key 512 1 0 0 0","v2 1 key 26 2 0 0 0","v2 1 key 26 1 1 0 0","v2 1 mouse 0 1 0 0 0",
        "v2 1 mouse 256 1 0 0 0","v2 1 wheel 3 1 0 0 0","v2 1 wheel 1 -1 0 0 0","v2 1 wheel 1 257 0 0 0",
        "v2 1 motion 1 0 0 0 0","v2 1 motion 0 1 0 0 0","v2 1 motion 0 0 nan 0 0","v2 1 focus 0 2 0 0 0",
        "v2 1 focus 1 1 0 0 0","v2 1 focus 0 1 1 0 0","v2 1 unknown 1 1 0 0 0","v2 1 key 26 1 0 0 2",
        "v2 1 key 26 1 0 0 0 junk","v2 1 wheel 1 1.5 0 0 0"}) CHECK(!uc::parse_test_command(invalid));
    CHECK(!uc::parse_test_command(std::string(257,' ')));
    for(auto invalid:{"1 2 0 0 1","1 nan 0 0 1","1 0 0 1 1","1 0 0 0 2","1 0 0 0 1 junk","-1 0 0 0 1"," -1 0 0 0 1"}) CHECK(!uc::parse_test_command(invalid));
    {   // upscaling, sharpening and frame generation
        const auto& d=std::get<uc::Options>(defaults);
        CHECK(d.upscaler==uc::UpscalerKind::off && d.upscale_mode==uc::UpscaleMode::quality && !d.sharpening && d.sharpness==0.5 && !d.frame_generation && d.frame_generation_multiplier==2);
        auto o=std::get<uc::Options>(parse(std::array<std::string_view,8>{"--upscaler","dlss","--upscale-mode","ultra_performance","--sharpness","0.35","--frame-generation","3"}));
        CHECK(o.upscaler==uc::UpscalerKind::dlss && o.upscale_mode==uc::UpscaleMode::ultra_performance);
        CHECK(o.sharpening && o.sharpness==0.35 && o.frame_generation && o.frame_generation_multiplier==3);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--upscaler","blit"})).upscaler==uc::UpscalerKind::blit);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--upscaler","fsr"})).upscaler==uc::UpscalerKind::fsr);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--upscaler","smaa"})).upscaler==uc::UpscalerKind::smaa);
        CHECK(uc::upscaler_name(uc::UpscalerKind::smaa)=="smaa" && !uc::uses_jitter(uc::UpscalerKind::smaa) && uc::uses_jitter(uc::UpscalerKind::dlss) && uc::uses_jitter(uc::UpscalerKind::fsr));
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--upscale-mode","native"})).upscale_mode==uc::UpscaleMode::native);
        CHECK(!d.features && std::get<uc::Options>(parse(std::array<std::string_view,1>{"--features"})).features);   // launcher's support query
        auto off=std::get<uc::Options>(parse(std::array<std::string_view,5>{"--sharpness","1","--no-sharpening","--frame-generation","0"}));
        CHECK(!off.sharpening && off.sharpness==1 && !off.frame_generation);
        for(auto bad:{std::array<std::string_view,2>{"--sharpness","0"},{"--sharpness","1.01"},{"--sharpness","nan"},{"--frame-generation","1"},{"--frame-generation","7"},
                      {"--upscale-mode","dlaa"},{"--upscaler","xess"}}) CHECK(std::holds_alternative<std::string>(parse(bad)));
    }
    {   // graphics effects: anisotropic filtering
        CHECK(std::get<uc::Options>(defaults).effects.anisotropy==16);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--anisotropy","8"})).effects.anisotropy==8);
        const auto bad=parse(std::array<std::string_view,2>{"--anisotropy","3"});
        CHECK(std::holds_alternative<std::string>(bad) && std::get<std::string>(bad)=="invalid value for --anisotropy");
        CHECK(std::get<uc::Options>(defaults).effects.ambient_occlusion==uc::EffectLevel::off);
        for(const auto& [name,level]:{std::pair{"off",uc::EffectLevel::off},{"low",uc::EffectLevel::low},{"medium",uc::EffectLevel::medium},{"high",uc::EffectLevel::high},{"ultra",uc::EffectLevel::ultra}}) {
            CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--ambient-occlusion",name})).effects.ambient_occlusion==level);
            CHECK(uc::effect_level_name(level)==name && uc::effect_level_from_name(name)==level);
        }
        const auto bad_level=parse(std::array<std::string_view,2>{"--ambient-occlusion","extreme"});
        CHECK(std::holds_alternative<std::string>(bad_level) && std::get<std::string>(bad_level)=="invalid value for --ambient-occlusion");
        CHECK(!uc::effect_level_from_name("High") && !uc::effect_level_from_name(""));
        CHECK(std::get<uc::Options>(defaults).effects.motion_blur==uc::EffectLevel::off);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--motion-blur","medium"})).effects.motion_blur==uc::EffectLevel::medium);
        const auto bad_blur=parse(std::array<std::string_view,2>{"--motion-blur","extreme"});
        CHECK(std::holds_alternative<std::string>(bad_blur) && std::get<std::string>(bad_blur)=="invalid value for --motion-blur");
        const auto missing_blur=parse(std::array<std::string_view,1>{"--motion-blur"});
        CHECK(std::holds_alternative<std::string>(missing_blur) && std::get<std::string>(missing_blur)=="missing value for --motion-blur");
        CHECK(!std::get<uc::Options>(defaults).effects.ray_reconstruction && std::get<uc::Options>(parse(std::array<std::string_view,1>{"--ray-reconstruction"})).effects.ray_reconstruction);   // off by default, on by its flag
        CHECK(!std::get<uc::Options>(defaults).effects.ray_tracing && std::get<uc::Options>(defaults).effects.rt_shadows==uc::EffectLevel::off);
        const auto rt=std::get<uc::Options>(parse(std::array<std::string_view,3>{"--ray-tracing","--rt-shadows","high"}));
        CHECK(rt.effects.ray_tracing && rt.effects.rt_shadows==uc::EffectLevel::high);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--rt-reflections","low"})).effects.rt_reflections==uc::EffectLevel::low);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--rt-lighting","medium"})).effects.rt_lighting==uc::EffectLevel::medium);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--rt-local-shadows","ultra"})).effects.rt_local_shadows==uc::EffectLevel::ultra);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,2>{"--rt-ambient-occlusion","low"})).effects.rt_ambient_occlusion==uc::EffectLevel::low);
        const auto bad_rt=parse(std::array<std::string_view,2>{"--rt-shadows","bogus"});
        CHECK(std::holds_alternative<std::string>(bad_rt) && std::get<std::string>(bad_rt)=="invalid value for --rt-shadows");
        const auto missing_rt=parse(std::array<std::string_view,1>{"--rt-lighting"});
        CHECK(std::holds_alternative<std::string>(missing_rt) && std::get<std::string>(missing_rt)=="missing value for --rt-lighting");
        CHECK(!std::get<uc::Options>(defaults).effects.contact_shadows);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,1>{"--contact-shadows"})).effects.contact_shadows);
        CHECK(!std::get<uc::Options>(defaults).effects.per_pixel_lighting);
        CHECK(std::get<uc::Options>(parse(std::array<std::string_view,1>{"--per-pixel-lighting"})).effects.per_pixel_lighting);
        CHECK(uc::valid_anisotropy(1) && uc::valid_anisotropy(16) && !uc::valid_anisotropy(0) && !uc::valid_anisotropy(32));
        CHECK(uc::clamp_anisotropy(16,true,8.f)==8 && uc::clamp_anisotropy(16,false,16.f)==1 && uc::clamp_anisotropy(4,true,16.f)==4 && uc::clamp_anisotropy(16,true,0.5f)==1);
    }
}
