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
    r.button(key(10),true); r.button(key(25),true); r.motion(10,0); f=r.consume();
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
static void knife_direction_bursts_and_overflow() {
    auto r=game(); r.motion(3,4); CHECK(downs(r.consume(),Action::knife)==0);
    r.button(key(25),true); auto f=r.consume(); CHECK(f.held==bit(Action::knife));
    CHECK(f.count==1 && f.events[0].direction_x==1 && f.events[0].direction_y==0);
    CHECK(r.consume().held==bit(Action::knife)); // Game cooldown, not resolver, repeats held V.
    r.button(key(25),false); r.consume(); r.button(mouse(1),true); r.consume();
    r.motion(3,4); f=r.consume(); CHECK(downs(f,Action::knife)==1);
    CHECK(std::abs(f.events[0].direction_x-.6f)<1e-6f && std::abs(f.events[0].direction_y-.8f)<1e-6f);
    for(int i=0;i<10000;++i)r.motion(1,0); f=r.consume(); CHECK(downs(f,Action::knife)==1 && f.events[0].direction_x==1);
    r.motion(3,4); r.motion(-3,-4); f=r.consume(); CHECK(downs(f,Action::knife)==0 && !(f.pressed&bit(Action::knife)));
    r.motion(1,0); r.button(key(21),true); r.motion(0,1); f=r.consume();
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
int main() {
    defaults_and_context(); multiple_sources_and_short_click(); ordered_chord_and_dedup(); clear_special_and_remap();
    wheel_keyboard_repeat_and_chords(); replace_focus_and_context_cancel(); knife_direction_bursts_and_overflow();
    wheel_pulse_preserves_keyboard_hold(); validation_preserves_configuration();
    qte_mouse_jerks();
}
