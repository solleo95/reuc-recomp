#include "check.h"
#include "binding_editor.h"
using namespace uc;
static PhysicalInput key(uint16_t code) {return {PhysicalKind::key,code};}
int main() {
    BindingEditor editor;
    editor.clear(Action::grenade); CHECK(editor.bindings()[size_t(Action::grenade)].empty());
    CHECK(!editor.add(Action::grenade,{BindingKind::physical,key(11),{}}));
    CHECK(editor.add(Action::grenade,{BindingKind::physical,key(11),{}}));
    CHECK(editor.bindings()[size_t(Action::grenade)].size()==1);
    editor.begin_capture(Action::fire); editor.event(key(224),true); editor.event(key(9),true);
    CHECK(!editor.capturing());
    auto binding=editor.bindings()[size_t(Action::fire)].back();
    CHECK(binding.input==key(9) && binding.modifiers==std::vector<PhysicalInput>{key(224)});
    editor.begin_capture(Action::fire); editor.event(key(41),true);
    CHECK(!editor.capturing() && editor.bindings()[size_t(Action::fire)].size()==3);
    CHECK(editor.bindings()[size_t(Action::fire)].back().input==key(41));
    CHECK(editor.bindings()[size_t(Action::fire)].back().modifiers.empty());
    editor.begin_capture(Action::knife); editor.event(key(230),true); editor.event(key(230),false);
    CHECK(editor.bindings()[size_t(Action::knife)].back().input==key(230));
    editor.begin_capture(Action::knife); editor.focus(false); editor.event(key(5),true);
    CHECK(!editor.capturing() && editor.bindings()[size_t(Action::knife)].back().input==key(230));
    editor.begin_capture(Action::reload); editor.event({PhysicalKind::mouse,4},true);
    CHECK(editor.bindings()[size_t(Action::reload)].back().input==(PhysicalInput{PhysicalKind::mouse,4}));
    editor.clear(Action::weapon_next); editor.begin_capture(Action::weapon_next); editor.wheel(-2);
    CHECK(editor.bindings()[size_t(Action::weapon_next)].back().input==(PhysicalInput{PhysicalKind::wheel,2}));
    editor.begin_capture(Action::knife); editor.clear(Action::knife); CHECK(!editor.capturing());
    CHECK(editor.bindings()[size_t(Action::knife)].empty());
    CHECK(editor.add(Action::fire,{BindingKind::interact_fire,{},{}}));
    editor.restore_defaults(); CHECK(editor.bindings()==default_bindings());
    editor.remove(Action::fire,0); CHECK(editor.bindings()[size_t(Action::fire)].empty());
    editor.restore_defaults();
    const auto original=editor.bindings();
    editor.begin_capture(Action::weapon_next,true);editor.focus(false);
    CHECK(editor.bindings()==original);
    editor.begin_capture(Action::weapon_next,true);editor.event(key(0),true);
    CHECK(editor.error() && editor.capturing());CHECK(editor.bindings()==original);
    editor.event(key(41),true);
    CHECK(!editor.capturing());
    CHECK(editor.bindings()[size_t(Action::weapon_next)]==std::vector<Binding>({{BindingKind::physical,key(41),{}}}));
    editor.begin_capture(Action::weapon_next);editor.event(key(9),true);
    CHECK(editor.bindings()[size_t(Action::weapon_next)].size()==2);
    return 0;
}
