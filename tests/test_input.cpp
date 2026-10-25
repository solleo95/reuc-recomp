#include "check.h"
#include "native_input.h"
#include "ppc_fixture.h"
#include <cmath>
#include <limits>
#include <chrono>
#include <fstream>
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

int main() {
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
