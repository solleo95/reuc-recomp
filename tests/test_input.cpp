#include "check.h"
#include "native_input.h"
#include "ppc_fixture.h"
#include "wide_view.h"
#include <cmath>
#include <limits>
#include <chrono>
#include <fstream>
#include <thread>
#include "input_bindings.h"

static uint64_t action_bit(uc::Action action) { return uint64_t(1)<<unsigned(action); }
static void host_test_commands_are_atomic_and_acknowledged() {
    uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
    CHECK(input.apply_test_event(1,{uc::HostEventKind::mouse,3,1,0,0},1000,500));
    input.consume(); CHECK(input.last_consumed_sequence()==0);
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
    CHECK(input.last_consumed_sequence()==1);
    CHECK(!input.apply_test_event(1,{uc::HostEventKind::mouse,3,0,0,0},1000,500));
    CHECK(input.consume_actions().held==action_bit(uc::Action::fire));
    CHECK(!input.apply_test_event(2,{uc::HostEventKind::key,512,1,0,0},1000,500));
    CHECK(!input.apply_test_event(2,{uc::HostEventKind::wheel,1,1,1,0},1000,500));
    CHECK(input.apply_test_event(2,{uc::HostEventKind::motion,0,0,100,50},1000,500));
    const auto sample=input.consume(); CHECK(sample.x==.2f && sample.y==.2f);
    input.consume_actions(); CHECK(input.last_consumed_sequence()==2);
    CHECK(input.apply_test_event(3,{uc::HostEventKind::focus,0,0,0,0},1000,500));
    CHECK(input.consume_actions().held==0 && !input.consume().valid && input.last_consumed_sequence()==3);
    CHECK(!input.apply_test_event(4,{uc::HostEventKind::mouse,3,1,0,0},1000,500));
    CHECK(input.apply_test_event(4,{uc::HostEventKind::mouse,3,0,0,0},1000,500));
    CHECK(input.consume_actions().held==0 && input.last_consumed_sequence()==4);
    CHECK(input.apply_test_event(5,{uc::HostEventKind::focus,0,1,0,0},1000,500)); input.consume_actions();
    CHECK(input.apply_test_event(6,{uc::HostEventKind::mouse,3,1,0,0},1000,500));
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
    CHECK(input.apply_test_command(7,0,0,uc::kButtonA)); input.consume(); CHECK(input.last_consumed_sequence()==7);
    CHECK(!input.apply_test_event(7,{uc::HostEventKind::mouse,3,1,0,0},1000,500));
}
static void host_actions_have_separate_consumers() {
    uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
    input.host_button({uc::PhysicalKind::key,224},true);
    input.host_button({uc::PhysicalKind::key,8},true);
    input.host_button({uc::PhysicalKind::mouse,3},true);
    auto service=input.consume_service_actions();
    CHECK(service.pressed==action_bit(uc::Action::screenshot));
    CHECK(input.consume_service_actions().pressed==0);
    auto game=input.consume_actions();
    CHECK(game.pressed==action_bit(uc::Action::fire) && game.held==action_bit(uc::Action::fire));
    CHECK(input.consume_actions().pressed==0);
    input.host_wheel(2); input.host_button({uc::PhysicalKind::mouse,1},true); input.host_motion(0,-600);
    game=input.consume_actions(); CHECK(game.count>=5 && game.pressed&action_bit(uc::Action::knife));
    CHECK(input.consume().pressed==0); // Legacy KPAD consumes a separate stream.
    auto binds=uc::default_bindings(); binds[size_t(uc::Action::fire)].push_back({uc::BindingKind::physical,{uc::PhysicalKind::key,9},{}});
    input.bindings(binds); input.consume_actions();
    input.host_button({uc::PhysicalKind::mouse,3},false); input.host_button({uc::PhysicalKind::mouse,3},true);
    input.host_button({uc::PhysicalKind::key,9},true); input.consume_actions();
    input.host_button({uc::PhysicalKind::mouse,3},false);
    CHECK(input.consume_actions().held==action_bit(uc::Action::fire));
    input.host_button({uc::PhysicalKind::key,21},true); input.focus(false);
    game=input.consume_actions(); CHECK(game.held==0 && game.pressed==0 && game.count==0);
    input.focus(true); input.host_button({uc::PhysicalKind::key,9},true); CHECK(input.consume_actions().held==0);
    input.host_button({uc::PhysicalKind::key,9},false); input.host_button({uc::PhysicalKind::key,9},true);
    CHECK(input.consume_actions().held==action_bit(uc::Action::fire));
    input.context(uc::InputContext::menu); game=input.consume_actions(); CHECK(game.held==0 && game.pressed==0);
    const auto generation=input.generation();
    input.context(uc::InputContext::menu); CHECK(input.generation()==generation);
    input.focus(false); input.focus(true); CHECK(input.generation()!=generation);
    CHECK(input.consume_actions().generation==input.generation());
    input.focus(false); input.focus(true); input.context(uc::InputContext::gameplay);
    input.host_button({uc::PhysicalKind::key,224},false); input.host_button({uc::PhysicalKind::key,8},false);
    input.host_button({uc::PhysicalKind::key,224},true); input.host_button({uc::PhysicalKind::key,8},true);
    bool overflow=false; try { input.host_wheel(129); } catch(const std::overflow_error&) { overflow=true; }
    CHECK(overflow && input.consume_actions().pressed==0 && input.consume_service_actions().pressed==0 && !input.consume().valid);
}

