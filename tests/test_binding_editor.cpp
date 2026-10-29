#include "check.h"
#include "binding_editor.h"
using namespace uc;
static PhysicalInput key(uint16_t code) {return {PhysicalKind::key,code};}
static PhysicalInput button(uint16_t code) {return {PhysicalKind::pad_button,code};}
static PhysicalInput axis(uint16_t code) {return {PhysicalKind::pad_axis,code};}
// The gamepad table: its own defaults and rule, only gamepad inputs are captured, and no modifier keys.
static void gamepad_editor() {
    BindingEditor editor(default_pad_bindings(),BindingTable::gamepad);
    CHECK(editor.bindings()==default_pad_bindings());
    editor.begin_capture(Action::reload,true);
    editor.event(key(4),true); editor.event({PhysicalKind::mouse,1},true); editor.wheel(1);
    CHECK(editor.capturing() && editor.bindings()==default_pad_bindings());                  // the keyboard and mouse do nothing while a pad button is awaited
    editor.event(button(3),true);
    CHECK(!editor.capturing() && editor.bindings()[size_t(Action::reload)]==std::vector<Binding>({{BindingKind::physical,button(3),{}}}));
    editor.begin_capture(Action::fire);                                                       // the trigger pull and a held shoulder button are one binding each
    editor.event(axis(9),true);
    CHECK(!editor.capturing() && editor.bindings()[size_t(Action::fire)].size()==2 && editor.bindings()[size_t(Action::fire)].back().input==axis(9));
    editor.begin_capture(Action::fire);editor.event(button(9),false);                         // a release alone is not a press
    CHECK(editor.capturing());
    editor.event(button(9),true);
    CHECK(!editor.capturing() && editor.bindings()[size_t(Action::fire)].back().input==button(9) && editor.bindings()[size_t(Action::fire)].back().modifiers.empty());
    editor.begin_capture(Action::pause);editor.event(button(32),true);                        // outside SDL's buttons: refused, still waiting
    CHECK(editor.capturing() && editor.error());
    editor.event(button(6),true); CHECK(editor.capturing() && editor.error());               // Start is already Pause: a duplicate is refused, the capture goes on
    editor.cancel_capture(); CHECK(editor.bindings()[size_t(Action::pause)].size()==1);
    CHECK(editor.add(Action::fire,{BindingKind::physical,key(9),{}}));                        // a key does not belong in this table
    CHECK(!editor.add(Action::fire,{BindingKind::physical,button(12),{}}));
    CHECK(editor.add(Action::fire,{BindingKind::physical,button(12),{}}));                    // and no duplicates
    editor.begin_capture(Action::knife); editor.focus(false); editor.event(button(5),true);
    CHECK(!editor.capturing());
    editor.restore_defaults(); CHECK(editor.bindings()==default_pad_bindings());              // defaults are the gamepad's, not the keyboard's
    BindingEditor keyboard;                                                                   // and the keyboard editor ignores the pad
    keyboard.begin_capture(Action::reload,true); keyboard.event(button(3),true); keyboard.event(axis(9),true);
    CHECK(keyboard.capturing() && keyboard.bindings()==default_bindings());
    CHECK(keyboard.add(Action::fire,{BindingKind::physical,button(3),{}}));
}
int main() {
    gamepad_editor();
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
    {   // Insert (settings menu) and P (pause) belong to the port: a capture refuses them and keeps waiting
        const auto before=editor.bindings();
        editor.begin_capture(Action::reload,true);
        for(uint16_t reserved:{uint16_t(73),uint16_t(19)}) {
            editor.event(key(reserved),true);
            CHECK(editor.capturing() && editor.error() && editor.bindings()==before);
        }
        editor.event(key(21),true);
        CHECK(!editor.capturing() && editor.bindings()[size_t(Action::reload)].back().input==key(21));
    }
    {   // the menu key is rebindable; the keys the port itself takes (P, Tab, Escape for the menu) are not, and nothing else may use the key that opens the menu
        editor.begin_capture(Action::menu,true);
        for(uint16_t refused:{uint16_t(19),uint16_t(43),uint16_t(41)}) { editor.event(key(refused),true); CHECK(editor.capturing() && editor.error()); }
        editor.event(key(67),true);   // F10
        CHECK(!editor.capturing() && editor.bindings()[size_t(Action::menu)]==std::vector<Binding>({{BindingKind::physical,key(67),{}}}));
        editor.begin_capture(Action::reload,true); editor.event(key(67),true);
        CHECK(editor.capturing() && editor.error());                                    // F10 now opens the menu
        editor.event(key(73),true);
        CHECK(!editor.capturing() && editor.bindings()[size_t(Action::reload)].back().input==key(73));   // Insert is an ordinary key again
        editor.begin_capture(Action::menu,true); editor.event(key(73),true);
        CHECK(editor.capturing() && editor.error());   // reload has it
        editor.clear(Action::reload); editor.begin_capture(Action::menu,true); editor.event(key(73),true);
        CHECK(!editor.capturing() && editor.bindings()[size_t(Action::menu)][0].input==key(73));
        editor.clear(Action::menu); editor.begin_capture(Action::knife,true); editor.event(key(73),true);
        CHECK(editor.capturing() && editor.error());                                    // none bound: Insert still opens it
        editor.cancel_capture();
    }
    return 0;
}
