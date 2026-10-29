#include "check.h"
#include "input_prompts.h"
#include <stdexcept>
using namespace uc;
static Binding key(uint16_t code) {return {BindingKind::physical,{PhysicalKind::key,code},{}};}
static PhysicalInput button(uint16_t code) {return {PhysicalKind::pad_button,code};}
static PhysicalInput axis(uint16_t code) {return {PhysicalKind::pad_axis,code};}
static void pad_prompts() {
    // Button names follow the family of the controller: SDL positions south 0, east 1, west 2, north 3, back 4, start 6, left shoulder 9, right shoulder 10, axis codes axis*2+1.
    CHECK(pad_glyph_label(button(0),PadFamily::xbox)=="A" && pad_glyph_label(button(1),PadFamily::xbox)=="B" && pad_glyph_label(button(2),PadFamily::xbox)=="X" && pad_glyph_label(button(3),PadFamily::xbox)=="Y");
    CHECK(pad_glyph_label(button(9),PadFamily::xbox)=="LB" && pad_glyph_label(axis(11),PadFamily::xbox)=="RT" && pad_glyph_label(button(6),PadFamily::xbox)=="Menu" && pad_glyph_label(button(4),PadFamily::xbox)=="View");
    CHECK(pad_glyph_label(button(0),PadFamily::playstation)=="Cross" && pad_glyph_label(button(1),PadFamily::playstation)=="Circle" && pad_glyph_label(button(2),PadFamily::playstation)=="Square" && pad_glyph_label(button(3),PadFamily::playstation)=="Triangle");
    CHECK(pad_glyph_label(button(10),PadFamily::playstation)=="R1" && pad_glyph_label(axis(9),PadFamily::playstation)=="L2" && pad_glyph_label(button(6),PadFamily::playstation)=="Options" && pad_glyph_label(button(4),PadFamily::playstation)=="Create");
    CHECK(pad_glyph_label(button(0),PadFamily::nintendo)=="B" && pad_glyph_label(button(1),PadFamily::nintendo)=="A" && pad_glyph_label(button(2),PadFamily::nintendo)=="Y" && pad_glyph_label(button(3),PadFamily::nintendo)=="X");   // the Nintendo layout swaps the letters
    CHECK(pad_glyph_label(button(9),PadFamily::nintendo)=="L" && pad_glyph_label(axis(11),PadFamily::nintendo)=="ZR" && pad_glyph_label(button(6),PadFamily::nintendo)=="+" && pad_glyph_label(button(4),PadFamily::nintendo)=="-");
    CHECK(pad_glyph_label(button(0),PadFamily::generic)=="South" && pad_glyph_label(button(1),PadFamily::generic)=="East" && pad_glyph_label(button(2),PadFamily::generic)=="West" && pad_glyph_label(button(3),PadFamily::generic)=="North");
    CHECK(pad_glyph_label(button(7),PadFamily::xbox)=="LS" && pad_glyph_label(button(8),PadFamily::playstation)=="R3" && pad_glyph_label(button(11),PadFamily::xbox)=="D-pad Up");
    CHECK(pad_glyph_label(axis(2),PadFamily::xbox)=="Left Stick Up" && pad_glyph_label(axis(1),PadFamily::xbox)=="Left Stick Right" && pad_glyph_label(axis(7),PadFamily::generic)=="Right Stick Down");
    CHECK(pad_glyph_label(button(31),PadFamily::xbox)=="Button 31" && pad_glyph_label({PhysicalKind::key,4},PadFamily::xbox).empty());   // unnamed inputs get a number; non-pad inputs have no glyph
    // Prompts built from the gamepad table: the first assigned input of each action, and the family travels with the description.
    const auto pad=default_pad_bindings(); const PromptDevice xbox{true,PadFamily::xbox,false};
    auto reload=primary_prompt(pad,{Action::reload},xbox);
    CHECK(reload.action==Action::reload && reload.alternatives.size()==1 && reload.alternatives[0].choices[0].bindings[0].input==button(2) && reload.device.gamepad && reload.device.family==PadFamily::xbox);
    auto weapons=primary_prompt(pad,{Action::weapon_previous,Action::weapon_next},xbox);
    CHECK(weapons.alternatives.size()==2 && weapons.alternatives[0].choices[0].bindings[0].input==button(9) && weapons.alternatives[1].choices[0].bindings[0].input==button(10));
    auto fire_grenade=chord_prompt(pad,Action::interact,Action::fire,xbox);
    CHECK(fire_grenade.alternatives.size()==1 && fire_grenade.alternatives[0].choices.size()==2 && fire_grenade.alternatives[0].choices[1].bindings[0].input==axis(11));
    CHECK(!describe_prompt(default_bindings(),Action::reload).device.gamepad);                    // the keyboard device is the default and unchanged
    bool refused=false;try {describe_prompt(pad,Action::reload);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);   // a gamepad table is not a keyboard table
    refused=false;try {describe_prompt(default_bindings(),Action::reload,xbox);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
    // With a gyro the shake is a gesture first (the button after it) and the knife gains a swing after its button.
    const PromptDevice gyro{true,PadFamily::playstation,true};
    auto shake=describe_prompt(pad,Action::qte_shake,gyro);
    CHECK(shake.alternatives.size()==2 && shake.alternatives[0].gesture==PromptGesture::shake && shake.alternatives[1].choices[0].bindings[0].input==button(8));
    CHECK(primary_prompt(pad,{Action::qte_shake},gyro).alternatives[0].gesture==PromptGesture::shake);
    auto knife=describe_prompt(pad,Action::knife,gyro);
    CHECK(knife.alternatives.size()==2 && knife.alternatives[0].choices[0].bindings[0].input==button(1) && knife.alternatives[1].gesture==PromptGesture::directional_motion && knife.alternatives[1].choices[0].held);
    CHECK(describe_prompt(pad,Action::qte_shake,xbox).alternatives.size()==1);                      // no gyro: only the button
    auto cleared=pad; cleared[size_t(Action::interact)].clear();
    CHECK(describe_prompt(cleared,Action::knife,gyro).alternatives.size()==1);                      // a swing needs interact held: without it there is none to show
    // Which device the prompts show: the player's choice, or the last device used; the model: the choice or the connected pad's family.
    const auto pick=[](ControlType type,PadModel model,bool pad_last,bool connected,PadFamily family,bool gyro_on,bool has_gyro) {
        return resolve_prompt_device(type,model,pad_last,connected,family,gyro_on,has_gyro);
    };
    CHECK(!pick(ControlType::keyboard_mouse,PadModel::xbox,true,true,PadFamily::xbox,true,true).gamepad);
    CHECK(!pick(ControlType::automatic,PadModel::automatic,false,true,PadFamily::xbox,false,false).gamepad);   // the mouse was used last
    CHECK(!pick(ControlType::automatic,PadModel::automatic,true,false,PadFamily::generic,false,false).gamepad);   // no pad connected: keyboard prompts
    auto automatic=pick(ControlType::automatic,PadModel::automatic,true,true,PadFamily::nintendo,true,true);
    CHECK(automatic.gamepad && automatic.family==PadFamily::nintendo && automatic.gyro);
    CHECK(pick(ControlType::automatic,PadModel::xbox,true,true,PadFamily::nintendo,false,true).family==PadFamily::xbox);   // an explicit model wins over the connected pad
    auto forced=pick(ControlType::gamepad,PadModel::automatic,false,false,PadFamily::generic,true,false);
    CHECK(forced.gamepad && forced.family==PadFamily::xbox && !forced.gyro);   // no pad to ask: the Xbox look, as for a pad of unknown make; "generic" is only picked by hand                         // a chosen gamepad shows even unplugged; without gyro hardware there is no gyro prompt
    CHECK(!pick(ControlType::gamepad,PadModel::playstation,false,true,PadFamily::xbox,false,true).gyro);   // gyro off in the settings
}
static PhysicalInput wii(uint16_t code) {return {PhysicalKind::wii_button,code};}
static void wii_prompts() {
    // The remote's buttons are named as printed on it; the family of a gamepad changes nothing.
    const char* names[]={"D-pad Left","D-pad Right","D-pad Down","D-pad Up","+","2","1","B","A","-","Home","Z","C"};
    for(uint16_t code=0;code<kWiiButtonCount;++code) CHECK(pad_glyph_label(wii(code),PadFamily::xbox)==names[code] && pad_glyph_label(wii(code),PadFamily::nintendo)==names[code]);
    const auto table=default_wii_bindings(); const PromptDevice remote{false,PadFamily::generic,false,true};
    auto fire=primary_prompt(table,{Action::fire},remote);
    CHECK(fire.alternatives.size()==1 && fire.alternatives[0].choices[0].bindings[0].input==wii(7) && fire.device.wiimote && !fire.device.gamepad);
    auto grenade=chord_prompt(table,Action::interact,Action::fire,remote);   // A+B
    CHECK(grenade.alternatives.size()==1 && grenade.alternatives[0].choices.size()==2 && grenade.alternatives[0].choices[0].bindings[0].input==wii(8) && grenade.alternatives[0].choices[1].bindings[0].input==wii(7));
    // The remote swings and shakes with its own motion, so the gestures show without any gyro switch.
    auto shake=describe_prompt(table,Action::qte_shake,remote);
    CHECK(!shake.alternatives.empty() && shake.alternatives[0].gesture==PromptGesture::shake);
    auto knife=describe_prompt(table,Action::knife,remote);
    CHECK(knife.alternatives.size()==2 && knife.alternatives[0].choices[0].bindings[0].input==wii(11) && knife.alternatives[1].gesture==PromptGesture::directional_motion && knife.alternatives[1].choices[0].held);
    // A table of one device is not read for another.
    bool refused=false;try {describe_prompt(table,Action::reload,PromptDevice{true,PadFamily::xbox,false});}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
    refused=false;try {describe_prompt(default_bindings(),Action::reload,remote);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
    refused=false;try {describe_prompt(table,Action::reload);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
    CHECK(!resolve_prompt_device(ControlType::automatic,PadModel::automatic,false,false,PadFamily::xbox,false,false).wiimote);
}
int main() {
    wii_prompts();
    pad_prompts();
    auto bindings=default_bindings();
    auto weapon=describe_prompt(bindings,Action::weapon_previous);
    CHECK(weapon.alternatives.size()==2 && weapon.alternatives[0].choices[0].bindings[0].input.kind==PhysicalKind::wheel);
    CHECK(weapon.alternatives[1].choices[0].bindings[0]==key(26));
    auto grenade=describe_prompt(bindings,Action::grenade);
    CHECK(grenade.alternatives.size()==2 && grenade.alternatives[1].choices.size()==2);
    CHECK(grenade.alternatives[1].choices[0].held && !grenade.alternatives[1].choices[1].held);
    bindings[size_t(Action::interact)]={key(40),key(44)};bindings[size_t(Action::fire)]={key(9),key(12)};
    grenade=describe_prompt(bindings,Action::grenade);
    CHECK(grenade.alternatives[1].choices[0].bindings==bindings[size_t(Action::interact)]);
    CHECK(grenade.alternatives[1].choices[1].bindings==bindings[size_t(Action::fire)]);
    bindings[size_t(Action::fire)].clear();CHECK(grenade.alternatives[1].choices[1].bindings.size()==2);
    CHECK(describe_prompt(bindings,Action::grenade).alternatives.size()==1);
    auto knife=describe_prompt(bindings,Action::knife);
    CHECK(knife.alternatives.size()==2 && knife.alternatives[0].choices[0].held);
    CHECK(knife.alternatives[1].gesture==PromptGesture::directional_motion && knife.alternatives[1].choices[0].held);
    bindings[size_t(Action::interact)].clear();CHECK(describe_prompt(bindings,Action::knife).alternatives.size()==1);
    bindings[size_t(Action::knife)].clear();CHECK(describe_prompt(bindings,Action::knife).alternatives.empty());
    auto qte=describe_prompt(bindings,Action::qte_shake);
    CHECK(qte.alternatives.size()==1 && qte.alternatives[0].choices.empty() && qte.alternatives[0].gesture==PromptGesture::shake);
    bindings[size_t(Action::qte_shake)].clear();CHECK(describe_prompt(bindings,Action::qte_shake).alternatives.empty());
    auto screenshot=describe_prompt(bindings,Action::screenshot);
    CHECK(screenshot.alternatives.size()==2 && screenshot.alternatives[0].choices[0].bindings[0].modifiers.size()==1);
    bool failed=false;try {describe_prompt(bindings,Action::count);}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    bindings[size_t(Action::fire)]={{BindingKind::interact_fire,{},{}}};
    failed=false;try {describe_prompt(bindings,Action::fire);}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    bindings=default_bindings();bindings[size_t(Action::interact)].clear();bindings[size_t(Action::fire)].clear();
    for(uint16_t code=1;code<=64;++code) {bindings[size_t(Action::interact)].push_back(key(code));bindings[size_t(Action::fire)].push_back(key(code));}
    grenade=describe_prompt(bindings,Action::grenade);
    CHECK(grenade.alternatives.size()==2 && grenade.alternatives[1].choices.size()==2);
    CHECK(grenade.alternatives[1].choices[0].bindings.size()==64 && grenade.alternatives[1].choices[1].bindings.size()==64);
    bindings=default_bindings();
    auto primary=primary_prompt(bindings,{Action::weapon_previous,Action::weapon_next});
    CHECK(primary.action==Action::weapon_previous && primary.alternatives.size()==2);
    CHECK(primary.alternatives[0].choices[0].bindings.size()==1 && primary.alternatives[0].choices[0].bindings[0].input.kind==PhysicalKind::wheel);
    CHECK(primary.alternatives[1].choices[0].bindings[0].input.kind==PhysicalKind::wheel);
    CHECK(primary.alternatives[0].choices[0].bindings[0]!=primary.alternatives[1].choices[0].bindings[0]);
    primary=primary_prompt(bindings,{Action::grenade});
    CHECK(primary.alternatives.size()==1 && primary.alternatives[0].choices.size()==1 && !primary.alternatives[0].gesture);
    bindings[size_t(Action::interact)]={key(40),key(44)};
    primary=primary_prompt(bindings,{Action::interact});
    CHECK(primary.alternatives.size()==1 && primary.alternatives[0].choices[0].bindings==std::vector<Binding>{key(40)});
    bindings[size_t(Action::interact)].clear();
    CHECK(primary_prompt(bindings,{Action::interact}).alternatives.empty());
    primary=primary_prompt(bindings,{Action::interact,Action::fire});
    CHECK(primary.alternatives.size()==1 && primary.alternatives[0].choices[0].bindings[0].input.kind==PhysicalKind::mouse);
    failed=false;try {primary_prompt(bindings,{});}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    // A+B button prompt: one alternative holding both first bindings, empty when either action is cleared.
    bindings=default_bindings();
    auto chord=chord_prompt(bindings,Action::interact,Action::fire);
    CHECK(chord.action==Action::interact && chord.alternatives.size()==1 && chord.alternatives[0].choices.size()==2 && !chord.alternatives[0].gesture);
    CHECK(chord.alternatives[0].choices[0].bindings==std::vector<Binding>{bindings[size_t(Action::interact)][0]});
    CHECK(chord.alternatives[0].choices[1].bindings==std::vector<Binding>{bindings[size_t(Action::fire)][0]});
    CHECK(!chord.alternatives[0].choices[0].held && !chord.alternatives[0].choices[1].held);
    bindings[size_t(Action::fire)]={key(5),key(6)};
    chord=chord_prompt(bindings,Action::interact,Action::fire);
    CHECK(chord.alternatives[0].choices[1].bindings==std::vector<Binding>{key(5)});
    bindings[size_t(Action::fire)].clear();CHECK(chord_prompt(bindings,Action::interact,Action::fire).alternatives.empty());
    bindings=default_bindings();bindings[size_t(Action::interact)].clear();CHECK(chord_prompt(bindings,Action::interact,Action::fire).alternatives.empty());
    return 0;
}