static size_t probe_calls=0;
static uc::InputSample observed;
static void probe(const ppc::Ctx& c,const uc::InputSample& sample) {
    CHECK(c.r[3]==1); observed=sample; ++probe_calls;
}

static void raw_demo_sample_remains_compatible() {
    const auto path=std::filesystem::temp_directory_path()/std::filesystem::path("uc-input-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".demo");
    std::array<uint8_t,33> legacy{}; legacy[0]=1; legacy[4]=0x3F; legacy[29]=1; // sample tag, x=0.5f, valid=true in the original 32-byte record.
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(legacy.data()),legacy.size());CHECK(out.good());}
    uc::NativeInput replay;replay.demo_play(path);
    auto sample=replay.consume();CHECK(sample.x==.5f && sample.valid && sample.stick_x==0 && sample.stick_y==0);
    {uc::NativeInput recording;recording.focus(true);recording.aim(.25f,-.5f);recording.demo_record(path);recording.consume();}
    CHECK(std::filesystem::file_size(path)==33);
    uc::NativeInput roundtrip;roundtrip.demo_play(path);sample=roundtrip.consume();
    CHECK(sample.x==.25f && sample.y==-.5f && sample.valid && sample.stick_x==0 && sample.stick_y==0);
    std::filesystem::remove(path);
}

// The analog camera rides in the demo as its own tag after the action record; a demo without the tag (every older recording) replays with a zero camera.
static void demo_carries_the_analog_camera() {
    const auto path=std::filesystem::temp_directory_path()/std::filesystem::path("uc-camera-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".demo");
    {
        uc::NativeInput recording;recording.focus(true);recording.context(uc::InputContext::gameplay);recording.demo_record(path);
        recording.consume_actions();                                  // frame 1: no camera
        recording.host_camera({64,-127});recording.consume_actions(); // frame 2
        recording.host_camera({0,0});recording.consume_actions();     // frame 3: back to rest
    }
    uc::NativeInput replay;replay.demo_play(path);
    CHECK((replay.consume_actions().camera==std::array<int8_t,2>{0,0}));
    CHECK((replay.consume_actions().camera==std::array<int8_t,2>{64,-127}));
    CHECK((replay.consume_actions().camera==std::array<int8_t,2>{0,0}));
    std::filesystem::remove(path);
    // A recording made before the camera existed: one action frame with held bits, no camera tag.
    std::vector<uint8_t> legacy{uint8_t(2)};   // Tag::actions
    const uint64_t head[5]={0,0,0,7,0};
    legacy.insert(legacy.end(),reinterpret_cast<const uint8_t*>(head),reinterpret_cast<const uint8_t*>(head)+sizeof head);
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(legacy.data()),std::streamsize(legacy.size()));CHECK(out.good());}
    uc::NativeInput old;old.demo_play(path);
    const auto frame=old.consume_actions();CHECK((frame.generation==7 && frame.camera==std::array<int8_t,2>{0,0}));
    std::filesystem::remove(path);
}

