#include "wiimote_input.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace uc {
namespace {
static_assert(unsigned(WiiButton::count)==kWiiButtonCount,"the binding tables and the protocol must agree on the Wii Remote's buttons");
constexpr size_t kMaxEvents=1024;
constexpr auto kFindEvery=std::chrono::seconds(2),kSilence=std::chrono::seconds(2);   // a remote reports about 100 times a second: two quiet seconds are a lost one
uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}
WiimoteInput::WiimoteInput(Opener opener) : opener_(std::move(opener)) {
    enabled_=false;   // the first update() says whether the remote may be looked for
    thread_=std::thread([this] { run(); });
}
WiimoteInput::~WiimoteInput() {
    stop_=true;
    if(thread_.joinable()) thread_.join();
}
bool WiimoteInput::nap(unsigned ms) {
    for(unsigned waited=0;waited<ms && !stop_;waited+=10) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10u,ms-waited)));
    return !stop_;
}
void WiimoteInput::set_link(bool connected,const std::string& error) {
    std::lock_guard lock(mutex_);
    if(connected!=connected_) ++changes_;
    connected_=connected; error_=error;
    if(!connected) { events_.clear(); state_=WiiState{}; }
}
bool WiimoteInput::handshake(WiiTransport& link) {
    for(const auto& step:wii_init_sequence()) {
        if(!link.write(step.report)) return false;
        if(!nap(step.delay_ms)) return false;
    }
    const auto request=wii_status_request();
    return link.write(request);   // the answer says whether a nunchuk is plugged in
}
void WiimoteInput::publish(WiiState state,bool extension,WiiState& previous,WiiSwing& swing) {
    if(!extension) {   // the remote did not say a nunchuk is plugged in: bytes that look like one are not
        state.has_nunchuk=false; state.stick={128,128};
        state.buttons&=~((1u<<unsigned(WiiButton::nunchuk_z))|(1u<<unsigned(WiiButton::nunchuk_c)));
    }
    const uint64_t ns=now_ns();
    const auto motion=swing.feed(state.accel,ns,threshold_);
    std::lock_guard lock(mutex_);
    const uint32_t changed=state.buttons^previous.buttons;
    for(unsigned code=0;code<kWiiButtonCount;++code) if(changed&(1u<<code)) {
        if(events_.size()>=kMaxEvents) events_.pop_front();
        events_.push_back({true,uint16_t(code),(state.buttons&(1u<<code))!=0,0,0,ns});
    }
    if(motion[0]!=0 || motion[1]!=0) {
        if(events_.size()>=kMaxEvents) events_.pop_front();
        events_.push_back({false,0,false,motion[0],motion[1],ns});
    }
    state_=state; ++reports_;
    previous=state;
}
void WiimoteInput::run() {
    std::unique_ptr<WiiTransport> link;
    auto next_try=std::chrono::steady_clock::now();
    WiiState previous;
    WiiSwing swing;
    bool extension=false,announced=false;
    auto last_report=std::chrono::steady_clock::now();
    while(!stop_) {
        if(!link) {
            if(!enabled_ || std::chrono::steady_clock::now()<next_try) { nap(20); continue; }
            next_try=std::chrono::steady_clock::now()+kFindEvery;
            std::string error;
            try { link=opener_(error); } catch(const std::exception& failure) { error=failure.what(); link.reset(); }
            if(!link) { set_link(false,error); continue; }
            if(!handshake(*link)) {
                link.reset();
                if(!stop_) set_link(false,"The Wii Remote did not accept its set-up (another program may be using it)");
                continue;
            }
            previous=WiiState{}; swing=WiiSwing{}; extension=false; announced=false;
            last_report=std::chrono::steady_clock::now();
        }
        if(!enabled_) { link.reset(); set_link(false,""); continue; }
        std::vector<uint8_t> input;
        if(!link->read(input,20)) { link.reset(); set_link(false,"The Wii Remote connection was lost"); continue; }
        if(input.empty()) {
            if(std::chrono::steady_clock::now()-last_report>kSilence) {   // a handle that is open but says nothing: an empty slot of a DolphinBar, or a remote out of range
                link.reset();
                set_link(false,announced ? "The Wii Remote connection was lost" : "");
            }
            continue;
        }
        last_report=std::chrono::steady_clock::now();
        if(!announced) { announced=true; set_link(true,""); }   // connected is what has been heard, not what has been opened
        if(const auto status=wii_parse_status(input)) {   // a plugged or unplugged extension reverts the data mode: set it again
            bool sent=true;
            if(status->extension && !extension) for(const auto& step:wii_extension_steps()) { sent=sent && link->write(step.report); nap(step.delay_ms); }
            extension=status->extension;
            const auto mode=wii_reporting_report(0x37);
            sent=sent && link->write(mode);
            if(!sent) { link.reset(); set_link(false,"The Wii Remote connection was lost"); }
            continue;
        }
        WiiState state;
        if(wii_parse_report(input,state)) publish(state,extension,previous,swing);
    }
}
void WiimoteInput::capture_sink(std::function<void(PhysicalInput,bool)> sink) {
    std::lock_guard lock(mutex_);
    sink_=std::move(sink);
}
WiimoteStatus WiimoteInput::status() const {
    std::lock_guard lock(mutex_);
    return {connected_,connected_ && state_.has_nunchuk,error_};
}
void WiimoteInput::update(WiimoteTarget* target,bool active,const WiimoteSettings& settings) {
    enabled_=settings.enabled; threshold_=settings.swing_threshold;
    std::deque<Event> events; WiiState state; bool connected; uint64_t reports; std::function<void(PhysicalInput,bool)> sink;
    {
        std::lock_guard lock(mutex_);
        events.swap(events_); state=state_; connected=connected_; reports=reports_; sink=sink_;
    }
    if(sink) for(const auto& event:events) if(event.button) sink({PhysicalKind::wii_button,event.code},event.down);
    for(const auto& event:events) if(event.button && event.down) ++activity_;
    if(!target) return;
    if(changes_!=seen_changes_) {   // the remote dropped and came back since the last update: what it held is let go (its releases were lost with the link)
        seen_changes_=changes_;
        if(engaged_) { target->wii_reset(); engaged_=false; last_stick_={}; }
    }
    if(!active || !connected || !settings.enabled) {
        if(engaged_) { target->wii_reset(); engaged_=false; last_stick_={}; }
        seen_reports_=reports;
        return;   // what happened meanwhile (events drained above) never turns into a press
    }
    engaged_=true;
    try {
        for(const auto& event:events) {
            if(event.button) target->host_button({PhysicalKind::wii_button,event.code},event.down);
            else target->host_wii_motion(event.x,event.y,event.ns);
        }
    } catch(const std::overflow_error&) { engaged_=false; return; }   // the target's queue was full: it dropped its input state itself (a stalled loop piled up too much)
    if(reports==seen_reports_) return;   // nothing new from the remote
    seen_reports_=reports;
    if(const auto pointer=wii_ir_pointer(state.ir)) target->host_wii_pointer((*pointer)[0],(*pointer)[1],now_ns());   // no dots: the pointer stays where it was
    const auto stick=state.has_nunchuk ? wii_stick(state.stick,settings.stick_dead_zone) : std::array<float,2>{0,0};
    if(stick!=last_stick_) { target->host_wii_stick(stick[0],stick[1]); last_stick_=stick; }
}
}
