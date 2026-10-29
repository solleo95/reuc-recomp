#include "input_bindings.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
constexpr uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
enum class Device { keyboard,pad,wii,both };   // which inputs a table may hold
bool valid(PhysicalInput input,Device device,bool wheel=true) {
    const bool desk=device==Device::keyboard || device==Device::both,gamepad=device==Device::pad || device==Device::both,remote=device==Device::wii || device==Device::both;
    switch(input.kind) {
    case PhysicalKind::key: return desk && input.code>0 && input.code<512;
    case PhysicalKind::mouse: return desk && input.code>0 && input.code<256;
    case PhysicalKind::wheel: return desk && wheel && (input.code==1 || input.code==2);
    case PhysicalKind::pad_button: return gamepad && input.code<32;
    case PhysicalKind::pad_axis: return gamepad && input.code<16;
    case PhysicalKind::wii_button: return remote && input.code<kWiiButtonCount;
    }
    return false;
}
// Slot of a held button in BindingResolver::down_; SIZE_MAX for inputs that are not held (the wheel).
size_t slot_of(PhysicalInput input) {
    switch(input.kind) {
    case PhysicalKind::key: return input.code;
    case PhysicalKind::mouse: return 512+input.code;
    case PhysicalKind::pad_button: return 768+input.code;
    case PhysicalKind::pad_axis: return 800+input.code;
    case PhysicalKind::wii_button: return 816+input.code;
    case PhysicalKind::wheel: break;
    }
    return SIZE_MAX;
}
Binding key(uint16_t code) { return {BindingKind::physical,{PhysicalKind::key,code},{}}; }
Binding mouse(uint16_t code) { return {BindingKind::physical,{PhysicalKind::mouse,code},{}}; }
Binding pad_button(uint16_t code) { return {BindingKind::physical,{PhysicalKind::pad_button,code},{}}; }
Binding pad_axis(uint16_t code) { return {BindingKind::physical,{PhysicalKind::pad_axis,code},{}}; }
Binding wii_button(uint16_t code) { return {BindingKind::physical,{PhysicalKind::wii_button,code},{}}; }
std::optional<std::string> validate(const Bindings& bindings,Device device) {
    for(size_t a=0;a<bindings.size();++a) {
        const auto& entries=bindings[a];
        if(entries.size()>64) return "too many bindings for action";
        for(size_t i=0;i<entries.size();++i) {
            const auto& b=entries[i];
            if(b.kind==BindingKind::physical) {
                if(!valid(b.input,device) || b.modifiers.size()>4) return "invalid physical binding";
                if(a==size_t(Action::menu) && (b.input.kind!=PhysicalKind::key || !b.modifiers.empty())) return "the menu binding must be a single key";
                for(size_t m=0;m<b.modifiers.size();++m) {
                    if(!valid(b.modifiers[m],device,false) || b.modifiers[m]==b.input) return "invalid binding modifier";
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
    result[size_t(Action::menu)]={key(73)};   // Insert
    return result;
}
Bindings default_pad_bindings() {
    // SDL positions: south 0, east 1, west 2, north 3, back 4, start 6, left stick 7, right stick 8, shoulders 9/10; axes: left trigger 9, right trigger 11.
    Bindings result;
    result[size_t(Action::interact)]={pad_button(0)};
    result[size_t(Action::fire)]={pad_axis(11)};
    result[size_t(Action::weapon_previous)]={pad_button(9)};
    result[size_t(Action::weapon_next)]={pad_button(10)};
    result[size_t(Action::reload)]={pad_button(2)};
    // No special kinds here: the interact+fire throw, the swing and the shake are switched by the keyboard table's entries for them, for every device (the gyro swings and shakes like a mouse).
    result[size_t(Action::grenade)]={pad_button(3)};
    result[size_t(Action::knife)]={pad_button(1)};
    result[size_t(Action::pause)]={pad_button(6)};
    result[size_t(Action::confirm)]={pad_button(0)};
    result[size_t(Action::back)]={pad_button(1)};
    result[size_t(Action::screenshot)]={pad_button(4)};
    result[size_t(Action::qte_shake)]={pad_button(8)};
    result[size_t(Action::recenter)]={pad_button(7)};
    return result;
}
Bindings default_wii_bindings() {
    // WiiButton codes: left 0, right 1, down 2, up 3, plus 4, two 5, one 6, B 7, A 8, minus 9, home 10, nunchuk Z 11, nunchuk C 12. B is the trigger; the swing, the shake and
    // the interact+fire throw come from the keyboard table's special entries (the remote swings and shakes like a mouse).
    Bindings result;
    result[size_t(Action::interact)]={wii_button(8)};
    result[size_t(Action::fire)]={wii_button(7)};
    result[size_t(Action::weapon_previous)]={wii_button(9),wii_button(0)};
    result[size_t(Action::weapon_next)]={wii_button(4),wii_button(1)};
    result[size_t(Action::reload)]={wii_button(6),wii_button(12)};
    result[size_t(Action::grenade)]={wii_button(5)};
    result[size_t(Action::knife)]={wii_button(11)};
    result[size_t(Action::pause)]={wii_button(10)};
    result[size_t(Action::confirm)]={wii_button(8)};
    result[size_t(Action::back)]={wii_button(7)};
    return result;
}
std::optional<std::string> validate_bindings(const Bindings& bindings) { return validate(bindings,Device::keyboard); }
std::optional<std::string> validate_pad_bindings(const Bindings& bindings) { return validate(bindings,Device::pad); }
std::optional<std::string> validate_wii_bindings(const Bindings& bindings) { return validate(bindings,Device::wii); }
Bindings merge_bindings(const Bindings& keyboard,const Bindings& pad,const Bindings& wii) {
    Bindings result=keyboard;
    for(const Bindings* table:{&pad,&wii}) for(size_t a=0;a<result.size();++a) for(const auto& binding:(*table)[a])
        if(std::find(result[a].begin(),result[a].end(),binding)==result[a].end()) result[a].push_back(binding);
    return result;
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
    const auto slot=slot_of(input);
    return slot!=SIZE_MAX && down_[slot];
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
    blocked_|=down_; down_.reset();
    canceled_releases_|=frame_.released|frame_.held;
    frame_={}; frame_.released=canceled_releases_; raw_actions_=0; suppress_fire_=false;
    motion_event_.reset(); motion_x_=motion_y_=0;
    shake_={};
}
void BindingResolver::replace(const Bindings& bindings) {
    if(auto error=validate(bindings,Device::both)) throw std::invalid_argument(*error);
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
    if(!valid(input,Device::both,false)) throw std::invalid_argument("invalid input button");
    finish_motion();
    const auto slot=slot_of(input);
    if(blocked_[slot]) {
        if(!down) blocked_[slot]=false;
        return;
    }
    if(!focused_) return;
    const bool was=held(input); if(was==down)return;
    down_[slot]=down;
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
bool BindingResolver::down(PhysicalInput input) const { return held(input); }
void BindingResolver::release_pad() {
    for(unsigned slot=768;slot<816;++slot) {
        blocked_[slot]=false;
        if(!down_[slot]) continue;
        button(slot<800 ? PhysicalInput{PhysicalKind::pad_button,uint16_t(slot-768)} : PhysicalInput{PhysicalKind::pad_axis,uint16_t(slot-800)},false);
    }
}
void BindingResolver::release_wii() {
    for(unsigned code=0;code<kWiiButtonCount;++code) {
        blocked_[816+code]=false;
        if(down_[816+code]) button({PhysicalKind::wii_button,uint16_t(code)},false);
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
