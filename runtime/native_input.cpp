#include "native_input.h"
#include "gx_vertices.h"
#include "gx_audit.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>

namespace {
uc::NativeInput* input; // set before guest execution; cleared after all guest threads join
uc::InputProbe input_probe;
uc::InputMap input_map;
uint64_t motion_time() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
void check(ppc::Ctx& c,uint32_t address,size_t size) {
    if (!address) throw gx::Error(gx::ErrorCode::array_bounds,address,size,0);
    gx::Memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}}.read(address,size);
}
}
namespace uc {
void input_hle_set_probe(InputProbe probe) { input_probe=probe; }
void input_hle_set_map(InputMap map) { input_map=map; }
NativeInput::NativeInput() { bindings(default_bindings()); }
void NativeInput::bindings(const Bindings& bindings) {
    if(auto error=validate_bindings(bindings))throw std::invalid_argument(*error);
    auto game=bindings,service=bindings;
    for(size_t i=0;i<bindings.size();++i) {
        const bool is_service=i==size_t(Action::release_cursor) || i==size_t(Action::screenshot);
        (is_service ? game : service)[i].clear();
    }
    std::lock_guard lock(mutex_);
    auto next_game=actions_,next_service=services_;
    next_game.replace(game); next_service.replace(service);
    actions_=std::move(next_game); services_=std::move(next_service); ++generation_;
}
void NativeInput::context(InputContext context) {
    std::lock_guard lock(mutex_); actions_.context(context);
    if(context_!=context) { context_=context; ++generation_; }
}
void NativeInput::qte_window(bool active) {std::lock_guard lock(mutex_);actions_.qte_window(active);}
void NativeInput::host_button(PhysicalInput button,bool down) {
    std::lock_guard lock(mutex_);
    try { actions_.button(button,down); services_.button(button,down); }
    catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::host_wheel(int steps) {
    std::lock_guard lock(mutex_);
    try { actions_.wheel(steps); services_.wheel(steps); }
    catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::host_motion(float x,float y,uint64_t timestamp_ns) {
    std::lock_guard lock(mutex_);
    try { actions_.motion(x,y,timestamp_ns ? timestamp_ns : motion_time()); }
    catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::cancel_host() {
    ++generation_;
    actions_.focus(false); services_.focus(false);
    state_.valid=false; state_.released|=state_.held; state_.held=state_.pressed=0;
}
ActionFrame NativeInput::consume_actions() {
    std::lock_guard lock(mutex_);
    if(host_sequence_==sequence_)consumed_sequence_=sequence_;
    auto result=actions_.consume(); result.generation=generation_; return result;
}
ActionFrame NativeInput::consume_service_actions() { std::lock_guard lock(mutex_); return services_.consume(); }
void NativeInput::focus(bool focused) {
    std::lock_guard lock(mutex_);
    state_.valid=focused;
    actions_.focus(focused); services_.focus(focused);
    if(!focused)++generation_;
    if (!focused) { state_.released|=state_.held; state_.held=0; state_.pressed=0; }
}
bool NativeInput::move_raw(float dx,float dy,uint32_t width,uint32_t height) {
    if (!width || !height || !std::isfinite(dx) || !std::isfinite(dy)) return false;
    std::lock_guard lock(mutex_);
    if (!state_.valid) return false;
    state_.x=std::clamp(state_.x+2*dx/width,-1.0f,1.0f);
    state_.y=std::clamp(state_.y+2*dy/height,-1.0f,1.0f);
    return true;
}
void NativeInput::button(uint32_t mask,bool down) {
    std::lock_guard lock(mutex_);
    if (down && state_.valid) { state_.pressed|=mask&~state_.held; state_.held|=mask; }
    else if (!down) { state_.released|=mask&state_.held; state_.held&=~mask; }
}
bool NativeInput::aim(float x,float y) {
    if(!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1) return false;
    std::lock_guard lock(mutex_);
    if(!state_.valid) return false;
    state_.x=x; state_.y=y; return true;
}
InputSample NativeInput::consume() {
    std::lock_guard lock(mutex_);
    auto result=state_;
    result.dx=result.x-previous_x_; result.dy=result.y-previous_y_;
    previous_x_=result.x; previous_y_=result.y;
    state_.pressed=state_.released=0;
    if(host_sequence_!=sequence_)consumed_sequence_=sequence_;
    return result;
}
bool NativeInput::apply_test_command(uint64_t sequence,float x,float y,uint32_t held) {
    if(!sequence || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1 || (held&~0x1C10u)) return false;
    std::lock_guard lock(mutex_);
    if(!state_.valid || sequence<=sequence_) return false;
    actions_.focus(false); services_.focus(false); actions_.focus(true); services_.focus(true);
    ++generation_;
    legacy_test_input_=true;
    state_.x=x; state_.y=y;
    state_.pressed|=held&~state_.held; state_.released|=state_.held&~held;
    state_.held=held; sequence_=sequence; return true;
}
bool NativeInput::using_host_input() { std::lock_guard lock(mutex_); return !legacy_test_input_; }
uint64_t NativeInput::generation() { std::lock_guard lock(mutex_); return generation_; }
bool NativeInput::apply_test_event(uint64_t sequence,const HostEvent& event,uint32_t width,uint32_t height) {
    if(!sequence || !std::isfinite(event.x) || !std::isfinite(event.y))return false;
    if(event.kind!=HostEventKind::motion && (event.x!=0 || event.y!=0))return false;
    switch(event.kind) {
    case HostEventKind::key: if(!event.code || event.code>=512 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::mouse: if(!event.code || event.code>=256 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::wheel: if((event.code!=1 && event.code!=2) || event.value<=0 || event.value>256)return false; break;
    case HostEventKind::motion: if(event.code || event.value || !width || !height)return false; break;
    case HostEventKind::focus: if(event.code || event.value<0 || event.value>1)return false; break;
    default: return false;
    }
    std::lock_guard lock(mutex_);
    const bool release=(event.kind==HostEventKind::key || event.kind==HostEventKind::mouse) && event.value==0;
    if(sequence<=sequence_ || (!state_.valid && event.kind!=HostEventKind::focus && !release))return false;
    try {
        switch(event.kind) {
        case HostEventKind::key:
        case HostEventKind::mouse: {
            const PhysicalInput button{event.kind==HostEventKind::key ? PhysicalKind::key : PhysicalKind::mouse,uint16_t(event.code)};
            actions_.button(button,event.value!=0); services_.button(button,event.value!=0); break;
        }
        case HostEventKind::wheel: {
            const int steps=event.code==1 ? event.value : -event.value;
            actions_.wheel(steps); services_.wheel(steps); break;
        }
        case HostEventKind::motion:
            state_.x=std::clamp(state_.x+2*event.x/width,-1.f,1.f);
            state_.y=std::clamp(state_.y+2*event.y/height,-1.f,1.f);
            actions_.motion(event.x,event.y,motion_time()); break;
        case HostEventKind::focus:
            state_.valid=event.value!=0;
            actions_.focus(state_.valid); services_.focus(state_.valid);
            if(!state_.valid)cancel_host();
            break;
        }
    } catch(const std::overflow_error&) { cancel_host(); throw; }
    sequence_=host_sequence_=sequence; legacy_test_input_=false;
    return true;
}
uint64_t NativeInput::last_consumed_sequence() { std::lock_guard lock(mutex_); return consumed_sequence_; }
void input_hle_init(NativeInput* source) { input=source; }
}
void hle_KPADRead(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802F21EC,c);
    const auto channel=c.r[3],buffer=c.r[4],count=c.r[5]; c.r[3]=0;
    if (!input || channel!=0 || !count || count>16) return;
    try { check(c,buffer,132); } catch (const gx::Error&) { return; }
    auto sample=input->consume();
    if(input_map)sample=input_map(c,sample);
#ifdef UC_TRACE
    if (sample.pressed) std::fprintf(stderr,"[input] pressed %08X aim %.3f %.3f\n",sample.pressed,sample.x,sample.y);
#endif
    std::memset(c.mem+(buffer&0x3FFFFFFF),0,132);
    ppc::wr32(c,buffer,sample.held); ppc::wr32(c,buffer+4,sample.pressed); ppc::wr32(c,buffer+8,sample.released);
    ppc::wrf32(c,buffer+0x20,sample.x); ppc::wrf32(c,buffer+0x24,sample.y);
    ppc::wrf32(c,buffer+0x28,sample.dx); ppc::wrf32(c,buffer+0x2C,sample.dy);
    ppc::wrf32(c,buffer+0x30,std::hypot(sample.dx,sample.dy));
    // shortcut: mouse has no motion sensor; neutral gravity/horizon/distance until native action mapping (TD-041).
    ppc::wrf32(c,buffer+0x10,-1); ppc::wrf32(c,buffer+0x18,1);
    ppc::wrf32(c,buffer+0x34,1); ppc::wrf32(c,buffer+0x48,1); ppc::wrf32(c,buffer+0x58,-1);
    ppc::wr8(c,buffer+0x5E,sample.valid ? 1 : 0); ppc::wr8(c,buffer+0x5F,2);
    c.r[3]=1;
    if(input_probe) input_probe(c,sample);
}
void hle_WPADProbe(ppc::Ctx& c) {
    const auto channel=c.r[3],type=c.r[4];
    const bool connected=input && channel==0;
    if (channel>3) { c.r[3]=uint32_t(-4); return; }
    if (type) {
        try { check(c,type,4); } catch (const gx::Error&) { c.r[3]=uint32_t(-4); return; }
        ppc::wr32(c,type,connected ? 0 : 0xFD);
    }
    c.r[3]=connected ? 0 : uint32_t(-1);
}
