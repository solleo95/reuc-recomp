#include "game_input.h"
#include "hooks.h"
#include "input_api.h"
#include "test_state.h"
#include "gx_vertices.h"
#include "guest_call.h"
#include "dispatch.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
namespace {
struct PadState {
    uc::NativeInput* input=nullptr;
    uc::InputContext context=uc::InputContext::inactive;
    uint64_t generation=0,qte_token=0,native_holds=0;
    uint32_t actor=0;
    std::deque<uc::ActionEvent> requests;
};
// Guest baton owns both states. Scoped selection also preserves nested guest calls.
PadState active_pad,other_pad;
unsigned player=0;
auto& input=active_pad.input;
auto& current_context=active_pad.context;
auto& generation=active_pad.generation;
auto& qte_token=active_pad.qte_token;
auto& native_holds=active_pad.native_holds;
auto& actor=active_pad.actor;
auto& requests=active_pad.requests;
struct PadScope {
    unsigned previous=player;
    explicit PadScope(unsigned next) { select(next); }
    ~PadScope() { select(previous); }
    static void select(unsigned next) { if(next!=player) {std::swap(active_pad,other_pad);player=next;} }
};
unsigned owner(uint32_t object,uint32_t offset) {
    return other_pad.actor && object==other_pad.actor+offset ? 1-player : player;
}
bool trace_actions=false;
unsigned trace_records=0;
unsigned request_records=0,wrapper_records=0,gate_records=0;
unsigned tutorial_records=0;
uint32_t traced_tutorial=0;
bool weapon(uc::Action action) { return action==uc::Action::weapon_previous || action==uc::Action::weapon_next; }
bool semantic(uc::Action action) { return weapon(action) || action==uc::Action::reload || action==uc::Action::grenade || action==uc::Action::knife || action==uc::Action::qte_shake; }
uint64_t bit(uc::Action action) { return uint64_t(1)<<unsigned(action); }
gx::Memory memory(const ppc::Ctx& c) { return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}}; }
bool current_requests() {
    if(input && input->using_host_input() && input->generation()==generation)return true;
    requests.clear(); native_holds=0; return false;
}
std::optional<uc::ActionEvent> take(uc::Action action) {
    if(!current_requests())return {};
    std::optional<uc::ActionEvent> result;
    for(auto i=requests.begin();i!=requests.end();) {
        if(i->action==action) {
            if(!result || action!=uc::Action::knife || i->pulse || !result->pulse)result=*i;
            i=requests.erase(i);
        } else ++i;
    }
    return result;
}
bool ready(const ppc::Ctx& c,uint32_t expected_actor) {
    if(!current_requests() || actor!=expected_actor || current_context!=uc::InputContext::gameplay)return false;
    const auto state=uc::input_probe_snapshot(c);
    return state.players[player].address==expected_actor && uc::game_input_context(state,player)==uc::InputContext::gameplay;
}
struct NativeResult { uint32_t word; double scalar; };
NativeResult invoke(ppc::Ctx& c,uint32_t address,std::initializer_list<uint32_t> args) {
    const auto saved=c;
    try {
        uc::call_guest(c,address,args);
        const NativeResult result{c.r[3],c.f[1]}; const auto polls=c.poll_count;
        c=saved; c.poll_count=polls; return result;
    } catch(...) { const auto polls=c.poll_count; c=saved; c.poll_count=polls; throw; }
}
void original_update(ppc::Ctx& c,uint32_t body,uint32_t stack_size,size_t object_size) {
    memory(c).read(c.r[3],object_size);
    const auto stack=c.r[1]; memory(c).read(stack-stack_size,stack_size+8);
    // Static lowering of the original entry's stwu; remaining body stays compiled.
    ppc::wr32(c,stack-stack_size,stack); c.r[1]=stack-stack_size; uc::call_indirect(c,body);
}
void action_log(const char* action,uint32_t object,bool accepted) {
    uc::test_state_action(action,accepted);
    if(trace_actions && trace_records<64) { ++trace_records; std::fprintf(stderr,"[native-action] %s object %08X accepted %u\n",action,object,accepted); }
}
}
namespace uc {
struct GameInputSnapshot { PadState active,other; unsigned player; };
std::shared_ptr<const GameInputSnapshot> game_input_checkpoint() {
    return std::make_shared<GameInputSnapshot>(GameInputSnapshot{active_pad,other_pad,player});
}
void game_input_restore(const GameInputSnapshot& saved) { active_pad=saved.active; other_pad=saved.other; player=saved.player; }
InputContext game_input_context(const InputProbeSnapshot& state,unsigned index) {
    if(index>=state.players.size())return InputContext::inactive;
    if(!state.ui_valid || !state.ui_enabled)return InputContext::inactive;
    // UI update dispatch801483D8: HUD2 and draw-only3/6 persist in active gameplay.
    for(size_t slot:{0u,1u,4u,5u,7u,8u,9u,10u})
        if(int32_t(state.ui_states[slot])>=0)return InputContext::menu;
    if(int32_t(state.ui_states[11])>0)return InputContext::menu;
    if(state.game_mode_valid && (state.pause_state==2 || state.game_mode==3))return InputContext::menu; // Original801FBAAC pause state and scene substate gate.
    if(state.tutorial_loaded && state.tutorial_state!=0x00100000)return InputContext::menu; // Original8024F558 busy getter.
    const auto& player=state.players[index];
    if(!player.valid || player.player_790!=index || player.state_794!=1)return InputContext::menu;
    // Timed-action mode set by800677A0/8006786C and consumed by800DB2D4.
    if(player.mode_630==1 || player.mode_630==2)return InputContext::qte;
    if(player.mode_630!=0)return InputContext::inactive;
    bool suspended=false;
    if(state.scene_valid) {
        // Original scene gate800F3E28: suspension counter/timer and both fades.
        suspended=std::bit_cast<float>(state.scene_words[0])>0 || int32_t(state.scene_words[1])>0;
        for(size_t offset:{0u,3u}) {
            float alpha=std::bit_cast<float>(state.fade_words[offset])/std::bit_cast<float>(state.fade_words[offset+2]);
            if(alpha>=1)alpha=1;
            if(state.fade_words[offset+1]>>24)alpha=1-alpha;
            if(alpha>0.9998999834060669f)return InputContext::menu;
        }
    }
    // The button QTEs (B, A+B) wait in a suspended scene: they need real A/B, not the menu's confirm/back (which has no B).
    return suspended ? InputContext::qte : InputContext::gameplay;
}
InputSample game_input_buttons(InputContext context,const ActionFrame& frame) {
    InputSample result;
    const auto buttons=[&](uint64_t actions) {
        uint32_t bits=0;
        if(context==InputContext::menu) {
            if(actions&bit(Action::confirm))bits|=kButtonA;
            if(actions&bit(Action::back))bits|=0x1000; // Original generic UI getter index11, including pause menu.
        } else if(context==InputContext::gameplay || context==InputContext::qte) {
            if(actions&bit(Action::interact))bits|=kButtonA;
            if(actions&bit(Action::fire))bits|=kButtonB;
            if(actions&bit(Action::pause))bits|=0x10; // Original801FBDA0 queries binding9 (Minus), not Plus.
        }
        return bits;
    };
    result.held=buttons(frame.held); result.pressed=buttons(frame.pressed);
    // Releases belong to the context that held the key, including a just-canceled context.
    if(frame.released&(bit(Action::interact)|bit(Action::confirm)))result.released|=kButtonA;
    if(frame.released&bit(Action::fire))result.released|=kButtonB;
    if(frame.released&bit(Action::pause))result.released|=0x10;
    if(frame.released&bit(Action::back))result.released|=0x1000;
    result.released&=~result.held;
    if(context==InputContext::gameplay) {
        const auto actions=frame.held|frame.pressed;
        result.stick_x=float(bool(actions&bit(Action::camera_right)))-float(bool(actions&bit(Action::camera_left)));
        result.stick_y=float(bool(actions&bit(Action::camera_up)))-float(bool(actions&bit(Action::camera_down)));
    }
    return result;
}
static InputSample game_input_sample_unfiltered(const ppc::Ctx& c,const InputSample& raw) {
    if(!input)return raw;
    const auto state=input_probe_snapshot(c);
    const auto context=game_input_context(state,player);
    if(player==0) {
        test_state_publish(state,uint8_t(context));
        test_state_apply_god(const_cast<ppc::Ctx&>(c),state);
    }
    if(trace_actions && state.tutorial_loaded && state.tutorial_state!=traced_tutorial) {
        traced_tutorial=state.tutorial_state;
        if(tutorial_records++<64)std::fprintf(stderr,"[native-tutorial] %08X\n",traced_tutorial);
    }
    if(trace_actions && (current_context!=context || actor!=state.players[player].address)) {
        const char* names[]={"inactive","menu","gameplay","qte"};
        std::fprintf(stderr,"[native-context] %s actor %08X ui6 %08X ui4 %08X\n",names[unsigned(context)],state.players[player].address,state.ui_states[6],state.ui_states[4]);
    }
    input->context(context);
    const uint64_t token=(uint64_t(state.players[player].mode_630)<<32)|state.players[player].qte_target_79C;
    if(actor!=state.players[player].address || qte_token!=token) {
        input->qte_window(false);
        std::erase_if(requests,[](auto request){return request.action==Action::qte_shake;});
    }
    qte_token=token;
    const bool qte_window=context==InputContext::qte && (state.players[player].flags_798&2);
    input->qte_window(qte_window);
    if(!input->using_host_input()) { requests.clear(); native_holds=0; current_context=context; actor=state.players[player].address; return raw; }
    const auto frame=input->consume_actions();
    if(current_context!=context || generation!=frame.generation || actor!=state.players[player].address)requests.clear();
    current_context=context; generation=frame.generation; actor=state.players[player].address; native_holds=frame.held;
    if(!qte_window)std::erase_if(requests,[](auto request){return request.action==Action::qte_shake;});
    if(trace_actions && (frame.pressed&bit(Action::pause))) {
        std::fprintf(stderr,"[native-pause] context %u scene %08X:%08X ui",unsigned(context),state.scene_words[0],state.scene_words[1]);
        for(auto slot:state.ui_states)std::fprintf(stderr," %08X",slot);
        std::fprintf(stderr,"\n");
    }
    for(size_t i=0;i<frame.count;++i) if(frame.events[i].down && semantic(frame.events[i].action)) {
        if(frame.events[i].action==Action::qte_shake && !qte_window)continue;
        if(requests.size()==256) { requests.clear(); throw std::overflow_error("native game action queue overflow"); }
        requests.push_back(frame.events[i]);
        if(trace_actions && request_records<32) { ++request_records; std::fprintf(stderr,"[native-request] action %u actor %08X generation %llu\n",unsigned(frame.events[i].action),actor,(unsigned long long)generation); }
    }
    const auto buttons=game_input_buttons(context,frame);
    auto result=raw;
    result.held=buttons.held; result.pressed=buttons.pressed; result.released=buttons.released;
    result.stick_x=buttons.stick_x; result.stick_y=buttons.stick_y;
    if(context==InputContext::gameplay && std::any_of(requests.begin(),requests.end(),[](auto request){return weapon(request.action);}))result.pressed|=kNativeWeaponStep;
    return result;
}
// Every pad sample the guest reads, host input or the harness's pad: the mods' input overrides (input.force_held and the others) apply last.
InputSample game_input_sample(const ppc::Ctx& c,const InputSample& raw) {
    PadScope selected(input_hle_channel());
    auto sample=game_input_sample_unfiltered(c,raw);
    uc::mods::InputView view{sample.x,sample.y,sample.held,sample.pressed,sample.released};
    uc::mods::filter_input(view);
    sample.x=view.x; sample.y=view.y; sample.held=view.held; sample.pressed=view.pressed; sample.released=view.released;
    return sample;
}
void game_input_init(NativeInput* source,bool trace,NativeInput* second) { active_pad={};other_pad={};player=0;input=source;other_pad.input=second;traced_tutorial=0; trace_actions=trace; trace_records=request_records=wrapper_records=gate_records=tutorial_records=0; input_hle_set_map(source ? game_input_sample : nullptr); }
void game_input_shutdown() { input_hle_set_map(nullptr); active_pad={};other_pad={};player=0; }
}
void hle_GameActionMask(ppc::Ctx& c) {
    PadScope selected(c.r[3]==0x8044A870 ? 1u : 0u);
    auto index=int32_t(c.r[4]); if(index<0 || index>=20)index=0;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(c.r[3],0x68);
    if(input && input->using_host_input() && c.r[3]==0x8044A808+player*0x68 &&
        ((index==4 && c.lr==0x80068510) || (index==5 && c.lr==0x8006854C))) {
        if(input->generation()!=generation)requests.clear();
        if(c.r[30]!=actor) {c.r[3]=0;return;} // Verified caller8006846C holds the original parent in r30.
        auto request=std::find_if(requests.begin(),requests.end(),[](auto event){return weapon(event.action);});
        c.r[3]=0;
        if(request!=requests.end() && current_context==uc::InputContext::gameplay) {
            const bool previous=request->action==uc::Action::weapon_previous;
            if(previous==(index==4))c.r[3]=uc::kNativeWeaponStep;
            if(index==5)requests.erase(request); // Both original direction queries belong to one selection attempt.
        }
        return;
    }
    if(input && input->using_host_input() && c.r[3]==0x8044A808+player*0x68 && c.lr==0x801AB590) { c.r[3]=0; return; }
    c.r[3]=(ppc::rd8(c,c.r[3]+0x54+index)&1) ? 0 : ppc::rd32(c,c.r[3]+4+index*4);
}
void hle_GameWeaponUpdate(ppc::Ctx& c) {
    PadScope selected(owner(c.r[3],0x48));
    auto uc_post_scope=uc::mods::post_scope<0x8006C268u>(c);   // mod post hooks: run when this wrapper returns, whichever way
    if(uc::mods::hook_enter<0x8006C268u>(c)) return;   // mod hook point (pre, then replace): the guest's r3 (weapon object) and r4 are read after it
    const auto object=c.r[3],allow=c.r[4],expected_actor=actor;
    if(trace_actions && wrapper_records<8) { ++wrapper_records; std::fprintf(stderr,"[native-wrapper] weapon %08X expected %08X allow %u\n",object,actor ? actor+0x48 : 0,allow); }
    const bool player_weapon=actor && object==actor+0x48;
    if(player_weapon && current_requests()) {
        memory(c).read(actor,0x7A0);
        if(allow!=1 || ppc::rd32(c,object+0x490)>1 || !(ppc::rdf32(c,actor+0x4F8+0xEC)>0))
            std::erase_if(requests,[](auto request){return weapon(request.action);});
    }
    original_update(c,0x8006C26C,128,0x4B0);
    if(!player_weapon || actor!=expected_actor)return;
    if(!take(uc::Action::reload))return;
    if(trace_actions && gate_records<32) {
        ++gate_records;std::fprintf(stderr,"[native-reload-gate] allow %u state %u timer %.6f context %u\n",allow,ppc::rd32(c,object+0x490),ppc::rdf32(c,actor+0x4F8+0xEC),unsigned(current_context));
    }
    bool accepted=false;
    if(allow==1 && ready(c,expected_actor) && ppc::rd32(c,object+0x490)<=1 && ppc::rdf32(c,expected_actor+0x4F8+0xEC)>0) {
        const auto capacity=invoke(c,0x801D431C,{ppc::rd32(c,object+0x480)}).word;
        const auto amount=ppc::rd32(c,object+0x48C),inventory_key=ppc::rd32(c,object+0x484);
        const bool refill=inventory_key==0 || (invoke(c,0x801BD014,{0x8056A2D0,ppc::rd32(c,object+0x494),inventory_key}).word&0xFFFF)>1;
        if(int32_t(capacity)>0 && amount<capacity && refill && ready(c,expected_actor)) {
            invoke(c,0x8006D980,{object}); accepted=ppc::rd32(c,object+0x490)==2;
        }
    }
    action_log("Reload",object,accepted);
}
void hle_GameActionOwnerUpdate(ppc::Ctx& c) {
    PadScope selected(owner(c.r[3],0x4F8));
    const auto object=c.r[3],allow=c.r[4],expected_actor=actor;
    const bool player_owner=actor && object==actor+0x4F8;
    if(trace_actions && gate_records<32 && ((native_holds&bit(uc::Action::knife)) || !requests.empty())) {
        ++gate_records;std::fprintf(stderr,"[native-owner-gate] object %08X expected %08X allow %u context %u requests %zu knife-held %u\n",object,actor ? actor+0x4F8 : 0,allow,unsigned(current_context),requests.size(),bool(native_holds&bit(uc::Action::knife)));
    }
    original_update(c,0x801AB354,80,0x100);
    if(!player_owner || actor!=expected_actor)return;
    const auto grenade=take(uc::Action::grenade);
    const auto gesture=take(uc::Action::knife);
    if(allow!=1 || !ready(c,expected_actor)) {
        if(grenade)action_log("Grenade",object,false);
        if(gesture)action_log("Knife",object,false);
        return;
    }
    if(grenade) {
        const auto inventory_player=ppc::rd32(c,object+0xFC);
        const auto count=invoke(c,0x801BD014,{0x8056A2D0,inventory_player,3}).word&0xFFFF;
        bool accepted=false;
        if(count && ppc::rdf32(c,object+0xF0)<=0 && ready(c,expected_actor)) {
            invoke(c,0x801AB860,{object});
            accepted=(invoke(c,0x801BD014,{0x8056A2D0,inventory_player,3}).word&0xFFFF)<count;
        }
        action_log("Grenade",object,accepted); return;
    }
    const bool held=(native_holds&bit(uc::Action::knife))!=0;
    if(!gesture && !held)return;
    if(!(ppc::rdf32(c,object+0xF4)<=0)) { if(gesture)action_log("Knife",object,false); return; }
    const float x=gesture ? gesture->direction_x : uc::kKnifeKeyDirectionX,y=gesture ? gesture->direction_y : uc::kKnifeKeyDirectionY;
    if(!std::isfinite(x) || !std::isfinite(y) || (x==0 && y==0))throw std::invalid_argument("invalid native knife direction");
    // Native semantic direction/state consumed by801ABA00; no sensor samples.
    const auto processed=object+4;
    ppc::wrf32(c,processed+0xC,x); ppc::wrf32(c,processed+0x10,y); ppc::wr32(c,processed+0x70,2);
    invoke(c,0x801ABA00,{object,0}); action_log("Knife",object,ppc::rdf32(c,object+0xF4)>0);
}
void hle_GameMotionCount(ppc::Ctx& c) {
    const auto object=c.r[3];
    const bool first=c.lr==0x800672C0 || c.lr==0x800673A0;
    const bool second=c.lr==0x80067310 || c.lr==0x80067400;
    PadScope selected(owner(object,first ? 0x5F8 : 0x610));
    if(!input || !input->using_host_input() || c.r[4]!=0 || (!first && !second)) {
        original_update(c,0x80171D3C,64,24); return;
    }
    memory(c).read(object,24);
    if(!actor || object!=actor+(first ? 0x5F8 : 0x610)) {c.r[3]=ppc::rd32(c,object+0x10);return;}
    memory(c).read(actor,0x7A0);
    auto count=ppc::rd32(c,object+0x10);
    const auto state=uc::input_probe_snapshot(c);
    const bool window=current_requests() && current_context==uc::InputContext::qte &&
        uc::game_input_context(state,player)==uc::InputContext::qte && state.players[player].address==actor && (state.players[player].flags_798&2) &&
        ((uint64_t(state.players[player].mode_630)<<32)|state.players[player].qte_target_79C)==qte_token &&
        state.players[player].mode_630==((c.lr==0x800672C0 || c.lr==0x80067310) ? 1u : 2u);
    unsigned steps=0;
    for(auto i=requests.begin();i!=requests.end();) {
        if(i->action==uc::Action::qte_shake) {if(window)++steps;i=requests.erase(i);}else ++i;
    }
    if(steps) {
        if(count>uint32_t(std::numeric_limits<int32_t>::max())-steps)throw std::overflow_error("native QTE counter overflow");
        count+=steps; ppc::wr32(c,object+0x10,count); action_log("QTE",object,true);
    }
    c.r[3]=count; // Original caller retains threshold, timing, animation and failure decisions.
}
