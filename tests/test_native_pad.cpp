#include "check.h"
#include "native_input.h"
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
using namespace uc;
constexpr uint64_t kSecond=1000000000ull;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static std::array<int8_t,2> cam(int8_t x,int8_t y) { return {x,y}; }
static size_t downs(const ActionFrame& frame,Action action) {
    size_t n=0; for(size_t i=0;i<frame.count;++i) n+=frame.events[i].action==action && frame.events[i].down; return n;
}
static bool near(float a,float b,float tolerance=1e-3f) { return std::fabs(a-b)<=tolerance; }
template<class F> static bool refuses(F f) { try { f(); } catch(const std::invalid_argument&) { return true; } return false; }
// A focused input on the harness window (1280x720), in gameplay unless said otherwise; the first consume sets the clock reference.
struct Pad {
    NativeInput input;
    uint64_t now=kSecond;
    explicit Pad(InputContext context=InputContext::gameplay) { input.extent(1280,720); input.focus(true); input.context(context); input.consume_at(now); }
    // Time passes in 50 ms steps, as frames do (one step may move the pointer for at most 100 ms).
    InputSample run(double seconds) { InputSample last; for(int i=0;i<int(seconds*20+.5);++i) {now+=kSecond/20; last=input.consume_at(now);} return last; }
};
static void apply(Pad& p,const GamepadSettings& settings) { p.input.pad_settings(settings); }
static void left_stick_moves_the_pointer_per_second() {
    Pad p; p.input.host_pad_stick(0,PadSide::left,1,0);
    CHECK(near(p.run(1).x,1.f));                                                   // a second at full deflection runs past the window edge: clamped
    Pad q; q.input.host_pad_stick(0,PadSide::left,0,1);
    auto s=q.run(.25);                                                             // a quarter second: 0.3 screen heights, normalized 2*0.3
    CHECK(near(s.y,.6f) && s.x==0);
    CHECK(near(q.run(.25).y,1.f));
    Pad r; r.input.host_pad_stick(0,PadSide::left,.575f,0);                        // half way after the dead zone: a quarter of the speed, horizontal in the same pixels per second
    CHECK(near(r.run(.5).x,2*.3f*.5f*720/1280,2e-3f));
}
static void dt_is_capped_and_an_idle_stick_is_still() {
    Pad p; p.input.host_pad_stick(0,PadSide::left,0,.5f);
    auto s=p.input.consume_at(100*kSecond);                                        // a hitch of 99 s counts as 0.1 s
    CHECK(near(s.y,2*.1f*std::pow((.5f-.15f)/.85f,2.f)*1.2f,2e-3f));
    Pad idle; idle.input.host_pad_stick(0,PadSide::left,.1f,-.1f);                 // inside the dead zone
    s=idle.input.consume_at(10*kSecond); CHECK(s.x==0 && s.y==0);
    Pad unfocused; unfocused.input.host_pad_stick(0,PadSide::left,1,0); unfocused.input.focus(false);
    s=unfocused.input.consume_at(2*kSecond); CHECK(s.x==0 && !s.valid);            // an unfocused game (a released cursor) is not aimed by the stick
    unfocused.input.focus(true); CHECK(unfocused.run(.25).x>0);                    // a stick that stayed pushed aims again at once: the pad kept its state
    // The camera stick follows the focus too: with the cursor released it does not turn the camera, and it does again once the cursor is back.
    Pad released; released.input.host_pad_stick(0,PadSide::right,1,0);
    CHECK(released.input.consume_actions().camera==cam(127,0));
    released.input.focus(false); CHECK(released.input.consume_actions().camera==cam(0,0));
    released.input.host_pad_stick(0,PadSide::right,0,-1); CHECK(released.input.consume_actions().camera==cam(0,0));   // pushed while released: still nothing
    released.input.focus(true); CHECK(released.input.consume_actions().camera==cam(0,127));                          // and the stick as it is now once focus is back
}
static void two_pads_add_after_their_own_dead_zones() {
    Pad p; p.input.host_pad_stick(0,PadSide::left,.1f,.1f); p.input.host_pad_stick(1,PadSide::left,.1f,-.1f);   // two idle pads drifting, each inside its dead zone
    auto s=p.input.consume_at(2*kSecond); CHECK(s.x==0 && s.y==0);
    p.input.host_pad_stick(0,PadSide::left,0,.5f); p.input.host_pad_stick(1,PadSide::left,0,.5f);
    s=p.run(.25); CHECK(s.y>.15f);                                                 // two real pushes add up (one alone gives about .1)
    CHECK(refuses([&]{p.input.host_pad_stick(8,PadSide::left,0,0);}));
    CHECK(refuses([&]{p.input.host_pad_stick(0,PadSide::left,2,0);}));
    CHECK(refuses([&]{p.input.host_pad_stick(0,PadSide::left,std::numeric_limits<float>::quiet_NaN(),0);}));
}
static void roles_choose_the_aim_and_camera_sticks() {
    Pad p;
    p.input.host_pad_stick(0,PadSide::right,.55f,0);
    auto frame=p.input.consume_actions(); CHECK(frame.camera==cam(64,0) || frame.camera==cam(63,0));
    CHECK(p.input.consume_at(2*kSecond).x==0);                                     // the right stick does not aim
    p.input.host_pad_stick(0,PadSide::right,0,-1);                                  // SDL y points down: pushed up is camera up
    CHECK(p.input.consume_actions().camera==cam(0,127));
    GamepadSettings swapped; swapped.aim_stick=StickRole::right; swapped.camera_stick=StickRole::left; apply(p,swapped);
    p.input.host_pad_stick(0,PadSide::right,0,0); p.input.host_pad_stick(0,PadSide::left,0,0);
    p.input.host_pad_stick(0,PadSide::right,1,0);
    CHECK(p.input.consume_at(3*kSecond).x>0);
    CHECK(p.input.consume_actions().camera==cam(0,0));
    p.input.host_pad_stick(0,PadSide::left,-1,0);
    CHECK(p.input.consume_actions().camera==cam(-127,0));
    GamepadSettings none; none.camera_stick=StickRole::none; apply(p,none); p.input.host_pad_stick(0,PadSide::right,1,0);
    CHECK(p.input.consume_actions().camera==cam(0,0));
    GamepadSettings bad; bad.aim_stick=bad.camera_stick=StickRole::left;
    CHECK(refuses([&]{p.input.pad_settings(bad);}));
}
static void recenter_action_centres_the_pointer() {
    Pad p; p.input.aim(.5f,-.25f);
    p.input.host_button({PhysicalKind::pad_button,7},true);
    const auto frame=p.input.consume_actions();
    CHECK(!(frame.pressed&bit(Action::recenter)) && !(frame.held&bit(Action::recenter)) && downs(frame,Action::recenter)==0);   // it never reaches demos or the network
    auto s=p.input.consume_at(2*kSecond); CHECK(s.x==0 && s.y==0);
    Pad menu(InputContext::menu); menu.input.aim(.5f,.5f); menu.input.host_button({PhysicalKind::pad_button,7},true);
    menu.input.consume_actions(); CHECK(menu.input.consume_at(2*kSecond).x==.5f);   // only in gameplay
}
static void gyro_modes() {
    GamepadSettings s; s.gyro.mode=GyroMode::off;
    Pad p; apply(p,s);
    p.input.host_pad_gyro(1,0,kSecond); p.input.host_pad_gyro(1,0,kSecond+kSecond/100);
    CHECK(p.input.consume_at(2*kSecond).x==0);                                      // off: nothing
    s.gyro.mode=GyroMode::always; apply(p,s);
    p.input.host_pad_gyro(1,0,3*kSecond); p.input.host_pad_gyro(1,0,3*kSecond+kSecond/100);   // 1 rad/s left for 10 ms: -20 counts of 1280 px
    CHECK(near(p.input.consume_at(4*kSecond).x,-2*20.f/1280,1e-4f));
    p.input.host_pad_gyro(0,1,5*kSecond); p.input.host_pad_gyro(0,1,5*kSecond+kSecond/100);   // tilting up moves up
    CHECK(near(p.input.consume_at(6*kSecond).y,-2*20.f/720,1e-4f));
    p.input.host_pad_gyro(.005f,.005f,7*kSecond); p.input.host_pad_gyro(.005f,.005f,7*kSecond+kSecond/100);   // desk noise below the dead zone
    const auto before=p.input.consume_at(8*kSecond);
    const auto after=p.input.consume_at(9*kSecond); CHECK(after.x==before.x && after.y==before.y);
    s.gyro.mode=GyroMode::hold; apply(p,s);                                         // only while the hold input (the left trigger) is down
    p.input.host_pad_gyro(1,0,11*kSecond); p.input.host_pad_gyro(1,0,11*kSecond+kSecond/100);
    CHECK(p.input.consume_at(12*kSecond).x==after.x);
    p.input.host_button({PhysicalKind::pad_axis,9},true);
    p.input.host_pad_gyro(1,0,13*kSecond); p.input.host_pad_gyro(1,0,13*kSecond+kSecond/100);
    CHECK(p.input.consume_at(14*kSecond).x<after.x);
    p.input.host_button({PhysicalKind::pad_axis,9},false);
    CHECK(refuses([&]{p.input.host_pad_gyro(std::numeric_limits<float>::quiet_NaN(),0,1);}));
}
static void a_gyro_swing_is_a_knife_and_a_shake_is_a_qte_step() {
    GamepadSettings s; s.gyro.mode=GyroMode::always;
    Pad swing; apply(swing,s);
    swing.input.host_button({PhysicalKind::pad_button,0},true); swing.input.consume_actions();   // interact held, as with the mouse
    uint64_t t=kSecond; swing.input.host_pad_gyro(0,0,t);
    t+=8000000; swing.input.host_pad_gyro(-4,0,t);                                  // 4 rad/s to the right for 8 ms: 64 counts at 8000 counts/s
    CHECK(downs(swing.input.consume_actions(),Action::knife)==1);
    Pad slow; apply(slow,s); slow.input.host_button({PhysicalKind::pad_button,0},true); slow.input.consume_actions();
    t=kSecond; for(int i=0;i<200;++i) {slow.input.host_pad_gyro(-.3f,0,t); t+=4000000;}
    CHECK(downs(slow.input.consume_actions(),Action::knife)==0);                    // an aiming-speed turn is no swing
    Pad shake(InputContext::qte); apply(shake,s); shake.input.qte_window(true);
    size_t steps=0; t=kSecond;
    for(int i=0;i<12;++i) {
        for(int k=0;k<3;++k) {shake.input.host_pad_gyro(i&1 ? 6.f : -6.f,0,t); t+=10000000;}   // 6 rad/s for 30 ms: ±360 counts per stroke
        steps+=downs(shake.input.consume_actions(),Action::qte_shake);
    }
    CHECK(steps>=8);
    Pad off(InputContext::qte); off.input.qte_window(true); size_t none=0; t=kSecond;   // gyro mode off: no steps
    for(int i=0;i<12;++i) {off.input.host_pad_gyro(i&1 ? 6.f : -6.f,0,t); t+=30000000; none+=downs(off.input.consume_actions(),Action::qte_shake);}
    CHECK(none==0);
}
static void reset_releases_the_gamepad() {
    Pad p; p.input.host_button({PhysicalKind::pad_axis,11},true); p.input.host_pad_stick(0,PadSide::left,1,0); p.input.host_pad_stick(0,PadSide::right,1,0);
    CHECK(p.input.consume_actions().held&bit(Action::fire));
    p.input.pad_reset();
    auto frame=p.input.consume_actions(); CHECK(!(frame.held&bit(Action::fire)) && (frame.released&bit(Action::fire)) && frame.camera==cam(0,0));
    CHECK(p.input.consume_at(2*kSecond).x==0);
    p.input.host_button({PhysicalKind::key,21},true); p.input.pad_reset();           // the keyboard is not touched by a gamepad reset
    CHECK(downs(p.input.consume_actions(),Action::reload)==1);
}
static void test_events_reach_the_pad_pipeline() {
    Pad p; uint64_t sequence=0;
    const auto event=[&](HostEventKind kind,uint32_t code,int value,float x=0,float y=0) { return p.input.apply_test_event(++sequence,{kind,code,value,x,y},1280,720); };
    CHECK(event(HostEventKind::pad_axis,11,1) && (p.input.consume_actions().held&bit(Action::fire)));
    CHECK(event(HostEventKind::pad_axis,11,0));
    CHECK(event(HostEventKind::pad_button,0,1) && downs(p.input.consume_actions(),Action::interact)==1);
    CHECK(event(HostEventKind::pad_stick,1,0,.55f,0) && p.input.consume_actions().camera[0]>=63);
    CHECK(event(HostEventKind::pad_stick,0,0,0,1) && p.input.consume_at(kSecond+kSecond/4).y>0);
    CHECK(event(HostEventKind::pad_gyro,0,0,1,0));
    const auto last_valid=sequence;
    CHECK(!event(HostEventKind::pad_button,32,1) && !event(HostEventKind::pad_axis,16,1) && !event(HostEventKind::pad_button,0,2));
    CHECK(!event(HostEventKind::pad_stick,2,0,0,0) && !event(HostEventKind::pad_stick,0,1,0,0) && !event(HostEventKind::pad_stick,0,0,2,0));
    CHECK(!event(HostEventKind::pad_gyro,1,0,0,0) && !event(HostEventKind::pad_gyro,0,0,std::numeric_limits<float>::infinity(),0));
    CHECK(!p.input.apply_test_event(last_valid,{HostEventKind::pad_button,0,1,0,0},1280,720));   // sequences stay monotonic
    CHECK(event(HostEventKind::focus,0,0) && p.input.consume_actions().camera==cam(0,0));   // focus loss clears the sticks too
}
int main() {
    left_stick_moves_the_pointer_per_second(); dt_is_capped_and_an_idle_stick_is_still(); two_pads_add_after_their_own_dead_zones();
    roles_choose_the_aim_and_camera_sticks(); recenter_action_centres_the_pointer(); gyro_modes();
    a_gyro_swing_is_a_knife_and_a_shake_is_a_qte_step(); reset_releases_the_gamepad(); test_events_reach_the_pad_pipeline();
}