static void absolute_aim_maps_pixels() {
    uc::NativeInput input; input.focus(true);
    CHECK(input.aim_absolute(500,250,1000,500));
    auto center=input.latest_aim(); CHECK(center[0]==0 && center[1]==0);
    CHECK(input.aim_absolute(0,0,1000,500)); CHECK((input.latest_aim()==std::array<float,2>{-1,-1}));
    CHECK(input.aim_absolute(1000,500,1000,500)); CHECK((input.latest_aim()==std::array<float,2>{1,1}));
    CHECK(input.aim_absolute(2000,-10,1000,500)); CHECK((input.latest_aim()==std::array<float,2>{1,-1}));
}
static void menu_pointer_follows_the_wide_layout() {
    // On a wider output a menu's 2D layout is narrowed to k of the width, so the pointer the game reads grows by 1 / k: the mouse over a button is the game's pointer over it.
    // A frame with a 3D scene has the wide projection already: the pointer stays as it is.
    uc::NativeInput input; input.focus(true);
    input.aim(0.3f,0.2f);
    gx::set_view_factors({0.75f,1.f,false});
    gx::set_view_scene(false);
    auto menu=input.consume();
    CHECK(std::abs(menu.x-0.4f)<1e-5f && std::abs(menu.y-0.2f)<1e-5f);
    input.aim(0.9f,0.2f);
    CHECK(input.consume().x==1.f);   // the game's range ends at 1
    CHECK((input.latest_aim()==std::array<float,2>{0.9f,0.2f}));   // the renderer's reticle follows the mouse itself
    gx::set_view_scene(true);
    input.aim(0.3f,0.2f);
    CHECK(std::abs(input.consume().x-0.3f)<1e-5f);
    gx::set_view_factors({});
    gx::set_view_scene(false);
}
static void absolute_aim_rejects_bad_input() {
    uc::NativeInput input; input.aim_absolute(500,250,1000,500);   // not focused
    CHECK((input.latest_aim()==std::array<float,2>{0,0}));
    CHECK(!input.aim_absolute(100,100,1000,500));
    input.focus(true);
    CHECK(!input.aim_absolute(std::numeric_limits<float>::quiet_NaN(),0,1000,500));
    CHECK(!input.aim_absolute(0,0,0,500));
    CHECK(!input.aim_absolute(0,0,1000,0));
    CHECK((input.latest_aim()==std::array<float,2>{0,0}));
}
static void absolute_aim_feeds_gestures() {
    uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
    input.host_button({uc::PhysicalKind::mouse,1},true);   // the knife swing needs the interact button held
    input.aim_absolute(500,600,1000,1000,1000000000);
    input.aim_absolute(500,0,1000,1000,1030000000);   // 600 px up in 30 ms: the knife swing a mouse makes with host_motion(0,-600)
    CHECK((input.consume_actions().pressed&action_bit(uc::Action::knife))!=0);
    uc::NativeInput slow; slow.context(uc::InputContext::gameplay); slow.focus(true);
    slow.host_button({uc::PhysicalKind::mouse,1},true);
    slow.aim_absolute(500,500,1000,1000,1000000000);
    slow.aim_absolute(500,490,1000,1000,1030000000);   // 10 px: aiming, not a swing
    CHECK((slow.consume_actions().pressed&action_bit(uc::Action::knife))==0);
}
static void aim_test_event_sets_pointer_and_feeds_gestures() {
    uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
    CHECK(input.apply_test_event(1,{uc::HostEventKind::aim,0,0,.5f,-.5f},1000,500));
    CHECK((input.latest_aim()==std::array<float,2>{.5f,-.5f}));
    CHECK(!input.apply_test_event(2,{uc::HostEventKind::aim,0,0,1.5f,0},1000,500));
    CHECK(!input.apply_test_event(2,{uc::HostEventKind::aim,1,0,0,0},1000,500));
    CHECK(!input.apply_test_event(2,{uc::HostEventKind::aim,0,0,0,0},0,500));
    CHECK((input.latest_aim()==std::array<float,2>{.5f,-.5f}));
}
static void absolute_gestures_follow_the_scale() {
    // 40 px in 8 ms is a swing at 100% display scale; at 200% the same 40 px are 20 of the points a mouse counts in: not fast enough
    for(const float scale:{1.f,.5f}) {
        uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
        input.host_button({uc::PhysicalKind::mouse,1},true);
        input.aim_absolute(500,500,1000,1000,1000000000,scale);
        input.aim_absolute(500,460,1000,1000,1008000000,scale);
        const bool knife=(input.consume_actions().pressed&action_bit(uc::Action::knife))!=0;
        CHECK(knife==(scale==1.f));
    }
}
static void smoothing_moves_part_way() {
    uc::NativeInput input; input.focus(true); input.pointer_smoothing(.5f);
    CHECK(input.aim_absolute(0,0,1000,500));   // the first point is taken as it is
    CHECK((input.latest_aim()==std::array<float,2>{-1,-1}));
    CHECK(input.aim_absolute(1000,500,1000,500));
    auto early=input.latest_aim(); CHECK(early[0]<-.5f && early[1]<-.5f);   // the pointer eases toward the new point: it has hardly left yet
    std::this_thread::sleep_for(std::chrono::milliseconds(400));            // and arrives without any further event: a gun that stops sending must not leave it short
    auto late=input.latest_aim(); CHECK(std::abs(late[0]-1)<.001f && std::abs(late[1]-1)<.001f);
    input.focus(false); input.focus(true);
    CHECK(input.aim_absolute(250,125,1000,500));   // after a focus loss the first point is taken as it is again
    CHECK((input.latest_aim()==std::array<float,2>{-.5f,-.5f}));
}

