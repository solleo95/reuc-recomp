#include "input_prompts.h"
#include <stdexcept>
namespace uc {
PromptDescription describe_prompt(const Bindings& bindings,Action action) {
    if(size_t(action)>=size_t(Action::count))throw std::invalid_argument("invalid prompt action");
    if(auto error=validate_bindings(bindings))throw std::invalid_argument(*error);
    PromptDescription result{action,{}};
    const auto& interact=bindings[size_t(Action::interact)];
    const auto& fire=bindings[size_t(Action::fire)];
    for(const auto& binding:bindings[size_t(action)]) {
        switch(binding.kind) {
        case BindingKind::physical:
            result.alternatives.push_back({{{{binding},action==Action::knife}},std::nullopt});break;
        case BindingKind::interact_fire:
            if(!interact.empty() && !fire.empty())result.alternatives.push_back({{{interact,true},{fire,false}},std::nullopt});
            break;
        case BindingKind::interact_motion:
            if(!interact.empty())result.alternatives.push_back({{{interact,true}},PromptGesture::directional_motion});
            break;
        case BindingKind::qte_motion:
            result.alternatives.push_back({{},PromptGesture::shake});break;
        }
    }
    return result;
}
PromptDescription primary_prompt(const Bindings& bindings,std::initializer_list<Action> actions) {
    if(!actions.size())throw std::invalid_argument("prompt needs an action");
    PromptDescription result{*actions.begin(),{}};
    for(Action action:actions) {
        auto full=describe_prompt(bindings,action);
        if(full.alternatives.empty())continue;
        auto first=std::move(full.alternatives.front());
        for(auto& choice:first.choices)if(choice.bindings.size()>1)choice.bindings.resize(1);
        result.alternatives.push_back(std::move(first));
    }
    return result;
}
PromptDescription chord_prompt(const Bindings& bindings,Action first,Action second) {
    PromptDescription result{first,{}};
    PromptAlternative alternative;
    for(Action action:{first,second}) {
        auto full=describe_prompt(bindings,action);
        if(full.alternatives.empty() || full.alternatives.front().choices.empty())return result;
        auto choice=std::move(full.alternatives.front().choices.front());
        choice.held=false;
        if(choice.bindings.size()>1)choice.bindings.resize(1);
        alternative.choices.push_back(std::move(choice));
    }
    result.alternatives.push_back(std::move(alternative));
    return result;
}
}
