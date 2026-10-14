#include "check.h"
#include "input_prompts.h"
#include <stdexcept>
using namespace uc;
static Binding key(uint16_t code) {return {BindingKind::physical,{PhysicalKind::key,code},{}};}
int main() {
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
    return 0;
}
