#include "check.h"
#include "native_input.h"
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
using namespace uc;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static PhysicalInput wii(uint16_t code) { return {PhysicalKind::wii_button,code}; }
static bool near(float a,float b,float tolerance=1e-4f) { return std::fabs(a-b)<=tolerance; }
struct Remote {
    NativeInput input;
    WiimoteTarget& target=input;
    Remote() { input.extent(1280,720); input.focus(true); input.context(InputContext::gameplay); }
};
static void buttons_drive_actions() {
    Remote r;
    r.target.host_button(wii(7),true);                                       // B, the trigger
    auto frame=r.input.consume_actions();
    CHECK(frame.pressed==bit(Action::fire) && frame.held==bit(Action::fire));
    r.target.host_button(wii(7),false);
    CHECK(r.input.consume_actions().released==bit(Action::fire));
    r.target.host_button(wii(8),true);                                       // A
    CHECK(r.input.consume_actions().pressed==bit(Action::interact));
}
static void the_nunchuk_stick_is_the_analog_camera() {
    Remote r;
    r.target.host_wii_stick(1,0);
    CHECK((r.input.consume_actions().camera==std::array<int8_t,2>{127,0}));
    r.target.host_wii_stick(0,-1);                                          // pushed up: y is down-positive like SDL's sticks, the camera's y is up
    CHECK((r.input.consume_actions().camera==std::array<int8_t,2>{0,127}));
    r.target.host_wii_stick(.5f,0);
    const auto half=r.input.consume_actions().camera; CHECK(half[0]>55 && half[0]<70 && half[1]==0);
    r.target.host_wii_stick(0,0);
    CHECK((r.input.consume_actions().camera==std::array<int8_t,2>{0,0}));
}
static void the_pointer_follows_the_ir_camera() {
    Remote r;
    r.target.host_wii_pointer(.5f,-.25f,1);
    CHECK(near(r.input.latest_aim()[0],.5f) && near(r.input.latest_aim()[1],-.25f));
    WiimoteSettings settings; settings.smoothing=.5f; r.input.wiimote_settings(settings);
    r.target.host_wii_pointer(-.5f,.75f,2);                                  // it eases toward the new reading ...
    CHECK(r.input.latest_aim()[0]>.3f);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));            // ... and arrives although the remote sends nothing more
    CHECK(near(r.input.latest_aim()[0],-.5f,.001f) && near(r.input.latest_aim()[1],.75f,.001f));
    Remote q; q.input.focus(false);
    q.target.host_wii_pointer(.5f,.5f,1);
    CHECK(near(q.input.latest_aim()[0],0) && near(q.input.latest_aim()[1],0));   // an unfocused window takes nothing
    bool refused=false; try { r.target.host_wii_pointer(std::nanf(""),0,3); } catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
}
static void a_lost_ir_jump_is_not_a_knife_swing() {
    Remote r;
    r.target.host_button(wii(8),true);                                       // interact held, the knife's condition
    r.target.host_wii_pointer(-.9f,0,1000000000);
    r.target.host_wii_pointer(.9f,0,1010000000);                             // across the whole screen in 10 ms
    CHECK((r.input.consume_actions().pressed&bit(Action::knife))==0);
}
static void a_swing_of_the_remote_is_the_knife() {
    Remote r;
    r.target.host_button(wii(8),true);
    r.target.host_wii_motion(300,0,1000000000);
    CHECK((r.input.consume_actions().pressed&bit(Action::knife))!=0);
    Remote q;
    q.target.host_wii_motion(300,0,1000000000);                              // without interact held: nothing to swing
    CHECK((q.input.consume_actions().pressed&bit(Action::knife))==0);
}
static void reset_lets_go_of_the_remote_only() {
    Remote r;
    r.input.host_button({PhysicalKind::pad_button,2},true);                  // a gamepad's west button: reload (not interact: with fire that is the grenade throw)
    r.target.host_button(wii(7),true); r.target.host_wii_stick(1,0);
    r.input.consume_actions();
    r.target.wii_reset();
    const auto frame=r.input.consume_actions();
    CHECK((frame.released&bit(Action::fire))!=0 && (frame.camera==std::array<int8_t,2>{0,0}));
    CHECK((frame.held&bit(Action::reload))!=0);                              // the gamepad still holds its button
}
static void settings_replace_the_table_and_are_checked() {
    Remote r;
    WiimoteSettings settings; settings.bindings[size_t(Action::fire)]={{BindingKind::physical,wii(3),{}}};   // the D-pad up fires
    r.input.wiimote_settings(settings);
    r.target.host_button(wii(3),true);
    CHECK(r.input.consume_actions().pressed==bit(Action::fire));
    r.target.host_button(wii(3),false); r.input.consume_actions();
    r.target.host_button(wii(7),true);                                       // B no longer fires
    CHECK(r.input.consume_actions().pressed==0);
    WiimoteSettings bad; bad.smoothing=1.5f;
    bool refused=false; try { r.input.wiimote_settings(bad); } catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
    r.target.host_button(wii(7),false); r.target.host_button(wii(3),true);
    CHECK(r.input.consume_actions().pressed==bit(Action::fire));            // the refused set changed nothing
}
static void test_events_reach_the_same_places() {
    NativeInput input; input.focus(true); input.context(InputContext::gameplay);
    CHECK(input.apply_test_event(1,{HostEventKind::wii_button,7,1,0,0},1280,720));
    CHECK(input.consume_actions().pressed==bit(Action::fire));
    CHECK(input.apply_test_event(2,{HostEventKind::wii_button,7,0,0,0},1280,720));
    CHECK(input.consume_actions().released==bit(Action::fire));
    CHECK(input.apply_test_event(3,{HostEventKind::wii_pointer,0,0,.25f,-.5f},1280,720));
    CHECK(near(input.latest_aim()[0],.25f) && near(input.latest_aim()[1],-.5f));
    CHECK(input.apply_test_event(4,{HostEventKind::wii_stick,0,0,1,0},1280,720));
    CHECK((input.consume_actions().camera==std::array<int8_t,2>{127,0}));
    input.host_button(wii(8),true);                                          // interact held for the swing
    CHECK(input.apply_test_event(5,{HostEventKind::wii_motion,0,0,300,0},1280,720));
    CHECK((input.consume_actions().pressed&bit(Action::knife))!=0);
    CHECK(!input.apply_test_event(6,{HostEventKind::wii_button,13,1,0,0},1280,720));   // outside the remote's buttons
    CHECK(!input.apply_test_event(6,{HostEventKind::wii_pointer,0,0,2,0},1280,720));
    CHECK(!input.apply_test_event(6,{HostEventKind::wii_button,7,1,1,0},1280,720));    // a digital event carries no analog values
}
int main() {
    test_events_reach_the_same_places();
    buttons_drive_actions();
    the_nunchuk_stick_is_the_analog_camera();
    the_pointer_follows_the_ir_camera();
    a_lost_ir_jump_is_not_a_knife_swing();
    a_swing_of_the_remote_is_the_knife();
    reset_lets_go_of_the_remote_only();
    settings_replace_the_table_and_are_checked();
}
