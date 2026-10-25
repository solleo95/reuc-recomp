#include "input_bindings.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
constexpr uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
bool valid(PhysicalInput input,bool wheel=true) {
    switch(input.kind) {
    case PhysicalKind::key: return input.code>0 && input.code<512;
    case PhysicalKind::mouse: return input.code>0 && input.code<256;
    case PhysicalKind::wheel: return wheel && (input.code==1 || input.code==2);
    }
    return false;
}
Binding key(uint16_t code) { return {BindingKind::physical,{PhysicalKind::key,code},{}}; }
Binding mouse(uint16_t code) { return {BindingKind::physical,{PhysicalKind::mouse,code},{}}; }
}
Bindings default_bindings() {
    Bindings result;
    result[size_t(Action::interact)]={mouse(1)};
    result[size_t(Action::fire)]={mouse(3)};
    result[size_t(Action::weapon_previous)]={{BindingKind::physical,{PhysicalKind::wheel,1},{}},key(26)};
    result[size_t(Action::weapon_next)]={{BindingKind::physical,{PhysicalKind::wheel,2},{}},key(22)};
    result[size_t(Action::reload)]={key(21)};
    result[size_t(Action::grenade)]={key(10),{BindingKind::interact_fire,{},{}}};
    result[size_t(Action::knife)]={key(25),{BindingKind::interact_motion,{},{}}};
    result[size_t(Action::pause)]={key(41)};
    result[size_t(Action::confirm)]={mouse(1),key(40)};
    result[size_t(Action::back)]={mouse(3),key(41)};
    result[size_t(Action::release_cursor)]={key(226),key(230)};
    result[size_t(Action::screenshot)]={
        {BindingKind::physical,{PhysicalKind::key,8},{{PhysicalKind::key,224}}},
        {BindingKind::physical,{PhysicalKind::key,8},{{PhysicalKind::key,228}}}};
    result[size_t(Action::qte_shake)]={{BindingKind::qte_motion,{},{}}};
    result[size_t(Action::camera_up)]={key(82)};
    result[size_t(Action::camera_down)]={key(81)};
    result[size_t(Action::camera_left)]={key(80)};
    result[size_t(Action::camera_right)]={key(79)};
    return result;
}
std::optional<std::string> validate_bindings(const Bindings& bindings) {
    for(size_t a=0;a<bindings.size();++a) {
        const auto& entries=bindings[a];
        if(entries.size()>64) return "too many bindings for action";
        for(size_t i=0;i<entries.size();++i) {
            const auto& b=entries[i];
            if(b.kind==BindingKind::physical) {
                if(!valid(b.input) || b.modifiers.size()>4) return "invalid physical binding";
                for(size_t m=0;m<b.modifiers.size();++m) {
                    if(!valid(b.modifiers[m],false) || b.modifiers[m]==b.input) return "invalid binding modifier";
                    if(std::find(b.modifiers.begin(),b.modifiers.begin()+m,b.modifiers[m])!=b.modifiers.begin()+m) return "duplicate binding modifier";
                }
            } else if((b.kind==BindingKind::interact_fire && a==size_t(Action::grenade)) ||
                      (b.kind==BindingKind::interact_motion && a==size_t(Action::knife)) ||
                      (b.kind==BindingKind::qte_motion && a==size_t(Action::qte_shake))) {
                if(b.input!=PhysicalInput{} || !b.modifiers.empty()) return "invalid action combination";
            } else return "invalid binding kind for action";
            for(size_t j=0;j<i;++j) {
                const auto& other=entries[j];
                if(other.kind==b.kind && other.input==b.input && other.modifiers.size()==b.modifiers.size() &&
                   std::is_permutation(other.modifiers.begin(),other.modifiers.end(),b.modifiers.begin())) return "duplicate action binding";
            }
        }
    }
    return {};
}
bool BindingResolver::allowed(Action action) const {
    if(!focused_) return false;
    if(action==Action::release_cursor || action==Action::screenshot) return true;
    if(action==Action::confirm || action==Action::back) return context_==InputContext::menu;
    if(action==Action::qte_shake)return context_==InputContext::qte;
    if(context_==InputContext::gameplay) return true;
    return context_==InputContext::qte && (action==Action::interact || action==Action::fire ||
        action==Action::reload || action==Action::knife || action==Action::pause);
}
bool BindingResolver::held(PhysicalInput input) const {
    return input.kind==PhysicalKind::key ? keys_[input.code] : input.kind==PhysicalKind::mouse && mouse_[input.code];
}
bool BindingResolver::matches(const Binding& binding,PhysicalInput input) const {
    return binding.kind==BindingKind::physical && binding.input==input &&
        std::all_of(binding.modifiers.begin(),binding.modifiers.end(),[this](auto m){return held(m);});
}
bool BindingResolver::special(Action action,BindingKind kind) const {
    return allowed(action) && std::any_of(bindings_[size_t(action)].begin(),bindings_[size_t(action)].end(),
        [kind](const auto& b){return b.kind==kind;});
}
uint64_t BindingResolver::physical_actions() const {
    uint64_t result=0;
    for(size_t a=0;a<bindings_.size();++a) if(allowed(Action(a)))
        for(const auto& b:bindings_[a]) if(b.kind==BindingKind::physical && held(b.input) && matches(b,b.input)) {result|=bit(Action(a));break;}
    return result;
}
void BindingResolver::cancel() {
    blocked_keys_|=keys_; blocked_mouse_|=mouse_; keys_.reset(); mouse_.reset();
    canceled_releases_|=frame_.released|frame_.held;
    frame_={}; frame_.released=canceled_releases_; raw_actions_=0; suppress_fire_=false;
    motion_event_.reset(); motion_x_=motion_y_=0;
    shake_={};
}
void BindingResolver::replace(const Bindings& bindings) {
    if(auto error=validate_bindings(bindings)) throw std::invalid_argument(*error);
    auto next=bindings; cancel(); bindings_=std::move(next);
}
void BindingResolver::context(InputContext context) {
    if(unsigned(context)>unsigned(InputContext::qte)) throw std::invalid_argument("invalid input context");
    if(context_!=context) {cancel();context_=context;}
}
void BindingResolver::qte_window(bool active) {
    if(qte_window_==active)return;
    finish_motion();
    qte_window_=active;
    shake_={};
    size_t kept=0;
    for(size_t i=0;i<frame_.count;++i)if(frame_.events[i].action!=Action::qte_shake)frame_.events[kept++]=frame_.events[i];
    frame_.count=kept;frame_.pressed&=~bit(Action::qte_shake);frame_.released&=~bit(Action::qte_shake);
}
void BindingResolver::focus(bool focused) {
    if(!focused) cancel();
    focused_=focused;
}
void BindingResolver::emit(Action action,bool down,float x,float y,bool pulse_event) {
    if(down && action==Action::grenade && (frame_.pressed&bit(action))) return;
    if(frame_.count==frame_.events.size()) {cancel();throw std::overflow_error("input action queue overflow");}
    frame_.events[frame_.count++]={action,down,x,y,pulse_event};
    (down ? frame_.pressed : frame_.released)|=bit(action);
}
void BindingResolver::pulse(Action action,float x,float y) {
    if(action==Action::grenade && (frame_.pressed&bit(action))) return;
    emit(action,true,x,y,true);
    if(!(frame_.held&bit(action))) emit(action,false,0,0,true);
}
void BindingResolver::finish_motion() {
    if(motion_event_ && motion_x_==0 && motion_y_==0) {
        frame_.count=*motion_event_;
        frame_.pressed&=~bit(Action::knife);
        frame_.released=(frame_.released&~bit(Action::knife))|(canceled_releases_&bit(Action::knife));
        for(size_t i=0;i<frame_.count;++i) if(frame_.events[i].action==Action::knife)
            (frame_.events[i].down ? frame_.pressed : frame_.released)|=bit(Action::knife);
    }
    if(motion_event_)motion_x_=motion_y_=0;   // the stroke became a swing (or was cancelled): the next one starts from zero
    motion_event_.reset();
}
void BindingResolver::button(PhysicalInput input,bool down) {
    if(!valid(input,false)) throw std::invalid_argument("invalid input button");
    finish_motion();
    const bool blocked=input.kind==PhysicalKind::key ? blocked_keys_[input.code] : blocked_mouse_[input.code];
    if(blocked) {
        if(!down) {if(input.kind==PhysicalKind::key)blocked_keys_[input.code]=false;else blocked_mouse_[input.code]=false;}
        return;
    }
    if(!focused_) return;
    const bool was=held(input); if(was==down)return;
    if(input.kind==PhysicalKind::key) keys_[input.code]=down; else mouse_[input.code]=down;
    const auto next=physical_actions();
    const bool chord=context_==InputContext::gameplay && (frame_.held&bit(Action::interact)) &&
        (next&bit(Action::fire)) && !(raw_actions_&bit(Action::fire)) && special(Action::grenade,BindingKind::interact_fire);
    if(!(next&bit(Action::fire)))suppress_fire_=false;
    if(chord)suppress_fire_=true;
    const auto active=next & ~(suppress_fire_ ? bit(Action::fire) : 0);
    const auto changed=frame_.held^active;
    frame_.held=active; raw_actions_=next;
    if(!(active&bit(Action::interact)))motion_x_=motion_y_=0;
    if(chord)pulse(Action::grenade);
    for(size_t a=0;a<bindings_.size();++a) if(changed&bit(Action(a))) {
        const bool pressed=(active&bit(Action(a)))!=0;
        emit(Action(a),pressed,pressed && Action(a)==Action::knife ? kKnifeKeyDirectionX : 0.0f,pressed && Action(a)==Action::knife ? kKnifeKeyDirectionY : 0.0f);
    }
}
void BindingResolver::wheel(int steps) {
    finish_motion();
    const int64_t count=steps<0 ? -int64_t(steps) : steps;
    if(count>256) {cancel();throw std::overflow_error("input wheel burst overflow");}
    if(!focused_)return;
    const PhysicalInput input{PhysicalKind::wheel,uint16_t(steps>0 ? 1 : 2)};
    for(int64_t i=0;i<count;++i) for(size_t a=0;a<bindings_.size();++a) if(allowed(Action(a)) &&
        std::any_of(bindings_[a].begin(),bindings_[a].end(),[&](const auto& b){return matches(b,input);})) {
        const auto action=Action(a);
        if(action==Action::fire && (frame_.held&bit(Action::interact)) && special(Action::grenade,BindingKind::interact_fire))pulse(Action::grenade);
        else if((action!=Action::fire && action!=Action::interact && action!=Action::knife) || !(frame_.held&bit(action)))
            pulse(action,action==Action::knife ? kKnifeKeyDirectionX : 0.0f,action==Action::knife ? kKnifeKeyDirectionY : 0.0f);
    }
}
void BindingResolver::qte_motion(float x,float y,uint64_t time) {
    if(!time || !qte_window_ || !special(Action::qte_shake,BindingKind::qte_motion))return;
    // One step per reversal: the mouse travels kShakeStroke counts back against the stroke since its peak, at shaking speed.
    // The speed is a decayed path length, not the speed since the turn: a hand's speed passes through zero at every turn.
    auto& s=shake_;
    if(s.last && (time<s.last || time-s.last>kShakeGapNs))s={};
    s.path=s.path*std::exp(-(s.last ? double(time-s.last)/1e9 : 0.0)/kShakePathSeconds)+std::hypot(x,y);
    s.last=time;
    if(!s.stroke) {
        s.back_x+=x; s.back_y+=y;
        const double length=std::hypot(s.back_x,s.back_y);
        if(length<kShakeStroke)return;
        s.dx=s.back_x/length; s.dy=s.back_y/length; s.proj=s.peak=length; s.back_x=s.back_y=0; s.stroke=true;
        return;
    }
    s.proj+=x*s.dx+y*s.dy;
    if(s.proj>=s.peak) {s.peak=s.proj; s.back_x=s.back_y=0; return;}
    s.back_x+=x; s.back_y+=y;
    if(-(s.back_x*s.dx+s.back_y*s.dy)<kShakeStroke)return;
    if(s.path<kShakeMinPath) {s.peak=s.proj; s.back_x=s.back_y=0; return;}   // a slow return is aiming: the stroke restarts here
    const double length=std::hypot(s.back_x,s.back_y);
    s.dx=s.back_x/length; s.dy=s.back_y/length; s.proj=s.peak=length; s.back_x=s.back_y=0;
    finish_motion(); pulse(Action::qte_shake,float(s.dx),float(s.dy));
}
void BindingResolver::motion(float x,float y,uint64_t timestamp_ns) {
    if(!std::isfinite(x) || !std::isfinite(y))throw std::invalid_argument("invalid mouse motion");
    if(x!=0 || y!=0)qte_motion(x,y,timestamp_ns);
    if((x==0 && y==0) || !(frame_.held&bit(Action::interact)) || !special(Action::knife,BindingKind::interact_motion))return;
    // Speed, not distance: motion inside a short window is summed and divided by the window's age. Slow drags and jitter never reach the speed.
    if(!timestamp_ns)return;   // no clock, no speed
    // The first event of a window has no start to measure from: it is credited the time since the previous event, at most one mouse report interval.
    const uint64_t since_last=last_motion_time_ && timestamp_ns>last_motion_time_ ? timestamp_ns-last_motion_time_ : kKnifeReportNs;
    last_motion_time_=timestamp_ns;
    if(!motion_event_ && (!stroke_time_ || timestamp_ns<stroke_time_ || timestamp_ns-stroke_time_>kKnifeWindowNs)) {
        stroke_time_=timestamp_ns;stroke_lead_=std::clamp<uint64_t>(since_last,1000000,kKnifeReportNs);motion_x_=motion_y_=0;
    }
    motion_x_+=x;motion_y_+=y;
    const auto length=std::hypot(motion_x_,motion_y_);
    if(!motion_event_) {
        const double seconds=double(timestamp_ns-stroke_time_+stroke_lead_)/1e9;
        if(length<kKnifeMinLength || length/seconds<kKnifeSpeed)return;
        motion_event_=frame_.count;pulse(Action::knife);
    }
    auto& event=frame_.events[*motion_event_];
    event.direction_x=length ? float(motion_x_/length) : 0;
    event.direction_y=length ? float(motion_y_/length) : 0;
}
ActionFrame BindingResolver::consume() {
    finish_motion();
    auto result=frame_;
    frame_.pressed=frame_.released=0;frame_.count=0;canceled_releases_=0;motion_event_.reset();
    return result;
}
}
