#include "check.h"
#include "input_bindings.h"
#include <cmath>
#include <limits>
#include <stdexcept>
using namespace uc;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static PhysicalInput key(uint16_t code) { return {PhysicalKind::key,code}; }
static PhysicalInput mouse(uint16_t code) { return {PhysicalKind::mouse,code}; }
static Binding physical(PhysicalInput input) { return {BindingKind::physical,input,{}}; }
static size_t downs(const ActionFrame& frame,Action action) {
    size_t n=0; for(size_t i=0;i<frame.count;++i) n+=frame.events[i].action==action && frame.events[i].down; return n;
}
static BindingResolver game() { BindingResolver r; r.context(InputContext::gameplay); r.focus(true); return r; }
template<class Exception,class F> static void fails(F run) {
    bool caught=false; try { run(); } catch(const Exception&) { caught=true; } CHECK(caught);
}
static PhysicalInput wii(uint16_t code) { return {PhysicalKind::wii_button,code}; }
static void wii_table_holds_only_wii_buttons() {
    CHECK(!validate_wii_bindings(default_wii_bindings()));
    auto bad=default_wii_bindings(); bad[size_t(Action::fire)].push_back(physical(key(9)));
    CHECK(validate_wii_bindings(bad));                                  // a key does not belong in the Wii Remote table
    bad=default_wii_bindings(); bad[size_t(Action::fire)].push_back(physical({PhysicalKind::pad_button,0}));
    CHECK(validate_wii_bindings(bad));
    bad=default_wii_bindings(); bad[size_t(Action::fire)].push_back(physical(wii(kWiiButtonCount)));
    CHECK(validate_wii_bindings(bad));                                  // there are kWiiButtonCount buttons
    auto keyboard=default_bindings(); keyboard[size_t(Action::fire)].push_back(physical(wii(7)));
    CHECK(validate_bindings(keyboard));                                 // and a Wii button does not belong in the keyboard one
    auto pad=default_pad_bindings(); pad[size_t(Action::fire)].push_back(physical(wii(7)));
    CHECK(validate_pad_bindings(pad));
}
static void merge_keeps_every_device() {
    auto keyboard=default_bindings(),pad=default_pad_bindings(),wii_table=default_wii_bindings();
    const auto merged=merge_bindings(keyboard,pad,wii_table);
    for(auto input:{physical(mouse(3)),physical({PhysicalKind::pad_axis,11}),physical(wii(7))})
        CHECK(std::find(merged[size_t(Action::fire)].begin(),merged[size_t(Action::fire)].end(),input)!=merged[size_t(Action::fire)].end());
    CHECK(merge_bindings(keyboard,pad)==merge_bindings(keyboard,pad,Bindings{}));   // the Wii table is optional
}
static void wii_buttons_drive_actions_and_do_not_collide_with_pads() {
    auto r=game(); auto binds=merge_bindings(default_bindings(),default_pad_bindings(),default_wii_bindings()); r.replace(binds);
    r.button(wii(7),true); auto f=r.consume();                          // B: the trigger
    CHECK(f.held==bit(Action::fire) && downs(f,Action::fire)==1);
    r.button({PhysicalKind::pad_button,0},true); r.button(wii(0),true); r.consume();   // pad south (interact) and Wii "left"
    r.release_wii();                                                    // the remote went away: only its inputs let go
    f=r.consume();
    CHECK(f.released&bit(Action::fire));                                 // B was released
    CHECK(r.down({PhysicalKind::pad_button,0}) && !r.down(wii(0)) && !r.down(wii(7)));
    r.release_pad(); CHECK(!r.down({PhysicalKind::pad_button,0}));
    r.button(wii(12),true); r.button(wii(11),true);                     // the nunchuk's C and Z: the last two slots work
    CHECK(r.down(wii(12)) && r.down(wii(11)));
    fails<std::invalid_argument>([&]{ r.button(wii(kWiiButtonCount),true); });
}
static void defaults_and_context() {
    auto r=game(); r.button(mouse(1),true); auto f=r.consume();
    CHECK(f.held==bit(Action::interact) && f.pressed==bit(Action::interact));
    r.button(mouse(1),false); r.consume(); r.button(mouse(3),true); f=r.consume();
    CHECK(f.held==bit(Action::fire) && downs(f,Action::fire)==1);
    r.button(mouse(3),false); r.consume(); r.button(key(8),true); CHECK(r.consume().pressed==0); // E is free.
    r.button(key(21),true); CHECK(downs(r.consume(),Action::reload)==1);
    r.button(key(10),true); CHECK(downs(r.consume(),Action::grenade)==1);
    r.button(key(41),true); CHECK(downs(r.consume(),Action::pause)==1);
    r.context(InputContext::menu); r.consume();
    r.button(mouse(1),true); f=r.consume(); CHECK(f.pressed==bit(Action::confirm));
    r.button(mouse(3),true); f=r.consume(); CHECK(f.pressed==bit(Action::back));
    r.button(key(40),true); CHECK(r.consume().held&bit(Action::confirm));
    r.context(InputContext::inactive); r.consume(); r.button(key(226),true);
    CHECK(downs(r.consume(),Action::release_cursor)==1);
}
static void multiple_sources_and_short_click() {
    auto r=game(); auto binds=default_bindings(); binds[size_t(Action::fire)].push_back(physical(key(9))); r.replace(binds);
    r.button(mouse(3),true); r.button(key(9),true); CHECK(downs(r.consume(),Action::fire)==1);
    r.button(mouse(3),false); auto f=r.consume(); CHECK(f.held&bit(Action::fire)); CHECK(!(f.released&bit(Action::fire)));
    r.button(key(9),false); f=r.consume(); CHECK(f.held==0 && f.released==bit(Action::fire));
    r.button(mouse(3),true); r.button(mouse(3),false); f=r.consume();
    CHECK(f.held==0 && f.pressed==bit(Action::fire) && f.released==bit(Action::fire));
    CHECK(r.consume().pressed==0);
}
static void ordered_chord_and_dedup() {
    auto r=game(); r.button(mouse(1),true); r.consume(); r.button(mouse(3),true); auto f=r.consume();
    CHECK(downs(f,Action::grenade)==1 && downs(f,Action::fire)==0 && !(f.held&bit(Action::fire)));
    r.button(mouse(3),true); CHECK(r.consume().pressed==0);
    r.button(mouse(1),false); CHECK(downs(r.consume(),Action::fire)==0); // No surprise shot on releasing modifier.
    r.button(mouse(3),false); r.consume();
    r.button(mouse(3),true); r.button(mouse(1),true); f=r.consume();
    CHECK(downs(f,Action::fire)==1 && downs(f,Action::grenade)==0);
    r=game(); r.button(mouse(1),true); r.button(key(10),true); r.button(mouse(3),true); f=r.consume();
    CHECK(downs(f,Action::grenade)==1 && downs(f,Action::fire)==0);
    r=game(); r.button(mouse(1),true); r.button(mouse(3),true); r.button(key(10),true); f=r.consume();
    CHECK(downs(f,Action::grenade)==1 && downs(f,Action::fire)==0);
}
static void clear_special_and_remap() {
    auto binds=default_bindings(); binds[size_t(Action::grenade)].pop_back();
    auto r=game(); r.replace(binds); r.button(mouse(1),true); r.button(mouse(3),true); auto f=r.consume();
    CHECK(downs(f,Action::fire)==1 && downs(f,Action::grenade)==0);
    binds[size_t(Action::grenade)].clear(); binds[size_t(Action::knife)].clear(); r.replace(binds); r.consume();
    r.button(key(10),true); r.button(key(25),true); r.motion(700,0,1000000000ull); f=r.consume();
    CHECK(!(f.pressed&(bit(Action::grenade)|bit(Action::knife))));
    binds=default_bindings(); binds[size_t(Action::interact)]={physical(key(27))}; binds[size_t(Action::fire)]={physical(key(9))};
    r=game(); r.replace(binds); r.button(key(27),true); r.button(key(9),true); f=r.consume();
    CHECK(downs(f,Action::grenade)==1 && downs(f,Action::fire)==0);
}
static void wheel_keyboard_repeat_and_chords() {
    auto r=game(); r.wheel(2); r.wheel(-1); auto f=r.consume();
    CHECK(downs(f,Action::weapon_previous)==2 && downs(f,Action::weapon_next)==1 && f.held==0);
    r.button(key(26),true); CHECK(downs(r.consume(),Action::weapon_previous)==1);
    r.button(key(26),true); CHECK(r.consume().pressed==0);
    r.button(key(22),true); CHECK(downs(r.consume(),Action::weapon_next)==1);
    r.button(key(8),true); CHECK(r.consume().pressed==0); r.button(key(224),true);
    CHECK(downs(r.consume(),Action::screenshot)==1); r.button(key(224),false); r.consume();
    r.button(key(228),true); CHECK(downs(r.consume(),Action::screenshot)==1);
}
static void wheel_pulse_preserves_keyboard_hold() {
    auto r=game(); r.button(key(26),true); auto f=r.consume();
    CHECK(f.held==bit(Action::weapon_previous) && !f.events[0].pulse);
    r.wheel(1); f=r.consume();
    CHECK(f.held==bit(Action::weapon_previous) && f.count==1);
    CHECK(f.events[0].action==Action::weapon_previous && f.events[0].down && f.events[0].pulse);
    CHECK(!(f.released&bit(Action::weapon_previous)));
    r.button(key(26),false); f=r.consume();
    CHECK(f.held==0 && f.count==1 && !f.events[0].down && !f.events[0].pulse);
    r.wheel(1); f=r.consume();
    CHECK(f.held==0 && f.count==2 && f.events[0].pulse && f.events[1].pulse);
}
static void replace_focus_and_context_cancel() {
    auto r=game(); r.button(mouse(3),true); r.consume(); r.replace(default_bindings()); auto f=r.consume();
    CHECK(f.held==0 && f.pressed==0 && f.released==bit(Action::fire));
    r.button(mouse(3),true); CHECK(r.consume().held==0);
    r.button(mouse(3),false); r.button(mouse(3),true); CHECK(r.consume().held==bit(Action::fire));
    r.button(key(21),true); r.focus(false); f=r.consume(); CHECK(f.held==0 && f.pressed==0 && downs(f,Action::reload)==0);
    r.focus(true); r.button(mouse(3),true); CHECK(r.consume().held==0);
    r.button(mouse(3),false); r.button(mouse(3),true); r.consume(); r.context(InputContext::menu); f=r.consume();
    CHECK(f.held==0 && f.pressed==0); r.button(mouse(3),true); CHECK(r.consume().pressed==0);
    r.button(mouse(3),false); r.button(mouse(3),true); CHECK(downs(r.consume(),Action::back)==1);
    r=game(); r.context(InputContext::qte); r.button(mouse(1),true); r.button(mouse(3),true); f=r.consume();
    CHECK(downs(f,Action::interact)==1 && downs(f,Action::fire)==1 && downs(f,Action::grenade)==0);
}
static void knife_key_direction_and_mouse_threshold() {
    // V swings from the lower left to the upper right (screen y points down), whatever the mouse does.
    auto r=game(); r.button(key(25),true); auto f=r.consume();
    CHECK(downs(f,Action::knife)==1 && std::fabs(f.events[0].direction_x-0.70710678f)<1e-4f && std::fabs(f.events[0].direction_y+0.70710678f)<1e-4f);
    // A held interact button needs a FAST movement. Slow drags never swing, however far they go: 1 count/ms, 5 counts per 8 ms, 20 counts per 20 ms.
    auto m=game(); m.button({PhysicalKind::mouse,1},true); m.consume();
    for(int i=0;i<3000;++i) {m.motion(1,0,1000000000ull+i*1000000ull); CHECK(downs(m.consume(),Action::knife)==0);}
    for(int i=0;i<500;++i) {m.motion(5,0,5000000000ull+i*8000000ull); CHECK(downs(m.consume(),Action::knife)==0);}
    for(int i=0;i<200;++i) {m.motion(20,0,9000000000ull+i*20000000ull); CHECK(downs(m.consume(),Action::knife)==0);}
    // A tiny quick twitch is below the length floor; a pause starts a new window, so two slow halves never add up.
    m.motion(8,0,20000000000ull); CHECK(downs(m.consume(),Action::knife)==0);
    m.motion(8,0,21000000000ull); m.motion(8,0,21300000000ull); CHECK(downs(m.consume(),Action::knife)==0);
    // A quick stroke swings along its accumulated direction, whether it arrives in one event or several within the window.
    auto s=game(); s.button({PhysicalKind::mouse,1},true); s.consume();
    s.motion(30,0,2000000000ull); s.motion(0,-60,2005000000ull); f=s.consume();
    CHECK(downs(f,Action::knife)==1);
    for(size_t i=0;i<f.count;++i) if(f.events[i].action==Action::knife && f.events[i].down) {
        CHECK(std::fabs(f.events[i].direction_x-30/std::hypot(30.f,60.f))<1e-3f && std::fabs(f.events[i].direction_y+60/std::hypot(30.f,60.f))<1e-3f);
    }
    auto t=game(); t.button({PhysicalKind::mouse,1},true); t.consume();
    t.motion(40,0,3000000000ull); CHECK(downs(t.consume(),Action::knife)==1);
    // One stroke is one swing: slow follow-up motion in the next frame starts nothing.
    s.motion(0,-1,2040000000ull); CHECK(downs(s.consume(),Action::knife)==0);
    // Without a clock there is no speed.
    auto n=game(); n.button({PhysicalKind::mouse,1},true); n.consume(); n.motion(500,0); CHECK(downs(n.consume(),Action::knife)==0);
}
static void knife_direction_bursts_and_overflow() {
    auto r=game(); r.motion(3,4); CHECK(downs(r.consume(),Action::knife)==0);
    r.button(key(25),true); auto f=r.consume(); CHECK(f.held==bit(Action::knife));
    CHECK(f.count==1 && f.events[0].direction_x==kKnifeKeyDirectionX && f.events[0].direction_y==kKnifeKeyDirectionY);
    CHECK(r.consume().held==bit(Action::knife)); // Game cooldown, not resolver, repeats held V.
    r.button(key(25),false); r.consume(); r.button(mouse(1),true); r.consume();
    r.motion(240,320,5000000000ull); f=r.consume(); CHECK(downs(f,Action::knife)==1);
    CHECK(std::abs(f.events[0].direction_x-.6f)<1e-6f && std::abs(f.events[0].direction_y-.8f)<1e-6f);
    for(int i=0;i<10000;++i)r.motion(1,0,6000000000ull+i*1000ull); f=r.consume(); CHECK(downs(f,Action::knife)==1 && f.events[0].direction_x==1);
    r.motion(240,320,7000000000ull); r.motion(-240,-320,7001000000ull); f=r.consume(); CHECK(downs(f,Action::knife)==0 && !(f.pressed&bit(Action::knife)));
    r.motion(400,0,8000000000ull); r.button(key(21),true); r.motion(0,400,8001000000ull); f=r.consume();
    CHECK(downs(f,Action::knife)==2 && downs(f,Action::reload)==1);
    fails<std::overflow_error>([&]{r.wheel(129);}); f=r.consume(); CHECK(f.held==0 && f.pressed==0 && f.count==0);
    fails<std::invalid_argument>([&]{r.motion(std::numeric_limits<float>::infinity(),0);});
    fails<std::overflow_error>([&]{r.wheel(std::numeric_limits<int>::min());});
}
static void validation_preserves_configuration() {
    auto valid=default_bindings(); CHECK(!validate_bindings(valid));
    auto invalid=valid; invalid[0].push_back(invalid[0][0]); CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0].input=key(0); CHECK(validate_bindings(invalid));
    invalid[0][0].input=key(512); CHECK(validate_bindings(invalid));
    invalid[0][0].input={PhysicalKind(255),1}; CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0].kind=BindingKind(255); CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0].modifiers={{PhysicalKind::wheel,1}}; CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0].modifiers={key(224),key(224)}; CHECK(validate_bindings(invalid));
    invalid=valid; auto duplicate=physical(key(9)); duplicate.modifiers={key(224),key(225)};
    invalid[0]={duplicate}; duplicate.modifiers={key(225),key(224)}; invalid[0].push_back(duplicate); CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0].modifiers={key(224),key(225),key(226),key(227),key(228)}; CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0].assign(65,physical(key(9))); CHECK(validate_bindings(invalid));
    invalid=valid; invalid[0][0]={BindingKind::interact_fire,{}, {}}; CHECK(validate_bindings(invalid));
    auto r=game(); r.button(mouse(3),true); r.consume(); fails<std::invalid_argument>([&]{r.replace(invalid);});
    CHECK(r.consume().held==bit(Action::fire));
    fails<std::invalid_argument>([&]{r.button({PhysicalKind::wheel,1},true);});
    fails<std::invalid_argument>([&]{r.context(InputContext(255));});
}
static void qte_mouse_jerks() {
    auto r=game(); r.motion(32,0,1000000000); r.motion(-32,0,1030000000);
    CHECK(downs(r.consume(),Action::qte_shake)==0);
    r.context(InputContext::qte); r.qte_window(true); r.motion(32,0,2000000000); r.motion(-32,0,2030000000);
    CHECK(downs(r.consume(),Action::qte_shake)==1); // Bare mouse, real timed reversal.
    r.motion(32,0,2060000000); CHECK(downs(r.consume(),Action::qte_shake)==1);
    r.motion(-32,0,2260000000); CHECK(downs(r.consume(),Action::qte_shake)==0); // Slow movement.
    r.focus(false); r.focus(true); r.motion(32,0,3000000000); r.consume();
    r.motion(-32,0,3030000000); r.context(InputContext::menu);
    CHECK(downs(r.consume(),Action::qte_shake)==0);
    r.context(InputContext::qte); auto binds=default_bindings(); binds[size_t(Action::qte_shake)].clear(); r.replace(binds);
    r.motion(32,0,4000000000); r.motion(-32,0,4030000000); CHECK(downs(r.consume(),Action::qte_shake)==0);
    auto a=game(),b=game(); a.context(InputContext::qte); b.context(InputContext::qte);a.qte_window(true);b.qte_window(true);
    size_t separate=0;
    for(unsigned i=0;i<6;++i) {a.motion(i&1 ? -32.f : 32.f,0,5000000000ull+i*30000000ull); b.motion(i&1 ? -32.f : 32.f,0,5000000000ull+i*30000000ull);separate+=downs(a.consume(),Action::qte_shake);}
    CHECK(separate==5 && downs(b.consume(),Action::qte_shake)==separate); // Consume/render cadence independent.
    auto mixed=game();mixed.context(InputContext::qte);mixed.qte_window(true);mixed.button(mouse(1),true);
    mixed.motion(32,0,6000000000);mixed.motion(-32,0,6030000000);
    CHECK(downs(mixed.consume(),Action::qte_shake)==1); // Knife coalescing must not discard a QTE step.
}
// A hand shake is a sine in position: speed passes through zero at every turn, so a detector that measures speed from the turn misses moderate shakes.
static size_t sine_shake_steps(double amplitude,double hz,double seconds,unsigned report_hz) {
    auto r=game(); r.context(InputContext::qte); r.qte_window(true);
    const double pi=3.14159265358979323846; double previous=0,pending=0; size_t steps=0;
    for(unsigned i=1;i<=unsigned(seconds*report_hz);++i) {
        const double position=amplitude*std::sin(2*pi*hz*i/report_hz); pending+=position-previous; previous=position;
        const double counts=std::trunc(pending); pending-=counts;   // a mouse reports whole counts
        if(counts!=0) {r.motion(float(counts),0,1000000000ull+uint64_t(i)*1000000000ull/report_hz);steps+=downs(r.consume(),Action::qte_shake);}
    }
    return steps;
}
static void qte_mouse_shake_profiles() {
    for(unsigned rate:{1000u,125u}) for(double hz:{3.0,5.0,8.0}) for(double amplitude:{30.0,60.0,120.0,250.0,500.0}) {
        const size_t reversals=size_t(2*hz*2)-1,steps=sine_shake_steps(amplitude,hz,2,rate);
        CHECK(steps>=reversals*8/10 && steps<=reversals*12/10);   // about one step per reversal, whatever the amplitude, frequency or report rate
    }
    CHECK(sine_shake_steps(60,.5,4,1000)==0 && sine_shake_steps(60,1,4,1000)==0);   // a slow wave is aiming, not shaking
    CHECK(sine_shake_steps(15,8,4,1000)==0 && sine_shake_steps(10,10,4,1000)==0);   // tremor-sized jitter
}
static PhysicalInput pad(uint16_t code) { return {PhysicalKind::pad_button,code}; }
static PhysicalInput axis(uint16_t code) { return {PhysicalKind::pad_axis,code}; }
// SDL gamepad codes: buttons south 0, east 1, west 2, north 3, back 4, start 6, left stick 7, right stick 8, left shoulder 9, right shoulder 10; axis codes are axis*2+1 (left trigger 9, right trigger 11).
static BindingResolver pad_game(InputContext context=InputContext::gameplay) {
    BindingResolver r; r.replace(merge_bindings(default_bindings(),default_pad_bindings())); r.context(context); r.focus(true); return r;
}
static void pad_tables_validate_and_stay_separate() {
    CHECK(!validate_pad_bindings(default_pad_bindings()));
    CHECK(!validate_bindings(default_bindings()));
    auto keyboard=default_bindings(); keyboard[size_t(Action::fire)].push_back(physical(pad(0))); CHECK(validate_bindings(keyboard));
    keyboard=default_bindings(); keyboard[size_t(Action::fire)][0].modifiers={axis(9)}; CHECK(validate_bindings(keyboard));
    auto gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back(physical(key(9))); CHECK(validate_pad_bindings(gamepad));
    gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back(physical(mouse(1))); CHECK(validate_pad_bindings(gamepad));
    gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back(physical(pad(32))); CHECK(validate_pad_bindings(gamepad));
    gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back(physical(axis(16))); CHECK(validate_pad_bindings(gamepad));
    gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back({BindingKind::physical,pad(5),{axis(9)}}); CHECK(!validate_pad_bindings(gamepad));
    gamepad=default_pad_bindings(); gamepad[size_t(Action::fire)].push_back(gamepad[size_t(Action::fire)][0]); CHECK(validate_pad_bindings(gamepad));
    const auto defaults=default_pad_bindings();
    CHECK(defaults[size_t(Action::fire)].size()==1 && defaults[size_t(Action::fire)][0].input==axis(11));
    CHECK(defaults[size_t(Action::interact)][0].input==pad(0) && defaults[size_t(Action::reload)][0].input==pad(2));
    CHECK(defaults[size_t(Action::weapon_previous)][0].input==pad(9) && defaults[size_t(Action::weapon_next)][0].input==pad(10));
    CHECK(defaults[size_t(Action::recenter)][0].input==pad(7) && defaults[size_t(Action::screenshot)][0].input==pad(4));
    CHECK(defaults[size_t(Action::release_cursor)].empty() && defaults[size_t(Action::camera_up)].empty());
    CHECK(defaults[size_t(Action::menu)].empty());   // the menu is a key of the keyboard table
    auto menu_table=default_bindings();
    CHECK((menu_table[size_t(Action::menu)].size()==1 && menu_table[size_t(Action::menu)][0].input==PhysicalInput{PhysicalKind::key,uint16_t(73)} && !validate_bindings(menu_table)));   // Insert
    menu_table[size_t(Action::menu)]={{BindingKind::physical,{PhysicalKind::key,67},{}}}; CHECK(!validate_bindings(menu_table));                                                    // F10
    menu_table[size_t(Action::menu)]={{BindingKind::physical,{PhysicalKind::mouse,4},{}}}; CHECK(validate_bindings(menu_table));                                                    // a mouse button is no host key
    menu_table[size_t(Action::menu)]={{BindingKind::physical,{PhysicalKind::key,67},{{PhysicalKind::key,224}}}}; CHECK(validate_bindings(menu_table));                              // nor a chord
    menu_table[size_t(Action::menu)].clear(); CHECK(!validate_bindings(menu_table));                                                                                                // unbound: the game falls back to Insert
    gamepad=default_pad_bindings(); gamepad[size_t(Action::menu)].push_back(physical(pad(5))); CHECK(validate_pad_bindings(gamepad));   // the gamepad table has no menu key
}
static void merge_appends_pad_entries_without_duplicates() {
    const auto merged=merge_bindings(default_bindings(),default_pad_bindings());
    const auto keyboard=default_bindings();
    CHECK(merged[size_t(Action::interact)].size()==keyboard[size_t(Action::interact)].size()+1);
    CHECK(merged[size_t(Action::interact)].front()==keyboard[size_t(Action::interact)].front() && merged[size_t(Action::interact)].back().input==pad(0));
    size_t specials=0; for(const auto& b:merged[size_t(Action::grenade)]) specials+=b.kind==BindingKind::interact_fire; CHECK(specials==1);   // the pad table has none: the keyboard's entry serves every device
    auto both=default_pad_bindings(); both[size_t(Action::grenade)].push_back({BindingKind::interact_fire,{},{}}); CHECK(!validate_pad_bindings(both));
    const auto twice=merge_bindings(keyboard,both);
    specials=0; for(const auto& b:twice[size_t(Action::grenade)]) specials+=b.kind==BindingKind::interact_fire; CHECK(specials==1);   // listed twice, merged once
    auto cleared=keyboard; cleared[size_t(Action::grenade)].pop_back();                                  // the chord switched off in the keyboard table is off for the gamepad too
    auto off=pad_game(); off.replace(merge_bindings(cleared,default_pad_bindings())); off.button(pad(0),true); off.consume(); off.button(axis(11),true);
    auto after=off.consume(); CHECK(downs(after,Action::grenade)==0 && downs(after,Action::fire)==1);
    CHECK(!validate_bindings(keyboard));
    fails<std::invalid_argument>([&]{BindingResolver r; auto bad=merged; bad[size_t(Action::fire)].push_back(bad[size_t(Action::fire)].front()); r.replace(bad);});
}
static void pad_buttons_drive_actions_by_context() {
    auto r=pad_game(); r.button(axis(11),true); auto f=r.consume(); CHECK(f.held==bit(Action::fire) && downs(f,Action::fire)==1);
    r.button(axis(11),false); r.consume();
    r.button(pad(2),true); CHECK(downs(r.consume(),Action::reload)==1);
    r.button(pad(9),true); CHECK(downs(r.consume(),Action::weapon_previous)==1);
    r.button(pad(10),true); CHECK(downs(r.consume(),Action::weapon_next)==1);
    r.button(pad(6),true); CHECK(downs(r.consume(),Action::pause)==1);
    r.button(pad(4),true); CHECK(downs(r.consume(),Action::screenshot)==1);
    r.button(pad(7),true); CHECK(downs(r.consume(),Action::recenter)==1);
    r.button(pad(0),true); f=r.consume(); CHECK(f.held&bit(Action::interact));
    auto menu=pad_game(InputContext::menu); menu.button(pad(0),true); CHECK(menu.consume().pressed==bit(Action::confirm));
    menu.button(pad(1),true); CHECK(menu.consume().pressed==bit(Action::back));
    menu.button(pad(7),true); CHECK(menu.consume().pressed==0);   // recenter is a gameplay action
    auto qte=pad_game(InputContext::qte); qte.button(pad(7),true); CHECK(qte.consume().pressed==0);
    qte.qte_window(true); qte.button(pad(8),true); CHECK(downs(qte.consume(),Action::qte_shake)==1);   // R3 shakes inside a QTE window
}
static void pad_chord_and_cancel() {
    auto r=pad_game(); r.button(pad(0),true); r.consume(); r.button(axis(11),true); auto f=r.consume();
    CHECK(downs(f,Action::grenade)==1 && downs(f,Action::fire)==0);   // interact+fire chord throws, like the keyboard
    auto d=pad_game(); d.button(axis(11),true); d.consume(); d.focus(false); auto g=d.consume(); CHECK(g.held==0);
    d.focus(true); d.button(axis(11),true); CHECK(d.consume().held==0);   // a held trigger stays blocked until it is released
    d.button(axis(11),false); d.button(axis(11),true); CHECK(d.consume().held==bit(Action::fire));
    fails<std::invalid_argument>([&]{d.button(pad(32),true);});
    fails<std::invalid_argument>([&]{d.button(axis(16),true);});
    auto keyboard_only=game(); keyboard_only.button(pad(0),true); CHECK(keyboard_only.consume().pressed==0);   // default resolver has no pad table
}
int main() {
    wii_table_holds_only_wii_buttons(); merge_keeps_every_device(); wii_buttons_drive_actions_and_do_not_collide_with_pads();
    pad_tables_validate_and_stay_separate(); merge_appends_pad_entries_without_duplicates(); pad_buttons_drive_actions_by_context(); pad_chord_and_cancel();
    defaults_and_context(); multiple_sources_and_short_click(); ordered_chord_and_dedup(); clear_special_and_remap();
    wheel_keyboard_repeat_and_chords(); replace_focus_and_context_cancel(); knife_direction_bursts_and_overflow(); knife_key_direction_and_mouse_threshold();
    wheel_pulse_preserves_keyboard_hold(); validation_preserves_configuration();
    qte_mouse_jerks(); qte_mouse_shake_profiles();
}
