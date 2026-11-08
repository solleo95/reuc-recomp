#include "check.h"
#include "fake_wiimote.h"
#include <condition_variable>
using namespace uc;
using namespace std::chrono_literals;
static PhysicalInput wii(WiiButton button) { return {PhysicalKind::wii_button,uint16_t(button)}; }

struct Calls : WiimoteTarget {
    std::vector<std::pair<PhysicalInput,bool>> buttons;
    std::vector<std::array<float,2>> pointers,sticks,motions;
    unsigned resets=0;
    void host_button(PhysicalInput input,bool down) override { buttons.emplace_back(input,down); }
    void host_wii_pointer(float x,float y,uint64_t) override { pointers.push_back({x,y}); }
    void host_wii_stick(float x,float y) override { sticks.push_back({x,y}); }
    void host_wii_motion(float x,float y,uint64_t) override { motions.push_back({x,y}); }
    void wii_reset() override { ++resets; }
};
struct Rig {
    Opener opener;
    std::shared_ptr<FakeRemote> remote=std::make_shared<FakeRemote>();
    WiimoteSettings settings;
    Calls calls;
    std::unique_ptr<WiimoteInput> input;
    explicit Rig(bool remote_present=true,bool enabled=true) {
        if(remote_present) opener.remotes.push_back(remote);
        settings.enabled=enabled;
        input=std::make_unique<WiimoteInput>(opener.fn());
        update();   // the first update tells the thread whether it may look for a remote at all
    }
    void update(bool active=true) { input->update(&calls,active,settings); }
    bool connected() { return input->status().connected; }
    bool until(const std::function<bool()>& ready) { return wait_for([&]{ update(); return ready(); }); }
};
static void handshake_writes_the_init_sequence_in_order() {
    Rig rig; const auto steps=wii_init_sequence();
    CHECK(wait_for([&]{ return rig.remote->writes()>=steps.size()+1; }));    // the sequence, then a status request
    std::lock_guard lock(rig.remote->m);
    for(size_t i=0;i<steps.size();++i) CHECK(rig.remote->written[i]==steps[i].report);
    const auto request=wii_status_request();
    CHECK((rig.remote->written[steps.size()]==std::vector<uint8_t>(request.begin(),request.end())));
}
static void buttons_flow_to_the_target_in_order() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x08));   // A down
    rig.remote->push(report(0x00,0x00));   // A up
    rig.remote->push(report(0x00,0x04));   // B down
    CHECK(rig.until([&]{ return rig.calls.buttons.size()>=3; }));
    CHECK(rig.calls.buttons[0].first==wii(WiiButton::a) && rig.calls.buttons[0].second);
    CHECK(rig.calls.buttons[1].first==wii(WiiButton::a) && !rig.calls.buttons[1].second);
    CHECK(rig.calls.buttons[2].first==wii(WiiButton::b) && rig.calls.buttons[2].second);
}
static void the_ir_pointer_flows_and_a_lost_camera_keeps_it() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0,0,{0x82,0x82,0x9C},true));
    CHECK(rig.until([&]{ return !rig.calls.pointers.empty(); }));
    rig.remote->push(report(0,0,{0x82,0x82,0x9C},false));                    // the camera lost the sensor bar
    for(int i=0;i<40;++i) { rig.update(); std::this_thread::sleep_for(5ms); }   // reports with the old picture may still be on their way: let them pass
    const auto settled=rig.calls.pointers.size();
    for(int i=0;i<40;++i) { rig.update(); std::this_thread::sleep_for(5ms); }
    CHECK(rig.calls.pointers.size()==settled && rig.calls.resets==0);         // no new pointer, no jump to the middle, nothing released
}
static void a_nunchuk_looking_report_without_the_extension_flag_is_ignored() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0,0,{0x82,0x82,0x9C},false,true,{223,128},0xFE));   // no status report ever said a nunchuk is plugged in
    for(int i=0;i<60;++i) { rig.update(); std::this_thread::sleep_for(5ms); }
    CHECK(!rig.input->status().has_nunchuk && rig.calls.sticks.empty() && rig.calls.buttons.empty());
}
static void the_nunchuk_stick_flows() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push({0x20,0,0,0x02,0,0,0xC0});                                  // the remote says an extension is plugged in
    rig.remote->push(report(0,0,{0x82,0x82,0x9C},false,true,{223,128}));      // pushed right
    CHECK(rig.until([&]{ return !rig.calls.sticks.empty() && rig.calls.sticks.back()[0]>.9f; }));
    CHECK(rig.input->status().has_nunchuk);
    rig.remote->push(report(0,0,{0x82,0x82,0x9C},false,true,{128,128},0xFE));  // back to rest, Z held (active low)
    CHECK(rig.until([&]{ return rig.calls.sticks.back()[0]==0.f && !rig.calls.buttons.empty() && rig.calls.buttons.back().first==wii(WiiButton::nunchuk_z); }));
}
static void a_swing_arrives_as_motion() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    for(int i=0;i<30;++i) { rig.remote->push(report(0,0)); }
    rig.remote->push(report(0,0,{uint8_t(0x82+100),0x82,0x9C}));
    CHECK(rig.until([&]{ return !rig.calls.motions.empty(); }));
    CHECK(rig.calls.motions.back()[0]>100);
}
static void a_lost_remote_lets_go_of_everything_and_a_new_one_is_found() {
    Rig rig;
    auto second=std::make_shared<FakeRemote>();
    { std::lock_guard lock(rig.opener.m); rig.opener.remotes.push_back(second); }
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x04));
    CHECK(rig.until([&]{ return !rig.calls.buttons.empty(); }));
    rig.remote->disconnect();
    CHECK(rig.until([&]{ return rig.calls.resets==1 && !rig.connected(); }));
    CHECK(rig.until([&]{ return rig.connected() && rig.opener.calls>=2; }));  // the opener is asked again and finds the second remote
    CHECK(rig.input->changes()>=3);                                           // connected, lost, connected
}
static void a_drop_between_two_updates_still_releases_what_was_held() {
    Rig rig;
    auto second=std::make_shared<FakeRemote>();
    { std::lock_guard lock(rig.opener.m); rig.opener.remotes.push_back(second); }
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x04));                                       // B held
    CHECK(rig.until([&]{ return !rig.calls.buttons.empty(); }));
    rig.remote->disconnect();
    CHECK(wait_for([&]{ return !rig.connected(); }));
    CHECK(wait_for([&]{ return rig.connected() && rig.opener.calls>=2; }));    // lost and found again with no update() in between
    rig.update();
    CHECK(rig.calls.resets>=1);                                                // the held B is let go all the same
}
static void a_silent_remote_is_not_connected_and_is_let_go() {
    Rig rig(false);
    auto mute=std::make_shared<FakeRemote>(); mute->silent=true;               // a DolphinBar slot with no remote: it takes writes and says nothing
    { std::lock_guard lock(rig.opener.m); rig.opener.remotes.push_back(mute); }
    for(int i=0;i<100;++i) { std::this_thread::sleep_for(5ms); CHECK(!rig.connected()); }
    CHECK(wait_for([&]{ std::lock_guard lock(rig.opener.m); return rig.opener.calls>=2; },8000));   // given up after a silence, and looked for again
}
static void a_target_that_overflows_does_not_take_the_loop_down() {
    struct Overflowing : Calls { void host_button(PhysicalInput,bool) override { throw std::overflow_error("input action queue overflow"); } };
    Rig rig;
    Overflowing target;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x04));
    CHECK(wait_for([&]{ rig.input->update(&target,true,rig.settings); return rig.input->activity()>0; }));   // the throw stays inside update()
}
static void a_remote_that_refuses_writes_is_reported_and_the_loop_goes_on() {
    Rig rig(false);
    auto stubborn=std::make_shared<FakeRemote>(); stubborn->refuse_writes=true;
    { std::lock_guard lock(rig.opener.m); rig.opener.remotes.push_back(stubborn); }
    CHECK(wait_for([&]{ return !rig.input->status().error.empty(); }));
    CHECK(!rig.connected());
    rig.update();                                                              // the game's loop is not troubled
    CHECK(rig.calls.resets==0);
}
static void a_remote_that_cannot_be_opened_says_why() {
    Rig rig(false);
    { std::lock_guard lock(rig.opener.m); rig.opener.error="Wii Remote found but busy"; }
    CHECK(wait_for([&]{ return rig.input->status().error=="Wii Remote found but busy"; }));
}
static void an_opener_that_throws_is_survived() {
    WiimoteInput input([](std::string&) -> std::unique_ptr<WiiTransport> { throw std::runtime_error("boom"); });
    Calls calls; WiimoteSettings settings;
    input.update(&calls,true,settings);
    CHECK(wait_for([&]{ return input.status().error=="boom"; }));
}
static void inactive_and_disabled_let_go_once() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x04));
    CHECK(rig.until([&]{ return !rig.calls.buttons.empty(); }));
    rig.update(false); rig.update(false); CHECK(rig.calls.resets==1);           // a menu opened: one reset, not one per frame
    rig.remote->push(report(0x00,0x08));
    for(int i=0;i<40;++i) { rig.update(false); std::this_thread::sleep_for(5ms); }
    const auto before=rig.calls.buttons.size();
    rig.update(true); CHECK(rig.calls.buttons.size()==before);                  // what happened under the menu never turns into a press
}
static void a_disabled_remote_is_not_looked_for() {
    Rig rig(false,false);
    for(int i=0;i<30;++i) { rig.update(); std::this_thread::sleep_for(5ms); }
    { std::lock_guard lock(rig.opener.m); CHECK(rig.opener.calls==0); rig.opener.remotes.push_back(rig.remote); }
    rig.settings.enabled=true;
    CHECK(rig.until([&]{ return rig.connected(); }));
}
static void the_capture_sink_hears_buttons_even_when_inactive() {
    Rig rig;
    std::vector<std::pair<PhysicalInput,bool>> heard;
    rig.input->capture_sink([&](PhysicalInput input,bool down){ heard.emplace_back(input,down); });
    CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x10,0x00));                                         // plus
    CHECK(wait_for([&]{ rig.update(false); return !heard.empty(); }));
    CHECK(heard[0].first==wii(WiiButton::plus) && heard[0].second && rig.calls.buttons.empty());
}
static void the_motor_follows_rumble_and_survives_later_reports() {
    Rig rig;
    CHECK(wait_for([&]{ return rig.connected(); }));
    const auto last_write=[&] { std::lock_guard lock(rig.remote->m); return rig.remote->written.back(); };
    rig.input->rumble(true);
    CHECK(wait_for([&]{ return last_write()==std::vector<uint8_t>{0x10,0x01}; }));
    rig.input->rumble(true); std::this_thread::sleep_for(50ms);
    const auto before=rig.remote->writes();
    rig.remote->push({0x20,0,0,0x02,0,0,0xC0});                                  // a status report makes the thread set the data mode again: that write must not stop the motor
    CHECK(wait_for([&]{ return last_write()==std::vector<uint8_t>{0x12,0x05,0x37}; }));
    CHECK(rig.remote->writes()>before);
    rig.input->rumble(false);
    CHECK(wait_for([&]{ return last_write()==std::vector<uint8_t>{0x10,0x00}; }));
}
static void a_remote_found_while_rumbling_starts_with_the_motor_on() {
    Rig rig(false);
    rig.input->rumble(true);
    { std::lock_guard lock(rig.opener.m); rig.opener.remotes.push_back(rig.remote); }
    CHECK(wait_for([&]{ std::lock_guard lock(rig.remote->m); return !rig.remote->written.empty(); }));
    CHECK(wait_for([&]{ std::lock_guard lock(rig.remote->m); return rig.remote->written.back()[1]&1; }));   // every report it was sent has the motor bit, the last one included
    std::lock_guard lock(rig.remote->m);
    for(const auto& sent:rig.remote->written) CHECK(sent[1]&1);
}
static void the_windows_enumeration_survives_whatever_is_plugged_in() {
    // Whatever this machine has: no remote (null, no error), a remote (a transport), or one that will not open (null and a reason). Never a crash or a hang.
    std::string error;
    auto remote=wii_open_first(error);
    CHECK(remote || error.empty() || error.find("Wii Remote")!=std::string::npos);
    if(remote) { std::vector<uint8_t> report; remote->read(report,1); }
}
// Local two-player play: the host loop asks which remote pressed something before it decides whose target the remote feeds.
static void a_pressed_button_is_pending_until_update_hands_it_on() {
    Rig rig; CHECK(wait_for([&]{ return rig.connected(); }));
    CHECK(!rig.input->pending_press());
    rig.remote->push(report(0x00,0x08));   // A down
    CHECK(wait_for([&]{ return rig.input->pending_press(); }));
    rig.update();
    CHECK(!rig.input->pending_press() && rig.calls.buttons.size()==1);
    rig.remote->push(report(0x00,0x00));   // A up: a release is not a press
    CHECK(rig.until([&]{ return rig.calls.buttons.size()>=2; }));
    CHECK(!rig.input->pending_press());
}
static void a_press_that_arrives_after_the_hosts_look_is_not_handed_on_before_the_host_has_seen_it() {   // the reader thread may publish a press between the host's check and its update()
    Rig rig; CHECK(wait_for([&]{ return rig.connected(); }));
    rig.input->collect();                                                   // the host looks at this moment: nothing pending
    CHECK(!rig.input->pending_press());
    rig.remote->push(report(0x00,0x08));
    CHECK(wait_for([&]{ return rig.input->pending_press(); }));              // the press arrives after the look
    rig.update();
    CHECK(rig.calls.buttons.empty() && rig.input->pending_press());           // it is not handed to whatever target the host chose before it saw the press
    rig.input->collect(); rig.update();
    CHECK(rig.calls.buttons.size()==1 && !rig.input->pending_press());
}
static void the_device_id_is_the_transports_and_goes_with_the_link() {
    Rig rig; CHECK(rig.input->device_id().empty());
    CHECK(wait_for([&]{ return rig.connected(); }));
    CHECK(rig.input->device_id()=="remote-1");
    rig.remote->disconnect();
    CHECK(wait_for([&]{ return !rig.connected(); }));
    CHECK(rig.input->device_id().empty());
}
static void the_player_led_follows_set_player() {
    Rig rig; CHECK(wait_for([&]{ return rig.connected(); }));
    const auto led_written=[&](unsigned player) {
        const auto led=wii_led_report(player); std::lock_guard lock(rig.remote->m);
        return std::count(rig.remote->written.begin(),rig.remote->written.end(),std::vector<uint8_t>(led.begin(),led.end()));
    };
    CHECK(led_written(1)==1 && led_written(2)==0);                 // the set-up lights LED 1
    rig.input->set_player(1); CHECK(wait_for([&]{ return led_written(2)==1; }));
    rig.input->set_player(0); CHECK(wait_for([&]{ return led_written(1)==2; }));
}
static void release_lets_go_of_what_the_target_holds() {
    Rig rig; CHECK(wait_for([&]{ return rig.connected(); }));
    rig.remote->push(report(0x00,0x08));
    CHECK(rig.until([&]{ return rig.calls.buttons.size()>=1; }));
    CHECK(rig.calls.resets==0);
    rig.input->release(rig.calls); CHECK(rig.calls.resets==1);
    rig.input->release(rig.calls); CHECK(rig.calls.resets==1);       // nothing held now: nothing to let go of
}
static void the_claiming_enumeration_survives_whatever_is_plugged_in() {
    std::set<std::string> claimed; std::string error;
    auto first=wii_open_unclaimed(claimed,error);
    auto second=wii_open_unclaimed(claimed,error);                      // the remote the first call took is not offered again
    CHECK(!(first && second && first->id()==second->id()));
    CHECK(!first || (!first->id().empty() && claimed.count(first->id())==1));
    first.reset(); second.reset(); CHECK(claimed.empty());               // closing hands the claim back
}
int main() {
    the_windows_enumeration_survives_whatever_is_plugged_in();
    handshake_writes_the_init_sequence_in_order();
    buttons_flow_to_the_target_in_order();
    the_ir_pointer_flows_and_a_lost_camera_keeps_it();
    a_nunchuk_looking_report_without_the_extension_flag_is_ignored();
    the_nunchuk_stick_flows();
    a_swing_arrives_as_motion();
    a_lost_remote_lets_go_of_everything_and_a_new_one_is_found();
    a_drop_between_two_updates_still_releases_what_was_held();
    a_silent_remote_is_not_connected_and_is_let_go();
    a_target_that_overflows_does_not_take_the_loop_down();
    a_remote_that_refuses_writes_is_reported_and_the_loop_goes_on();
    a_remote_that_cannot_be_opened_says_why();
    an_opener_that_throws_is_survived();
    inactive_and_disabled_let_go_once();
    a_disabled_remote_is_not_looked_for();
    the_capture_sink_hears_buttons_even_when_inactive();
    the_motor_follows_rumble_and_survives_later_reports();
    a_remote_found_while_rumbling_starts_with_the_motor_on();
    a_pressed_button_is_pending_until_update_hands_it_on(); a_press_that_arrives_after_the_hosts_look_is_not_handed_on_before_the_host_has_seen_it(); the_device_id_is_the_transports_and_goes_with_the_link(); the_player_led_follows_set_player(); release_lets_go_of_what_the_target_holds();
    the_claiming_enumeration_survives_whatever_is_plugged_in();
}