static uc::NativeInput& zone_input(uc::NativeInput& input,uc::ReloadZone zone);
static void a_zone_pulse_leaves_a_held_reload_button_held() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::corner);
    input.host_button({uc::PhysicalKind::key,21},true);                      // the reload key, held by the player
    input.consume_actions();
    input.aim(-.98f,-.98f); input.host_button({uc::PhysicalKind::mouse,3},true); input.consume_actions();
    input.host_button({uc::PhysicalKind::mouse,3},false);
    const auto frame=input.consume_actions();                                // the pulse ends; the key is still down
    CHECK((frame.held&action_bit(uc::Action::reload))!=0 && (frame.released&action_bit(uc::Action::reload))==0);
}
static uc::NativeInput& zone_input(uc::NativeInput& input,uc::ReloadZone zone) {
    input.context(uc::InputContext::gameplay); input.focus(true); input.reload_zone(zone,.05f); input.consume_actions();
    return input;
}
static bool has_event(const uc::ActionFrame& frame,uc::Action action,bool down) {
    for(size_t i=0;i<frame.count;++i) if(frame.events[i].action==action && frame.events[i].down==down) return true;
    return false;
}
static void corner_zone_turns_fire_into_reload() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::corner);
    input.aim(-.98f,-.98f);
    input.host_button({uc::PhysicalKind::mouse,3},true);
    auto frame=input.consume_actions();
    CHECK(frame.pressed==action_bit(uc::Action::reload) && frame.held==action_bit(uc::Action::reload));
    CHECK(has_event(frame,uc::Action::reload,true) && !has_event(frame,uc::Action::fire,true));
    frame=input.consume_actions();   // the reload is one frame long
    CHECK(frame.held==0 && frame.pressed==0 && frame.released==action_bit(uc::Action::reload) && has_event(frame,uc::Action::reload,false));
    CHECK(input.consume_actions().released==0);
}
static void corner_zone_ignores_edge_middle_and_centre() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::corner);
    input.aim(-.98f,0); input.host_button({uc::PhysicalKind::mouse,3},true);
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
    input.host_button({uc::PhysicalKind::mouse,3},false); input.consume_actions();
    input.aim(0,0); input.host_button({uc::PhysicalKind::mouse,3},true);
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
}
static void edge_zone_triggers_on_any_edge() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::edge);
    input.aim(.2f,.95f); input.host_button({uc::PhysicalKind::mouse,3},true);
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::reload));
    input.consume_actions(); input.host_button({uc::PhysicalKind::mouse,3},false); input.consume_actions();
    input.aim(.2f,.5f); input.host_button({uc::PhysicalKind::mouse,3},true);
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
}
static void fire_release_after_suppressed_press_is_hidden() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::corner);
    input.aim(.99f,.99f); input.host_button({uc::PhysicalKind::mouse,3},true); input.consume_actions();
    input.aim(0,0); input.host_button({uc::PhysicalKind::mouse,3},false);
    auto frame=input.consume_actions();   // the reload ends; the game never sees the fire release
    CHECK(frame.held==0 && frame.released==action_bit(uc::Action::reload) && !has_event(frame,uc::Action::fire,false));
    input.host_button({uc::PhysicalKind::mouse,3},true);   // a later press outside the zone is a normal shot
    CHECK(input.consume_actions().pressed==action_bit(uc::Action::fire));
}
static void drifting_into_the_zone_while_firing_is_untouched() {
    uc::NativeInput input; zone_input(input,uc::ReloadZone::corner);
    input.aim(0,0); input.host_button({uc::PhysicalKind::mouse,3},true); input.consume_actions();
    input.aim(-.99f,-.99f); input.host_button({uc::PhysicalKind::mouse,3},false);
    auto frame=input.consume_actions();
    CHECK(frame.released==action_bit(uc::Action::fire) && !(frame.pressed&action_bit(uc::Action::reload)));
}
static void zone_is_off_by_default_and_in_menu_and_qte() {
    uc::NativeInput off; off.context(uc::InputContext::gameplay); off.focus(true); off.aim(-.99f,-.99f);
    off.host_button({uc::PhysicalKind::mouse,3},true);
    CHECK(off.consume_actions().pressed==action_bit(uc::Action::fire));
    for(auto context:{uc::InputContext::menu,uc::InputContext::qte}) {
        uc::NativeInput input; zone_input(input,uc::ReloadZone::edge); input.context(context); input.aim(-.99f,-.99f);
        input.host_button({uc::PhysicalKind::mouse,3},true);
        CHECK((input.consume_actions().pressed&action_bit(uc::Action::reload))==0);
    }
    uc::NativeInput bad;
    for(float size:{0.f,-.1f,.9f,std::numeric_limits<float>::quiet_NaN()}) {
        bool refused=false; try { bad.reload_zone(uc::ReloadZone::corner,size); } catch(const std::invalid_argument&) { refused=true; }
        CHECK(refused);
    }
}
static void demo_records_the_converted_frame() {
    const auto path=std::filesystem::temp_directory_path()/"uc-reload-zone-demo.bin";
    {
        uc::NativeInput input; zone_input(input,uc::ReloadZone::corner); input.demo_record(path);
        input.aim(-.99f,-.99f); input.host_button({uc::PhysicalKind::mouse,3},true); input.consume_actions(); input.consume_actions();
    }
    uc::NativeInput replay; replay.demo_play(path);
    CHECK(replay.consume_actions().pressed==action_bit(uc::Action::reload));
    CHECK(replay.consume_actions().released==action_bit(uc::Action::reload));
    std::filesystem::remove(path);
}

