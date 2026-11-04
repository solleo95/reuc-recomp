#include "check.h"
#include "native_input.h"
#include "gamepad_input.h"
#include "virtual_pad.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
using namespace uc;
constexpr uint64_t kSecond=1000000000ull;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static bool near(float a,float b,float tolerance=2e-3f) { return std::fabs(a-b)<=tolerance; }
static void pump(GamepadInput& pads,NativeInput* input,bool active=true) {
    SDL_PumpEvents();
    SDL_Event event; while(SDL_PollEvent(&event)) pads.event(event,input);
    if(input)pads.update(*input,active);
}
struct Rig {
    GamepadInput pads; NativeInput input; uint64_t now=kSecond;
    Rig() { input.extent(1280,720); input.focus(true); input.context(InputContext::gameplay); input.consume_at(now); }
    void pump(bool active=true) { ::pump(pads,&input,active); }
    InputSample run(double seconds) { InputSample last; for(int i=0;i<int(seconds*20+.5);++i) {now+=kSecond/20; pump(); last=input.consume_at(now);} return last; }
};
static void family_from_type_table() {
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_XBOX360)==PadFamily::xbox && pad_family_from_type(SDL_GAMEPAD_TYPE_XBOXONE)==PadFamily::xbox);
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_PS3)==PadFamily::playstation && pad_family_from_type(SDL_GAMEPAD_TYPE_PS4)==PadFamily::playstation && pad_family_from_type(SDL_GAMEPAD_TYPE_PS5)==PadFamily::playstation);
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)==PadFamily::nintendo && pad_family_from_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)==PadFamily::nintendo);
    // A pad SDL cannot name (a GameSir or another XInput clone reports a standard or unknown type) has the Xbox layout and look: that is what almost every PC pad is.
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_STANDARD)==PadFamily::xbox && pad_family_from_type(SDL_GAMEPAD_TYPE_UNKNOWN)==PadFamily::xbox);
}
static void a_pad_appears_and_its_buttons_reach_the_input() {
    Rig rig; CHECK(!rig.pads.status().connected);
    Virtual pad("Virtual Test Pad"); rig.pump();
    const auto status=rig.pads.status(); CHECK(status.connected && status.name=="Virtual Test Pad" && status.has_gyro);
    pad.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(rig.input.consume_actions().pressed==bit(Action::reload));                     // X reloads
    pad.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.pump(); rig.input.consume_actions();
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(rig.input.consume_actions().held&bit(Action::interact));
}
static void a_firm_press_on_a_trackpad_is_a_button() {   // between a touch and the physical click
    Rig rig; Virtual pad; rig.pump();
    GamepadSettings settings;   // the right trackpad's press fires, the left one reloads
    settings.bindings[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::pad_button,kPadTrackpadPress+1},{}}};
    settings.bindings[size_t(Action::reload)]={{BindingKind::physical,{PhysicalKind::pad_button,kPadTrackpadPress},{}}};
    CHECK(!validate_gamepad(settings)); rig.input.pad_settings(settings);
    const auto fire=[&] { return (rig.input.consume_actions().held&bit(Action::fire))!=0; };
    pad.touch(1,true,.05f); rig.pump(); CHECK(!fire());                    // a resting finger is only a touch
    pad.touch(1,true,.15f); rig.pump(); CHECK(fire());                    // a medium press
    pad.touch(1,true,.08f); rig.pump(); CHECK(fire());                    // eased off a little: still pressed (hysteresis)
    pad.touch(1,true,.03f); rig.pump(); CHECK(!fire());                   // eased off further: released while the finger stays
    pad.touch(1,true,.08f); rig.pump(); CHECK(!fire());                   // back below the press level: not pressed again
    pad.touch(1,true,1.f); rig.pump(); CHECK(fire());                     // a firm press
    pad.touch(1,false,0); rig.pump(); CHECK(!fire());
    pad.touch(0,true,.9f); rig.pump(); CHECK(rig.input.consume_actions().pressed&bit(Action::reload));
    pad.touch(0,false,0); rig.pump(); rig.input.consume_actions();
}
static void triggers_press_at_half_with_a_hysteresis() {
    Rig rig; Virtual pad; rig.pump();
    const auto fire=[&]{ return (rig.input.consume_actions().held&bit(Action::fire))!=0; };
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.37f); rig.pump(); CHECK(!fire());
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.49f); rig.pump(); CHECK(!fire());        // not yet
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.52f); rig.pump(); CHECK(fire());         // pressed
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.43f); rig.pump(); CHECK(fire());         // held by the hysteresis
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.37f); rig.pump(); CHECK(!fire());        // released
}
static void sticks_move_the_pointer_and_the_camera() {
    Rig rig; Virtual pad; rig.pump();
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.pump();
    CHECK(near(rig.run(1).x,.8f));                                                       // the left stick aims: it stops at the screen's edge (aim .8), not past it
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,0); rig.pump(); rig.input.aim(0,0);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767); rig.pump();
    const auto after=rig.run(.5);
    CHECK(after.x==0 && rig.input.consume_actions().camera[0]==127);                     // the right stick is the camera, the pointer stays
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,0); pad.axis(SDL_GAMEPAD_AXIS_RIGHTY,-32768); rig.pump();
    CHECK(rig.input.consume_actions().camera[1]==127);                                   // up is positive
}
static void gyro_is_calibrated_and_moves_the_pointer() {
    Rig rig; Virtual pad; rig.pump();
    GamepadSettings settings; settings.gyro.mode=GyroMode::always; rig.input.pad_settings(settings);
    uint64_t t=kSecond;
    for(int i=0;i<130;++i) {pad.gyro(.02f,-.01f,t); t+=4000000; rig.pump();}             // resting offset while the half second of measuring passes
    const auto rest=rig.input.consume_at(rig.now+=kSecond/20);
    for(int i=0;i<20;++i) {pad.gyro(.02f,-.01f,t); t+=4000000; rig.pump();}              // still at rest after calibration: a pad on a desk
    const auto still=rig.input.consume_at(rig.now+=kSecond/20); CHECK(still.x==rest.x && still.y==rest.y);
    for(int i=0;i<3;++i) {pad.gyro(1.02f,-.01f,t); t+=4000000; rig.pump();}              // 1 rad/s left on top of the offset for three 4 ms samples: 24 counts
    const auto moved=rig.input.consume_at(rig.now+=kSecond/20);
    CHECK(near(moved.x-still.x,-2*24.f/1280,1e-3f) && near(moved.y,still.y,1e-3f));
}
static void a_removed_pad_is_released() {
    Rig rig; auto pad=std::make_unique<Virtual>(); rig.pump();
    pad->axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); pad->axis(SDL_GAMEPAD_AXIS_LEFTX,32767); pad->axis(SDL_GAMEPAD_AXIS_RIGHTX,32767); pad->button(SDL_GAMEPAD_BUTTON_SOUTH,true);
    rig.pump(); CHECK(rig.input.consume_actions().held&bit(Action::interact));
    pad.reset(); rig.pump(); rig.pump();                                                 // unplugged with everything held (SDL reports the removal on its next update)
    const auto frame=rig.input.consume_actions();
    CHECK(!rig.pads.status().connected);
    CHECK(frame.camera[0]==0);
    CHECK(frame.held==0);
    const auto at=rig.input.consume_at(rig.now+=kSecond/20); CHECK(rig.run(.5).x==at.x);   // the pointer is still
}
static void an_inactive_gamepad_feeds_nothing_and_resumes_cleanly() {
    Rig rig; Virtual pad; rig.pump();
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); pad.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    rig.input.consume_actions();                                                         // the reload pressed while active
    rig.pump(false);                                                                     // a menu opens with the stick pushed
    const auto frozen=rig.input.consume_at(rig.now+=kSecond/20);
    for(int i=0;i<20;++i) {rig.now+=kSecond/20; rig.pump(false);}
    CHECK(rig.input.consume_at(rig.now).x==frozen.x);
    pad.button(SDL_GAMEPAD_BUTTON_NORTH,true); rig.pump(false);                         // a button pressed behind the menu...
    rig.pump(true); CHECK(rig.input.consume_actions().held==0);                          // ...is not a press when the game returns, and neither is the one still held
    CHECK(rig.run(.3).x>frozen.x);                                                       // the stick still pushed continues to aim
}
static void the_capture_sink_sees_every_input_even_while_inactive() {
    Rig rig; Virtual pad; rig.pump();
    std::vector<std::pair<PhysicalInput,bool>> seen;
    rig.pads.capture_sink([&](PhysicalInput input,bool down){ seen.emplace_back(input,down); });
    pad.button(SDL_GAMEPAD_BUTTON_EAST,true); pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); pad.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768); rig.pump(false);
    const auto saw=[&](PhysicalInput input) { return std::count(seen.begin(),seen.end(),std::make_pair(input,true))==1; };
    CHECK(seen.size()==3 && saw({PhysicalKind::pad_button,1}) && saw({PhysicalKind::pad_axis,11}) && saw({PhysicalKind::pad_axis,2}));   // east, the right trigger, and the left stick up (axis 1 negative: code 1*2+0)
    CHECK(rig.input.consume_actions().held==0);
}
static void the_motors_drive_the_pad_in_use_and_the_other_pad() {
    Rig rig; rig.pads.rumble(1,1.f);                                   // no pad: nothing to do
    Virtual first("Pad One"); rig.pump();
    rig.pads.rumble(1,.5f); CHECK(first.rumbles.size()==1 && near(float(first.rumbles.back())/65535.f,.5f,.01f));
    rig.pads.rumble(1,.5f); CHECK(first.rumbles.size()==1);            // unchanged: not sent again
    rig.pads.rumble(0,.5f); CHECK(first.rumbles.size()==2 && first.rumbles.back()==0);
    Virtual second("Pad Two"); rig.pump();                             // the newest pad is the one in use: player 1's
    rig.pads.rumble(2,1.f);
    CHECK(first.rumbles.size()==3 && first.rumbles.back()==65535 && second.rumbles.empty());   // player 2's motor drives the other pad
    rig.pads.rumble(3,0.f);                                            // strength 0 is off
    CHECK(first.rumbles.back()==0 && second.rumbles.empty());
    rig.pads.rumble(1,1.f); CHECK(second.rumbles.size()==1 && second.rumbles.back()==65535);
    rig.pads.rumble(1,7.f); CHECK(second.rumbles.size()==1);           // above full strength is full strength
}
static void a_refused_rumble_is_tried_again_on_the_next_pass() {   // a dropped command would leave the pad silent (or running) until the 200 ms refresh
    Rig rig; Virtual pad; rig.pump();
    pad.refuse=true; rig.pads.rumble(1,1.f); CHECK(pad.rumbles.size()==1);
    rig.pads.rumble(1,1.f); CHECK(pad.rumbles.size()==2);              // tried again, not given up
    pad.refuse=false; rig.pads.rumble(1,1.f); CHECK(pad.rumbles.size()==3 && pad.rumbles.back()==65535);
    rig.pads.rumble(1,1.f); CHECK(pad.rumbles.size()==3);              // taken: not sent again
    pad.refuse=true; rig.pads.rumble(0,1.f); CHECK(pad.rumbles.size()==4);   // the stop is refused too...
    rig.pads.rumble(0,1.f); CHECK(pad.rumbles.size()==5);              // ...and sent again
    pad.refuse=false; rig.pads.rumble(0,1.f); CHECK(pad.rumbles.size()==6 && pad.rumbles.back()==0);
}
static void the_deck_report_is_the_loud_simple_rumble_command() {   // what the Deck's hidraw node gets; +10 dB on both motors and a repeat every 60 ms felt best on the Deck (SDL's own: +2 / 0 dB, once)
    const auto on=deck_rumble_report(65535), off=deck_rumble_report(0);
    CHECK(on.size()==65 && on[0]==0 && on[1]==0xEB && on[2]==0);                               // report id 0, ID_TRIGGER_RUMBLE_CMD, length left 0 as SDL does
    CHECK(on[3]==0 && on[4]==0 && on[5]==0);                                                    // rumble type 0, intensity HAPTIC_INTENSITY_SYSTEM
    CHECK(on[6]==0xFF && on[7]==0xFF && on[8]==0xFF && on[9]==0xFF && on[10]==10 && on[11]==10);   // both motors full, +10 dB each
    CHECK(deck_rumble_report(0x1234)[6]==0x34 && deck_rumble_report(0x1234)[7]==0x12 && deck_rumble_report(0x1234)[8]==0x34 && deck_rumble_report(0x1234)[9]==0x12);
    CHECK(off[1]==0xEB && off[6]==0 && off[7]==0 && off[8]==0 && off[9]==0 && off[10]==0 && off[11]==0);   // stopping asks for no gain either
}
static void a_deck_pad_gets_its_motor_command_again_every_60_ms() {
    Rig rig; Virtual deck("Steam Deck",0x28de,0x1205); rig.pump();
    CHECK(rig.pads.status().family==PadFamily::steam_deck);
    rig.pads.rumble(1,1.f,1000); CHECK(deck.rumbles.size()==1 && deck.rumbles.back()==65535);
    rig.pads.rumble(1,1.f,1030); CHECK(deck.rumbles.size()==1);                 // too soon
    rig.pads.rumble(1,1.f,1060); CHECK(deck.rumbles.size()==2 && deck.rumbles.back()==65534);   // the same command again, a hair different so that nothing drops it as unchanged
    rig.pads.rumble(1,1.f,1120); CHECK(deck.rumbles.size()==3 && deck.rumbles.back()==65535);
    rig.pads.rumble(0,1.f,1130); CHECK(deck.rumbles.size()==4 && deck.rumbles.back()==0);   // off at once
    rig.pads.rumble(0,1.f,1500); CHECK(deck.rumbles.size()==4);                 // and nothing more while quiet
}
static void any_other_pad_keeps_its_200_ms_refresh() {
    Rig rig; Virtual pad("Other Pad"); rig.pump();
    rig.pads.rumble(1,1.f,1000); CHECK(pad.rumbles.size()==1);
    rig.pads.rumble(1,1.f,1060); CHECK(pad.rumbles.size()==1);                  // SDL is not told again for nothing
    rig.pads.rumble(1,1.f,1250); CHECK(pad.rumbles.size()==1);                  // (the refresh of an unchanged value only extends SDL's timer)
}
// Local two-player play: one GamepadInput, two targets. The pad assigned with assign_second feeds the second target, every other pad the first.
struct TwoRig {
    GamepadInput pads; NativeInput first,second; uint64_t now=kSecond;
    TwoRig() { for(auto* input:{&first,&second}) { input->extent(1280,720); input->focus(true); input->context(InputContext::gameplay); input->consume_at(now); } }
    void pump() {
        SDL_PumpEvents();
        SDL_Event event; while(SDL_PollEvent(&event)) pads.event(event,&first,&second);
        pads.update(first,true,&second);
    }
    void run(double seconds) { for(int i=0;i<int(seconds*20+.5);++i) {now+=kSecond/20; pump(); first.consume_at(now); second.consume_at(now);} }
};
static void the_assigned_pad_feeds_the_second_target_only() {
    TwoRig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    std::vector<SDL_JoystickID> pressed;
    rig.pads.press_sink([&](SDL_JoystickID id) { pressed.push_back(id); if(id==b.id && !pressed.empty() && pressed.size()==1) rig.pads.assign_second(id); });   // the join press is routed to the second target too
    b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(pressed.size()==1 && pressed[0]==b.id);
    CHECK(rig.second.consume_actions().pressed==bit(Action::reload) && rig.first.consume_actions().pressed==0);
    b.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.pump(); rig.second.consume_actions();
    a.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload) && rig.second.consume_actions().pressed==0);   // pad A is still the first player's
    CHECK(pressed.size()==2 && pressed[1]==a.id);                                                              // and its press was reported too
    CHECK(rig.pads.status().name=="Pad A");                                                                  // the second player's pad never becomes the pad in use
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump(); CHECK(rig.pads.status().name=="Pad A");
}
static void a_deliberate_stick_push_or_a_trigger_pull_counts_as_use_but_drift_does_not() {   // a new pad's stick must raise the question too, not just its buttons; a pad on the desk with a drifting stick must not
    TwoRig rig; Virtual a("Pad A"); rig.pump();
    std::vector<SDL_JoystickID> pressed;
    rig.pads.press_sink([&](SDL_JoystickID id) { pressed.push_back(id); });
    a.axis(SDL_GAMEPAD_AXIS_LEFTX,9000); a.axis(SDL_GAMEPAD_AXIS_RIGHTY,-12000); rig.run(.3);
    CHECK(pressed.empty());                                                // drift, a quarter of the travel: nothing
    a.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.run(.3);
    CHECK(pressed.size()==1 && pressed[0]==a.id);                          // a stick pushed past half is use
    a.axis(SDL_GAMEPAD_AXIS_LEFTX,0); rig.run(.2);
    a.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,1.f); rig.run(.2);
    CHECK(pressed.size()==2);                                              // and so is a trigger
}
static void a_muted_pad_reaches_nobody_and_lets_go_of_what_it_held() {   // a device a question is open about must not play for the first player meanwhile
    TwoRig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); b.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.run(.3);
    CHECK(rig.first.consume_actions().held&bit(Action::interact));        // unknown, unmuted: it plays for the first player
    rig.pads.mute(b.id); rig.run(.1);
    CHECK(!(rig.first.consume_actions().held&bit(Action::interact)));      // muted: what it held is let go...
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,false); rig.run(.1); b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.run(.1);
    CHECK(rig.first.consume_actions().pressed==0);                         // ...and its presses reach nobody
    const float x=rig.first.consume_at(rig.now).x; rig.run(.5);
    CHECK(near(rig.first.consume_at(rig.now).x,x));                        // nor its stick
    a.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.run(.1);
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload));       // another pad is not muted
    rig.pads.mute(0); b.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.run(.1); b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.run(.1);
    CHECK(rig.first.consume_actions().pressed&bit(Action::reload));       // unmuted: it plays again
}
static void the_assigned_pads_sticks_move_the_second_pointer() {
    TwoRig rig; Virtual a("Pad A"),b("Pad B"); rig.pump(); rig.pads.assign_second(b.id);
    b.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.run(1);
    CHECK(rig.second.consume_at(rig.now).x>.2f && near(rig.first.consume_at(rig.now).x,0));
    b.axis(SDL_GAMEPAD_AXIS_LEFTX,0); a.axis(SDL_GAMEPAD_AXIS_LEFTX,-32767); rig.run(1);
    CHECK(rig.first.consume_at(rig.now).x<-.2f);
}
static void unassigning_or_unplugging_returns_everything_to_rest() {
    TwoRig rig; Virtual a("Pad A"); auto b=std::make_unique<Virtual>("Pad B"); rig.pump(); rig.pads.assign_second(b->id);
    b->button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump(); CHECK(rig.second.consume_actions().held&bit(Action::interact));
    b->detach(); rig.pump(); rig.run(.1);                                   // unplugged while held: the second target is released
    CHECK(!(rig.second.consume_actions().held&bit(Action::interact)));
    Virtual c("Pad C"); rig.pump(); rig.pads.assign_second(c.id);
    c.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump(); CHECK(rig.second.consume_actions().held&bit(Action::interact));
    rig.pads.assign_second(0); rig.pump();                                 // handed back: the second target lets go...
    CHECK(!(rig.second.consume_actions().held&bit(Action::interact)));
    c.button(SDL_GAMEPAD_BUTTON_SOUTH,false); rig.pump(); c.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(rig.first.consume_actions().held&bit(Action::interact));        // ...and the pad feeds the first one again
}
static void the_motors_follow_the_assigned_pad() {
    TwoRig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();                 // pad B is the newest, so the pad in use...
    rig.pads.assign_second(b.id);                                          // ...until it becomes the second player's: then pad A is
    rig.pads.rumble(1,1.f); CHECK(a.rumbles.size()==1 && a.rumbles.back()==65535 && b.rumbles.empty());
    rig.pads.rumble(3,1.f); CHECK(b.rumbles.size()==1 && b.rumbles.back()==65535);
    rig.pads.rumble(0,1.f); CHECK(a.rumbles.back()==0 && b.rumbles.back()==0);
}
int main() {
    // Only the virtual pads of this test: a gamepad plugged into the PC must not take part.
    for(const char* hint:{SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_WGI,SDL_HINT_JOYSTICK_DIRECTINPUT,SDL_HINT_XINPUT_ENABLED}) SDL_SetHint(hint,"0");
    family_from_type_table(); a_pad_appears_and_its_buttons_reach_the_input(); a_firm_press_on_a_trackpad_is_a_button(); triggers_press_at_half_with_a_hysteresis();
    sticks_move_the_pointer_and_the_camera(); gyro_is_calibrated_and_moves_the_pointer(); a_removed_pad_is_released();
    an_inactive_gamepad_feeds_nothing_and_resumes_cleanly(); the_capture_sink_sees_every_input_even_while_inactive();
    the_motors_drive_the_pad_in_use_and_the_other_pad(); a_refused_rumble_is_tried_again_on_the_next_pass();
    the_deck_report_is_the_loud_simple_rumble_command(); a_deck_pad_gets_its_motor_command_again_every_60_ms(); any_other_pad_keeps_its_200_ms_refresh();
    the_assigned_pad_feeds_the_second_target_only(); a_deliberate_stick_push_or_a_trigger_pull_counts_as_use_but_drift_does_not(); a_muted_pad_reaches_nobody_and_lets_go_of_what_it_held(); the_assigned_pads_sticks_move_the_second_pointer(); unassigning_or_unplugging_returns_everything_to_rest(); the_motors_follow_the_assigned_pad();
}
