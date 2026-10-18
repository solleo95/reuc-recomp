#include "prompt_bridge.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace uc {
namespace {
uint32_t word(std::span<const std::byte> bytes,size_t at) {
    uint32_t v=0;for(size_t i=0;i<4;++i)v=(v<<8)|std::to_integer<uint8_t>(bytes[at+i]);return v;
}
}
void PromptRegistry::unregister_layout(uint32_t layout) {owners_.erase(layout);}
void PromptRegistry::unregister_pane(uint32_t pane) {
    std::erase_if(owners_,[&](const auto& owner){return owner.second.root==pane;});
}
void PromptRegistry::register_layout(const gx::Memory& memory,uint32_t layout,std::string_view name) {
    unregister_layout(layout); // The game allocator reuses Layout addresses between screens.
    if(!name.ends_with(".brlyt") || name.size()>63 || owners_.size()>=256)return;
    try {
        const auto root=word(memory.read(layout,0x18),0x10);
        memory.read(root,0xD4);
        unregister_pane(root);
        owners_.emplace(layout,Owner{root,std::string(name)});
    } catch(const gx::Error&) {return;} // An unsupported SDK instance retains its original draw.
}
std::optional<std::string> PromptRegistry::layout_name(const gx::Memory& memory,uint32_t pane) const {
    try {
        std::set<uint32_t> visited;
        for(size_t depth=0;pane && depth<64;++depth) {
            if(!visited.insert(pane).second)return {};
            for(const auto& [layout,owner]:owners_) if(owner.root==pane && word(memory.read(layout,0x18),0x10)==pane)return owner.name;
            pane=word(memory.read(pane,0x10),0x0C);
        }
    } catch(const gx::Error&) {return {};}
    return {};
}
namespace {
enum class Button {none,a,b,ab,shake};
// ACT_* layouts: a standalone layout names its picture tex/tex1/tex2; ACT_All holds every prompt and names the pane after the button.
Button button_of(std::string_view layout,std::string_view pane) {
    const bool generic=pane=="tex" || pane=="tex1" || pane=="tex2";
    if(layout=="ACT_A_Press.brlyt" || layout=="ACT_A_Repeat.brlyt")return generic ? Button::a : Button::none;
    if(layout=="ACT_B_Press.brlyt" || layout=="ACT_B_Repeat.brlyt")return generic ? Button::b : Button::none;
    if(layout=="ACT_AB_Press.brlyt")return generic ? Button::ab : Button::none;
    if(layout=="ACT_R_Hshake.brlyt" || layout=="ACT_N_Hshake.brlyt")return generic ? Button::shake : Button::none;
    if(layout=="ACT_All.brlyt") {
        if(pane=="tex_A_Press" || pane=="tex_A_Repeat")return Button::a;
        if(pane=="tex_B_Press" || pane=="tex_B_Repeat")return Button::b;
        if(pane=="tex_R_Hshake" || pane=="tex_N_Hshake")return Button::shake;
    }
    return Button::none;
}
}
std::vector<Action> PromptRegistry::actions(const gx::Memory& memory,uint32_t pane,InputContext context) const {
    if(context==InputContext::inactive)return {};
    try {
        const auto picture=memory.read(pane,0xEC);
        const auto text=picture.subspan(0xB8,16);
        const auto end=std::find(text.begin(),text.end(),std::byte{});
        if(end==text.end())return {};
        const std::string_view name(reinterpret_cast<const char*>(text.data()),size_t(end-text.begin()));
        memory.read(word(picture,0x28),4);
        const auto layout=layout_name(memory,pane);
        if(!layout)return {};
        const bool menu=context==InputContext::menu;
        switch(button_of(*layout,name)) {
        case Button::a: return {menu ? Action::confirm : Action::interact};
        case Button::b: return {menu ? Action::back : Action::fire};
        case Button::ab: return {Action::interact,Action::fire};   // like the dodge prompt, drawn while the scene gate reads as menu
        case Button::shake: return {Action::qte_shake}; // The dodge prompt of boss fights is drawn while the scene gate reads as menu.
        case Button::none: break;
        }
    } catch(const gx::Error&) {return {};} // Bounds failure cannot enable suppression.
    return {};
}
std::optional<Action> PromptRegistry::action(const gx::Memory& memory,uint32_t pane,InputContext context) const {
    const auto found=actions(memory,pane,context);
    if(found.size()!=1)return {};
    return found.front();
}
std::vector<Action> glyph_actions(uint16_t code,bool menu) {
    switch(code) {
    case 0x391: return {menu ? Action::confirm : Action::interact}; // A
    case 0x392: return {menu ? Action::back : Action::fire};        // B
    case 0x394: return {Action::weapon_previous,Action::weapon_next}; // Control Pad up/down
    case 0x393: return {Action::weapon_next}; // Nunchuk C: "…or C to switch weapons" cycles forward.
    case 0x39E: return {menu ? Action::back : Action::pause}; // Minus: menu footer "Back : −"; gameplay pause binding.
    default: return {}; // Z, stick: no port action emits them.
    }
}
}