int main() {
    corner_zone_turns_fire_into_reload();
    corner_zone_ignores_edge_middle_and_centre();
    edge_zone_triggers_on_any_edge();
    fire_release_after_suppressed_press_is_hidden();
    drifting_into_the_zone_while_firing_is_untouched();
    zone_is_off_by_default_and_in_menu_and_qte();
    demo_records_the_converted_frame();
    absolute_aim_maps_pixels();
    menu_pointer_follows_the_wide_layout();
    aim_test_event_sets_pointer_and_feeds_gestures();
    absolute_aim_rejects_bad_input();
    absolute_aim_feeds_gestures();
    smoothing_moves_part_way();
    absolute_gestures_follow_the_scale();
    a_zone_pulse_leaves_a_held_reload_button_held();
    demo_carries_the_analog_camera();
    raw_demo_sample_remains_compatible();
    host_actions_have_separate_consumers();
    host_test_commands_are_atomic_and_acknowledged();
    uc::NativeInput input;
    input.focus(true);
    CHECK(input.move_raw(100,50,1000,500));
    auto first=input.consume(); CHECK(std::abs(first.x-0.2f)<0.00001f && std::abs(first.y-0.2f)<0.00001f);
    CHECK(!input.move_raw(std::numeric_limits<float>::quiet_NaN(),0,1000,500));
    CHECK(!input.move_raw(1,1,0,500));
    input.button(uc::kButtonA,true); input.button(uc::kButtonA,false);
    auto click=input.consume(); CHECK(click.held==0 && click.pressed==uc::kButtonA && click.released==uc::kButtonA);
    CHECK(input.consume().pressed==0);
    input.button(uc::kButtonA,true); input.consume(); input.focus(false);
    auto lost=input.consume(); CHECK(!lost.valid && lost.held==0 && lost.pressed==0 && lost.released==uc::kButtonA);
    CHECK(!input.move_raw(50,0,1000,500)); input.focus(true);
    CHECK(input.apply_test_command(1,0.2f,0.2f,uc::kButtonA));
    CHECK(input.last_consumed_sequence()==0);
    CHECK(input.consume().pressed==uc::kButtonA && input.last_consumed_sequence()==1);
    CHECK(!input.apply_test_command(1,0,0,0));
    CHECK(!input.apply_test_command(2,2,0,0));
    CHECK(input.consume().held==uc::kButtonA);
    CHECK(input.apply_test_command(2,0.2f,0.2f,0)); input.consume();
    CHECK(!input.aim(2,0)); CHECK(input.aim(0.2f,0.2f));
    Fix fixture; auto& c=fixture.c; uc::input_hle_init(&input);
    uc::input_hle_set_probe(probe);
    c.r[3]=0; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(c.r[3]==0 && ppc::rd32(c,0x80001000)==1);
    input.button(uc::kButtonA,true);
    c.r[3]=0; c.r[4]=0xCC008000; c.r[5]=16; hle_KPADRead(c); CHECK(c.r[3]==0);
    CHECK(probe_calls==0);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=16; hle_KPADRead(c);
    CHECK(c.r[3]==1 && ppc::rd32(c,0x80002000)==0x800 && ppc::rd32(c,0x80002004)==0x800);
    CHECK(probe_calls==1 && observed.held==0x800 && observed.pressed==0x800 && observed.valid);
    CHECK(std::abs(ppc::rdf32(c,0x80002020)-0.2f)<0.00001f && ppc::rd8(c,0x8000205E)==1);
    CHECK(ppc::rd8(c,0x8000205C)==1 && ppc::rd8(c,0x8000205F)==5);
    CHECK(ppc::rdf32(c,0x80002060)==0 && ppc::rdf32(c,0x80002064)==0);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=1; hle_KPADRead(c); CHECK(c.r[3]==1 && ppc::rd32(c,0x80002004)==0);
    c.r[3]=1; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(int32_t(c.r[3])==-1);
    uc::NativeInput second; second.focus(true); second.aim(-0.6f,0.4f); second.button(uc::kButtonB,true);
    uc::input_hle_init(&input,&second);
    c.r[3]=1; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(c.r[3]==0);
    c.r[3]=1; c.r[4]=0x80003000; c.r[5]=1; hle_KPADRead(c);
    CHECK(c.r[3]==1 && ppc::rd32(c,0x80003000)==uc::kButtonB);
    CHECK(std::abs(ppc::rdf32(c,0x80003020)+0.6f)<0.00001f);
    CHECK(input.latest_aim()[0]==0.2f);
    c.r[3]=2; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(int32_t(c.r[3])==-1);
    uc::InputFrame network; network.sample.valid=true; network.sample.x=0.7f;
    network.host=true; network.actions.pressed=uint64_t(1)<<unsigned(uc::Action::reload); network.actions.generation=network.generation=42;
    second.install_frame(network); second.context(uc::InputContext::gameplay);
    CHECK(second.consume().x==0.7f && second.generation()==42 && second.using_host_input());
    CHECK(second.consume_actions().pressed==network.actions.pressed);
    uc::input_hle_init(nullptr);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=16; hle_KPADRead(c); CHECK(c.r[3]==0);
    CHECK(probe_calls==2);
    uc::input_hle_set_probe(nullptr);
}
