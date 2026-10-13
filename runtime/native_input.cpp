#include "native_input.h"
#include "gx_vertices.h"
#include "gx_audit.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace {
uc::NativeInput* input; // set before guest execution; cleared after all guest threads join
void check(ppc::Ctx& c,uint32_t address,size_t size) {
    if (!address) throw gx::Error(gx::ErrorCode::array_bounds,address,size,0);
    gx::Memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}}.read(address,size);
}
}
namespace uc {
void NativeInput::focus(bool focused) {
    std::lock_guard lock(mutex_);
    state_.valid=focused;
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
    consumed_sequence_=sequence_;
    return result;
}
bool NativeInput::apply_test_command(uint64_t sequence,float x,float y,uint32_t held) {
    if(!sequence || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1 || (held&~0x1C10u)) return false;
    std::lock_guard lock(mutex_);
    if(!state_.valid || sequence<=sequence_) return false;
    state_.x=x; state_.y=y;
    state_.pressed|=held&~state_.held; state_.released|=state_.held&~held;
    state_.held=held; sequence_=sequence; return true;
}
uint64_t NativeInput::last_consumed_sequence() { std::lock_guard lock(mutex_); return consumed_sequence_; }
void input_hle_init(NativeInput* source) { input=source; }
}
void hle_KPADRead(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802F21EC,c);
    const auto channel=c.r[3],buffer=c.r[4],count=c.r[5]; c.r[3]=0;
    if (!input || channel!=0 || !count || count>16) return;
    try { check(c,buffer,132); } catch (const gx::Error&) { return; }
    const auto sample=input->consume();
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
