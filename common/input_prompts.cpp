#include "input_prompts.h"
#include <algorithm>
#include <stdexcept>
namespace uc {
PromptDescription describe_prompt(const Bindings& bindings,Action action,const PromptDevice& device) {
    if(size_t(action)>=size_t(Action::count))throw std::invalid_argument("invalid prompt action");
    if(auto error=device.gamepad ? validate_pad_bindings(bindings) : validate_bindings(bindings))throw std::invalid_argument(*error);
    PromptDescription result{action,{},device};
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
    if(device.gamepad && device.gyro) {   // the gamepad's own gestures: the shake first (it is what the game asks for), the swing after the button
        if(action==Action::qte_shake)result.alternatives.insert(result.alternatives.begin(),{{},PromptGesture::shake});
        else if(action==Action::knife && !interact.empty() && result.alternatives.end()==std::find_if(result.alternatives.begin(),result.alternatives.end(),[](const auto& a){return a.gesture.has_value();}))
            result.alternatives.push_back({{{interact,true}},PromptGesture::directional_motion});
    }
    return result;
}
PromptDescription primary_prompt(const Bindings& bindings,std::initializer_list<Action> actions,const PromptDevice& device) {
    if(!actions.size())throw std::invalid_argument("prompt needs an action");
    PromptDescription result{*actions.begin(),{},device};
    for(Action action:actions) {
        auto full=describe_prompt(bindings,action,device);
        if(full.alternatives.empty())continue;
        auto first=std::move(full.alternatives.front());
        for(auto& choice:first.choices)if(choice.bindings.size()>1)choice.bindings.resize(1);
        result.alternatives.push_back(std::move(first));
    }
    return result;
}
PromptDescription chord_prompt(const Bindings& bindings,Action first,Action second,const PromptDevice& device) {
    PromptDescription result{first,{},device};
    PromptAlternative alternative;
    for(Action action:{first,second}) {
        auto full=describe_prompt(bindings,action,device);
        if(full.alternatives.empty() || full.alternatives.front().choices.empty())return result;
        auto choice=std::move(full.alternatives.front().choices.front());
        choice.held=false;
        if(choice.bindings.size()>1)choice.bindings.resize(1);
        alternative.choices.push_back(std::move(choice));
    }
    result.alternatives.push_back(std::move(alternative));
    return result;
}
std::string pad_glyph_label(PhysicalInput input,PadFamily family) {
    const bool xbox=family==PadFamily::xbox,sony=family==PadFamily::playstation,nintendo=family==PadFamily::nintendo;
    const auto pick=[&](const char* x,const char* p,const char* n,const char* g) { return std::string(xbox ? x : sony ? p : nintendo ? n : g); };
    if(input.kind==PhysicalKind::pad_button) {
        switch(input.code) {
        case 0: return pick("A","Cross","B","South");
        case 1: return pick("B","Circle","A","East");
        case 2: return pick("X","Square","Y","West");
        case 3: return pick("Y","Triangle","X","North");
        case 4: return pick("View","Create","-","Back");
        case 5: return pick("Xbox","PS","Home","Guide");
        case 6: return pick("Menu","Options","+","Start");
        case 7: return pick("LS","L3","LS","L3");
        case 8: return pick("RS","R3","RS","R3");
        case 9: return pick("LB","L1","L","LB");
        case 10: return pick("RB","R1","R","RB");
        case 11: return "D-pad Up";
        case 12: return "D-pad Down";
        case 13: return "D-pad Left";
        case 14: return "D-pad Right";
        default: return "Button "+std::to_string(input.code);
        }
    }
    if(input.kind==PhysicalKind::pad_axis) {
        const unsigned axis=input.code/2; const bool positive=input.code&1;
        static constexpr const char* sticks[2][2]={{"Left","Right"},{"Up","Down"}};   // x: negative left, positive right; y: negative up, positive down
        if(axis<4) return std::string(axis<2 ? "Left" : "Right")+" Stick "+sticks[axis&1][positive];
        if(axis==4 && positive) return pick("LT","L2","ZL","LT");
        if(axis==5 && positive) return pick("RT","R2","ZR","RT");
        return "Axis "+std::to_string(input.code);
    }
    return {};
}
PromptDevice resolve_prompt_device(ControlType type,PadModel model,bool pad_last_used,bool pad_connected,PadFamily connected_family,bool gyro_enabled,bool pad_has_gyro) {
    const bool gamepad=type==ControlType::gamepad || (type==ControlType::automatic && pad_last_used && pad_connected);
    if(!gamepad)return {};
    PadFamily family=connected_family;
    switch(model) {
    case PadModel::xbox: family=PadFamily::xbox; break;
    case PadModel::playstation: family=PadFamily::playstation; break;
    case PadModel::nintendo: family=PadFamily::nintendo; break;
    case PadModel::generic: family=PadFamily::generic; break;
    case PadModel::automatic: if(!pad_connected)family=PadFamily::xbox; break;
    }
    return {true,family,gyro_enabled && pad_has_gyro};
}
}
