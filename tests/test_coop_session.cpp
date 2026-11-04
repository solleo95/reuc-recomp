#include "check.h"
#include "coop_session.h"
#include "fake_wiimote.h"
#include "virtual_pad.h"
#include <cmath>
using namespace uc;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static bool near(float a,float b,float tolerance=2e-5f) { return std::fabs(a-b)<=tolerance; }
static PointerEvent button(uint64_t device,uint8_t which,bool down) { PointerEvent e; e.kind=PointerEvent::Kind::button; e.device=device; e.button=which; e.down=down; return e; }
static PointerEvent relative(uint64_t device,float x,float y) { PointerEvent e; e.kind=PointerEvent::Kind::relative; e.device=device; e.x=x; e.y=y; return e; }
static PointerEvent absolute(uint64_t device,float x,float y) { PointerEvent e; e.kind=PointerEvent::Kind::absolute; e.device=device; e.x=x; e.y=y; return e; }
// A pointer backend that is not there. `windows_like`: capturing a device takes every device over, as Raw Input does without the legacy messages.
struct FakePointers : PointerBackend {
    std::vector<PointerEvent> pending; std::vector<uint64_t> gone; std::vector<std::pair<uint64_t,bool>> captures; std::set<uint64_t> held;
    bool windows_like=false; std::string problem;
    void poll(std::vector<PointerEvent>& out,std::vector<uint64_t>& removed) override { out.insert(out.end(),pending.begin(),pending.end()); removed.insert(removed.end(),gone.begin(),gone.end()); pending.clear(); gone.clear(); }
    bool capture(uint64_t device,bool on) override { captures.emplace_back(device,on); if(on) held.insert(device); else held.erase(device); return true; }
    std::string status() const override { return problem; }
    bool all_devices() const override { return windows_like && !held.empty(); }
    bool last_absolute=false,last_playing=false; int hints=0;
    void hint(bool absolute,bool playing) override { last_absolute=absolute; last_playing=playing; ++hints; }
};
struct Rig {
    NativeInput first,second; GamepadInput pads; FakePointers pointers;
    std::unique_ptr<CoopSession> session; CoopSession::Frame frame;
    explicit Rig(bool enabled=true,std::array<WiimoteInput*,2> remotes={}) {
        for(auto* input:{&first,&second}) { input->extent(1280,720); input->context(InputContext::gameplay); input->focus(true); input->consume(); }
        input_hle_init(&first,&second); input_hle_set_second_connected(false);
        frame.in_gameplay=false; frame.captured=true; frame.window={1,1280,720,1};   // a second player is asked about in a menu: that is where the tests start
        session=std::make_unique<CoopSession>(CoopSession::Parts{first,second,&pads,remotes,&pointers},enabled);
        session->sensitivity(100);
    }
    void pump() {   // the loop's order: the session learns the state of the game first, then the events of the loop go through it
        session->tick(frame);
        SDL_PumpEvents();
        SDL_Event event; while(SDL_PollEvent(&event)) pads.event(event,&first,&second);
        pads.update(first,true,&second);
    }
    void menu() { frame.in_gameplay=false; frame.game_in_play=false; }   // a menu of the game: the title screen, the mission menus, a pause the game makes
    void game() { frame.in_gameplay=true; frame.game_in_play=true; }
    void menu_over_the_mission() { frame.in_gameplay=false; frame.game_in_play=true; }   // the port's menu opened over a mission: the game itself is still playing
    bool joined() { return session->status().joined; }
    bool asking() { return session->pending().asking; }
    void tap(Virtual& pad,SDL_GamepadButton which=SDL_GAMEPAD_BUTTON_SOUTH) { pad.button(which,true); pump(); pad.button(which,false); pump(); }
    void click(uint64_t device,uint8_t which=SDL_BUTTON_LEFT) { pointers.pending={button(device,which,true),button(device,which,false)}; pump(); }
};
static void the_first_device_used_is_player_ones_without_a_question() {
    Rig rig; Virtual a("Pad A"); rig.pump();
    rig.tap(a);
    CHECK(!rig.asking() && !rig.joined());
    rig.game(); rig.first.consume_actions(); rig.tap(a,SDL_GAMEPAD_BUTTON_WEST);
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload) && !rig.asking());   // and it plays for player one
}
static void another_device_in_a_menu_asks_and_the_answer_decides() {
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a);                                                               // player one's pad
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump(); b.button(SDL_GAMEPAD_BUTTON_SOUTH,false); rig.pump();
    CHECK(rig.asking() && rig.session->pending().kind==DeviceKind::gamepad && !rig.session->pending().replaces && !rig.joined() && !input_hle_second_connected());
    rig.session->answer_second();
    CHECK(rig.joined() && !rig.asking() && input_hle_second_connected() && rig.session->status().kind==DeviceKind::gamepad);
    rig.game(); rig.first.consume_actions(); rig.second.consume_actions();
    rig.tap(b,SDL_GAMEPAD_BUTTON_WEST);
    CHECK(rig.second.consume_actions().pressed==bit(Action::reload) && rig.first.consume_actions().pressed==0);   // pad B plays for player two
    rig.tap(a,SDL_GAMEPAD_BUTTON_WEST);
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload) && rig.second.consume_actions().pressed==0);   // pad A still for player one
}
static void answering_player_one_keeps_the_device_and_never_asks_again() {
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.tap(b);
    CHECK(rig.asking());
    rig.session->answer_first();
    CHECK(!rig.asking() && !rig.joined() && !input_hle_second_connected());
    rig.tap(b); CHECK(!rig.asking());                                          // the question is settled for this device
    rig.game(); rig.first.consume_actions(); rig.tap(b,SDL_GAMEPAD_BUTTON_WEST);
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload));          // and pad B plays for player one
}
static void a_stick_of_a_new_pad_asks_too_and_the_pad_waits_for_the_answer() {   // not only the buttons: pushing the stick of a new pad is use, and it must not move the first player's pointer meanwhile
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a);
    b.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.pump(); rig.pump();
    CHECK(rig.asking() && rig.session->pending().kind==DeviceKind::gamepad);
    rig.first.consume(); rig.first.consume_actions(); b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump(); rig.pump();
    CHECK(rig.first.consume_actions().pressed==0);                            // while the question is open the new pad plays for nobody
    rig.session->answer_first();
    b.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.pump(); b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(rig.first.consume_actions().pressed==bit(Action::reload));          // answered: it plays for player one
}
static void a_question_that_is_put_off_comes_back_with_the_next_press_and_the_pad_waits_again() {
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.tap(b); CHECK(rig.asking());
    rig.session->cancel_pending();
    CHECK(!rig.asking());
    rig.first.consume_actions(); b.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.pump(); b.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(rig.asking() && rig.first.consume_actions().pressed==0);             // nothing was decided: the press asks again and plays for nobody until it is answered
}
static void the_game_takes_no_second_player_in_the_middle_of_play() {
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.game();
    rig.tap(b);                                                                // an unknown pad in a mission: nothing asked, it feeds player one as before
    CHECK(!rig.asking() && !rig.joined());
    rig.menu(); rig.tap(b);                                                    // the question comes in a menu
    CHECK(rig.asking());
}
static void one_question_at_a_time_and_not_now_asks_again() {
    Rig rig; Virtual a("Pad A"),b("Pad B"),c("Pad C"); rig.pump();
    rig.tap(a); rig.tap(b);
    CHECK(rig.asking());
    rig.tap(c);                                                                // another new device while the question is open: ignored
    rig.session->answer_first();
    CHECK(!rig.asking());
    rig.tap(c); CHECK(rig.asking());                                           // c was not answered for: it asks now
    rig.session->cancel_pending();
    CHECK(!rig.asking() && !rig.joined());
    rig.tap(c); CHECK(rig.asking());                                           // "not now" settles nothing: the next press asks again
}
static void a_second_player_who_is_already_there_is_replaced_when_asked() {
    Rig rig; Virtual a("Pad A"),b("Pad B"),c("Pad C"); rig.pump();
    rig.tap(a); rig.tap(b); rig.session->answer_second(); CHECK(rig.joined());
    rig.tap(c);
    CHECK(rig.asking() && rig.session->pending().replaces);                    // the question says player 2 would be replaced
    rig.session->answer_second();
    CHECK(rig.joined() && !rig.asking());
    rig.game(); rig.first.consume_actions(); rig.second.consume_actions();
    rig.tap(c,SDL_GAMEPAD_BUTTON_WEST); CHECK(rig.second.consume_actions().pressed==bit(Action::reload));   // c is player two now
    rig.first.consume_actions();
    rig.tap(b,SDL_GAMEPAD_BUTTON_WEST);                                        // b was replaced: nobody has it, the next press asks (in the game too: it is a two-player game now)
    CHECK(rig.asking() && rig.first.consume_actions().pressed==0 && rig.session->pending().replaces);
}
static void a_keyboard_players_only_mouse_is_theirs() {   // a player on the keyboard aims with a mouse: touching it in a menu must not ask
    Rig rig; rig.session->note_keyboard();
    rig.pointers.pending={relative(4,5,5)}; rig.pump();
    rig.click(4);
    CHECK(!rig.asking() && !rig.joined() && rig.pointers.captures.empty());
}
static void with_a_keyboard_player_the_less_used_mouse_asks() {
    Rig rig; rig.session->note_keyboard();
    rig.pointers.pending={relative(4,50,0),relative(4,0,50),relative(5,2,0)}; rig.pump();   // mouse 4 aims, mouse 5 only twitched
    rig.click(4);
    CHECK(!rig.asking());                                                      // the busiest mouse is player one's
    rig.click(5);
    CHECK(rig.asking() && rig.session->pending().kind==DeviceKind::pointer);
    rig.session->answer_second();
    CHECK(rig.joined() && rig.pointers.captures.size()==1 && rig.pointers.captures[0]==std::make_pair(uint64_t(5),true));
}
static void a_pointer_that_joins_takes_the_click_back_from_player_one() {   // SDL still delivers the click to the first player, and its release never comes
    Rig rig; rig.frame.now_ns=1000000000ull;
    rig.click(4);                                                              // player one's mouse
    rig.first.host_button({PhysicalKind::mouse,SDL_BUTTON_RIGHT},true);        // the first player holds fire when the second player's device joins
    CHECK(rig.first.consume_actions().held&bit(Action::fire));
    rig.click(5); CHECK(rig.asking());
    rig.session->answer_second(); CHECK(rig.joined());
    CHECK(!(rig.first.consume_actions().held&bit(Action::fire)));              // let go
    SDL_Event click{}; click.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.common.timestamp=1100000000ull; CHECK(rig.session->swallows_mouse(click));   // SDL's copy of the click, 100 ms later: ignored
    click.common.timestamp=1300000000ull; CHECK(!rig.session->swallows_mouse(click));
    SDL_Event key{}; key.type=SDL_EVENT_KEY_DOWN; key.common.timestamp=1100000000ull; CHECK(!rig.session->swallows_mouse(key));
}
static void a_pointer_of_the_second_player_moves_only_the_second_pointer() {
    Rig rig; rig.click(4); rig.click(5); rig.session->answer_second(); CHECK(rig.joined());
    rig.game(); rig.first.consume(); rig.second.consume();
    rig.pointers.pending={relative(5,100,50)}; rig.pump();
    const auto moved=rig.second.consume(); CHECK(moved.x>0 && moved.y>0 && rig.first.consume().x==0);
    rig.pointers.pending={button(5,SDL_BUTTON_RIGHT,true)}; rig.pump();
    CHECK(rig.second.consume_actions().held&bit(Action::fire));
    rig.pointers.pending={relative(4,100,50)}; rig.pump();
    CHECK(rig.first.consume().x==0);                                           // the first player's mouse reaches the first player through SDL, not through here
}
static void a_disconnected_device_is_asked_about_again_in_a_menu_and_in_the_game() {   // it does not quietly become player one's
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.tap(b); rig.session->answer_second(); CHECK(rig.joined());
    rig.game(); rig.pump(); rig.menu_over_the_mission();                        // the port's menu over a mission: the disconnect button
    rig.session->release_second(); rig.pump();
    CHECK(!rig.joined() && input_hle_second_connected());                      // the mission goes on as a two-player game, the second pad idle
    CHECK(!(rig.second.consume_actions().held&bit(Action::interact)));
    rig.tap(b); CHECK(rig.asking() && !rig.joined());                          // in a menu: the window again
    rig.session->cancel_pending();
    rig.game(); rig.first.consume_actions();
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,false); rig.pump(); b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(rig.asking() && rig.first.consume_actions().pressed==0);             // and in the game: a two-player game was started, so a second player may come back; the press plays for nobody
    rig.session->answer_first(); rig.first.consume_actions();
    b.button(SDL_GAMEPAD_BUTTON_SOUTH,false); rig.pump(); b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(!rig.asking() && (rig.first.consume_actions().pressed&bit(Action::interact)));   // "Player 1 uses it": it plays for player one from now on
}
static void disconnecting_in_a_game_menu_makes_it_a_one_player_game_again() {   // the game stops seeing a second pad, so its second player (and the second reticle) can go
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.tap(b); rig.session->answer_second();
    CHECK(rig.joined() && input_hle_second_connected());
    rig.menu(); rig.session->release_second(); rig.pump();                     // a menu of the game (not a mission)
    CHECK(!rig.joined() && !input_hle_second_connected());
    rig.game(); rig.pump(); rig.tap(b);
    CHECK(!rig.asking());                                                      // no two-player mode any more: nothing is asked in the middle of play
    rig.menu(); rig.tap(b); CHECK(rig.asking());                               // in a menu the device asks, as any new one
    rig.session->answer_second(); CHECK(rig.joined() && input_hle_second_connected());   // and a second player can join again
}
static void in_a_game_that_never_had_a_second_player_nothing_is_asked() {
    Rig rig; Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.game();
    rig.tap(b); CHECK(!rig.asking());                                          // no two-player mode: the game takes no second player in the middle of play
}
static void leaving_keeps_the_second_inputs_focus_as_the_menu_and_pause_set_it() {   // uc_main sets the focus of both inputs: the session must not turn it back on under a menu
    Rig rig; rig.click(4); rig.click(5); rig.session->answer_second();
    CHECK(rig.joined() && rig.second.focused());
    rig.second.focus(false);                                                  // a menu opened
    rig.session->release_second(); CHECK(!rig.second.focused());
    rig.second.focus(true);
    rig.click(6); rig.session->answer_second(); CHECK(rig.joined());
    rig.session->release_second(); CHECK(rig.second.focused());
}
static void a_pointer_that_goes_away_releases_and_frees_the_place() {
    Rig rig; rig.click(4); rig.click(5); rig.session->answer_second(); CHECK(rig.joined());
    rig.game(); rig.pointers.pending={button(5,SDL_BUTTON_RIGHT,true)}; rig.pump();
    CHECK(rig.second.consume_actions().held&bit(Action::fire));
    rig.pointers.pending={button(5,SDL_BUTTON_RIGHT,false)}; rig.pointers.gone={5}; rig.pump();   // unplugged (the backend released the button first)
    CHECK(!rig.joined() && !rig.pointers.held.count(5));                       // and the grab was let go
    CHECK(!(rig.second.consume_actions().held&bit(Action::fire)));
    rig.menu(); rig.click(6); CHECK(rig.asking());
    rig.session->answer_second(); CHECK(rig.joined());                         // another device may take the place
    CHECK(input_hle_second_connected());                                       // the game still sees a second pad: it was never told it left
}
static void a_device_that_goes_away_while_it_is_asked_about_withdraws_the_question() {
    Rig rig; rig.click(4); rig.click(5); CHECK(rig.asking());
    rig.pointers.gone={5}; rig.pump();
    CHECK(!rig.asking());
}
static void a_disabled_session_never_asks() {
    Rig rig(false); Virtual a("Pad A"),b("Pad B"); rig.pump();
    rig.tap(a); rig.tap(b); rig.click(5);
    CHECK(!rig.asking() && !rig.joined() && !input_hle_second_connected() && rig.pointers.captures.empty());
    rig.game(); b.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(rig.first.consume_actions().held&bit(Action::interact));           // the pad is player one's, as before
}
static void when_the_backend_hands_over_every_mouse_player_one_comes_from_it_too() {   // Windows: a captured pointer takes the legacy mouse messages away from everybody
    Rig rig; rig.pointers.windows_like=true;
    rig.click(4);
    SDL_Event motion{}; motion.type=SDL_EVENT_MOUSE_MOTION;
    CHECK(!rig.session->swallows_mouse(motion));                               // nothing captured: SDL's mouse is the first player's
    rig.click(5); rig.session->answer_second(); CHECK(rig.joined());
    CHECK(rig.session->swallows_mouse(motion));                               // now SDL's copies are ignored...
    SDL_Event key{}; key.type=SDL_EVENT_KEY_DOWN; CHECK(!rig.session->swallows_mouse(key));
    rig.game(); rig.first.consume();
    rig.pointers.pending={relative(4,100,50)}; rig.pump();                    // ...and player one's mouse arrives through the backend
    const auto moved=rig.first.consume(); CHECK(near(moved.x,2*100.f/1280) && near(moved.y,2*50.f/720));
    rig.pointers.pending={button(4,SDL_BUTTON_RIGHT,true)}; rig.pump();
    CHECK(rig.first.consume_actions().pressed&bit(Action::fire));
    rig.session->release_second(); rig.pump();
    motion.common.timestamp=10000000000ull;                                     // 10 s later, long after the join click's copies
    CHECK(!rig.session->swallows_mouse(motion));                               // SDL's mouse is back
}
// The light gun rule: both guns' positions go through the same code as a single player's gun: exactly (2*position-1)*edge, from the middle of the screen to its edge.
static void two_absolute_guns_do_not_disturb_each_other_and_hit_what_they_aim_at() {
    Rig rig; rig.pointers.windows_like=true;
    rig.click(4); rig.click(5); rig.session->answer_second(); CHECK(rig.joined());
    rig.game();
    for(int step=0;step<=5;++step) {                                           // from the middle of the screen to its edge, both guns at once
        const float n=.5f+.1f*step;
        rig.pointers.pending={absolute(4,n,1-n),absolute(5,1-n,n)}; rig.pump();
        const auto one=rig.first.latest_aim(),two=rig.second.latest_aim();
        CHECK(near(one[0],(2*n-1)*kAimEdgeX) && near(one[1],(2*(1-n)-1)*kAimEdgeY));
        CHECK(near(two[0],(2*(1-n)-1)*kAimEdgeX) && near(two[1],(2*n-1)*kAimEdgeY));
    }
}
static void the_backend_is_told_what_the_game_is_doing() {
    Rig rig; rig.frame.pointer_absolute=true; rig.game(); rig.pump();
    CHECK(rig.pointers.hints>0 && rig.pointers.last_absolute && rig.pointers.last_playing);
    rig.frame.pointer_absolute=false; rig.menu(); rig.pump();
    CHECK(!rig.pointers.last_absolute && !rig.pointers.last_playing);
}
static void the_second_reader_waits_until_the_first_has_a_remote() {   // two readers racing for one remote could leave the first player's menu and bindings window without it
    Opener one,two; two.remotes.push_back(std::make_shared<FakeRemote>()); two.remotes[0]->name="remote-2";
    WiimoteInput reader_one(one.fn()),reader_two(two.fn());
    WiimoteSettings settings;
    Rig rig(true,{&reader_one,&reader_two});
    for(int i=0;i<60;++i) { rig.session->update_remotes(true,settings); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(two.calls==0 && !reader_two.status().connected);                    // the first reader has no remote: the second does not look yet
    one.remotes.push_back(std::make_shared<FakeRemote>()); one.remotes[0]->name="remote-1";
    CHECK(wait_for([&]{ rig.session->update_remotes(true,settings); return reader_one.status().connected; },8000));
    CHECK(wait_for([&]{ rig.session->update_remotes(true,settings); return two.calls>0; },8000));   // now it may look
}
static void a_second_remote_asks_and_lights_the_second_led_when_it_joins() {
    Opener one,two; one.remotes.push_back(std::make_shared<FakeRemote>()); one.remotes[0]->name="remote-1";
    two.remotes.push_back(std::make_shared<FakeRemote>()); two.remotes[0]->name="remote-2";
    auto remote_one=one.remotes[0],remote_two=two.remotes[0];
    WiimoteInput reader_one(one.fn()),reader_two(two.fn());
    WiimoteSettings settings;
    Rig rig(true,{&reader_one,&reader_two});
    rig.first.wiimote_settings(settings); rig.second.wiimote_settings(settings);
    const auto update=[&](bool active=true) { rig.session->update_remotes(active,settings); rig.session->tick(rig.frame); };
    update();
    CHECK(wait_for([&]{ update(); return reader_one.status().connected && reader_two.status().connected; }));
    remote_one->push(report(0x00,0x08)); remote_one->push(report(0x00,0x00));   // remote 1 presses A in a menu: the first device, player one's
    bool got=false;
    CHECK(wait_for([&]{ update(); got=got || rig.first.consume_actions().pressed!=0; return got; }));
    CHECK(!rig.asking());
    rig.first.consume_actions();
    remote_two->push(report(0x00,0x08)); remote_two->push(report(0x00,0x00));   // remote 2 presses A in a menu: asked
    CHECK(wait_for([&]{ update(); return rig.asking(); }));
    CHECK(rig.session->pending().kind==DeviceKind::wiimote && !rig.joined());
    CHECK(rig.first.consume_actions().pressed==0);                            // the press that raised the question, and everything after it, plays for nobody
    remote_two->push(report(0x00,0x04)); remote_two->push(report(0x00,0x00));
    for(int i=0;i<60;++i) { update(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(rig.first.consume_actions().pressed==0 && rig.asking());
    rig.session->answer_second(); CHECK(rig.joined());
    const auto led=wii_led_report(2);
    CHECK(wait_for([&]{ update(); std::lock_guard lock(remote_two->m); return std::count(remote_two->written.begin(),remote_two->written.end(),std::vector<uint8_t>(led.begin(),led.end()))==1; }));
    rig.game(); rig.first.consume_actions(); rig.second.consume_actions();
    remote_two->push(report(0x00,0x08));
    CHECK(wait_for([&]{ update(); return rig.second.consume_actions().pressed!=0; }));   // remote 2 plays for player two
    CHECK(rig.first.consume_actions().pressed==0);
    rig.session->release_second(); update();
    const auto led_one=wii_led_report(1);
    CHECK(wait_for([&]{ update(); std::lock_guard lock(remote_two->m); return std::count(remote_two->written.begin(),remote_two->written.end(),std::vector<uint8_t>(led_one.begin(),led_one.end()))==2; }));   // the set-up's LED 1 and the one that gives the remote back
}
int main() {
    for(const char* hint:{SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_WGI,SDL_HINT_JOYSTICK_DIRECTINPUT,SDL_HINT_XINPUT_ENABLED}) SDL_SetHint(hint,"0");
    the_first_device_used_is_player_ones_without_a_question(); another_device_in_a_menu_asks_and_the_answer_decides(); answering_player_one_keeps_the_device_and_never_asks_again();
    a_stick_of_a_new_pad_asks_too_and_the_pad_waits_for_the_answer(); a_question_that_is_put_off_comes_back_with_the_next_press_and_the_pad_waits_again();
    the_game_takes_no_second_player_in_the_middle_of_play(); one_question_at_a_time_and_not_now_asks_again(); a_second_player_who_is_already_there_is_replaced_when_asked();
    a_keyboard_players_only_mouse_is_theirs(); with_a_keyboard_player_the_less_used_mouse_asks(); a_pointer_that_joins_takes_the_click_back_from_player_one();
    a_pointer_of_the_second_player_moves_only_the_second_pointer(); a_disconnected_device_is_asked_about_again_in_a_menu_and_in_the_game(); in_a_game_that_never_had_a_second_player_nothing_is_asked();
    disconnecting_in_a_game_menu_makes_it_a_one_player_game_again();
    leaving_keeps_the_second_inputs_focus_as_the_menu_and_pause_set_it(); a_pointer_that_goes_away_releases_and_frees_the_place();
    a_device_that_goes_away_while_it_is_asked_about_withdraws_the_question(); a_disabled_session_never_asks();
    when_the_backend_hands_over_every_mouse_player_one_comes_from_it_too(); two_absolute_guns_do_not_disturb_each_other_and_hit_what_they_aim_at();
    the_backend_is_told_what_the_game_is_doing(); the_second_reader_waits_until_the_first_has_a_remote(); a_second_remote_asks_and_lights_the_second_led_when_it_joins();
}
