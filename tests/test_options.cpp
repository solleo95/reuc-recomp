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
    auto input_probe=parse(std::array<std::string_view,1>{"--input-probe"});
    CHECK(std::holds_alternative<uc::Options>(input_probe));
    CHECK(std::get<uc::Options>(input_probe).input_probe && !std::get<uc::Options>(defaults).input_probe);
    auto unlock=parse(std::array<std::string_view,1>{"--unlock-60fps"});
    CHECK(std::get<uc::Options>(unlock).unlock_60fps);
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
}
