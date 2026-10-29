#include "native_input.h"
#include "gx_vertices.h"
#include "gx_audit.h"
#include "wide_view.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace {
constexpr size_t kDemoSampleBytes=32; // Existing raw sample format; stick axes are reconstructed from recorded actions.
static_assert(offsetof(uc::InputSample,stick_x)==kDemoSampleBytes);
uc::NativeInput* input; // set before guest execution; cleared after all guest threads join
uc::NativeInput* second_input;
unsigned sampled_channel=0;
unsigned aim_channel=0;   // the pad whose pointer drives the local reticle (network play: the local player's slot)
bool aim_exact=false;   // network play: the reticle follows the mouse exactly (late_aim.h)
std::function<void(const ppc::Ctx&)> input_poll;
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
struct NativeInput::Snapshot {
    BindingResolver actions,services;
    InputSample state;
    float previous_x,previous_y;
    uint64_t sequence,host_sequence,consumed_sequence,generation;
    bool legacy;
    InputContext context;
    std::optional<InputFrame> network;
    size_t sample_at,action_at,speed_at;
    uint8_t host;
    uint64_t demo_generation,speed_calls;
    uint32_t speed_now,speed_last;
    std::array<int8_t,2> camera;
};
std::shared_ptr<const NativeInput::Snapshot> NativeInput::checkpoint() {
    std::lock_guard lock(mutex_);
    if(demo_out_) throw std::logic_error("rollback cannot rewind an input recording");
    return std::make_shared<Snapshot>(Snapshot{actions_,services_,state_,previous_x_,previous_y_,sequence_,host_sequence_,
        consumed_sequence_,generation_,legacy_test_input_,context_,network_frame_,demo_sample_at_,demo_action_at_,demo_speed_at_,
        demo_host_,demo_generation_,speed_calls_,speed_now_,speed_last_,camera_});
}
void NativeInput::restore(const Snapshot& saved) {
    std::lock_guard lock(mutex_);
    actions_=saved.actions; services_=saved.services; state_=saved.state; previous_x_=saved.previous_x; previous_y_=saved.previous_y;
    sequence_=saved.sequence; host_sequence_=saved.host_sequence; consumed_sequence_=saved.consumed_sequence;
    generation_=saved.generation; legacy_test_input_=saved.legacy; context_=saved.context; network_frame_=saved.network;
    demo_sample_at_=saved.sample_at; demo_action_at_=saved.action_at; demo_speed_at_=saved.speed_at; demo_host_=saved.host;
    demo_generation_=saved.demo_generation; speed_calls_=saved.speed_calls; speed_now_=saved.speed_now; speed_last_=saved.speed_last;
    camera_=saved.camera;
}
NativeInput::~NativeInput() { if(demo_out_)std::fclose(demo_out_); }
void NativeInput::demo_record(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    demo_out_=_wfopen(path.c_str(),L"wb");
    if(!demo_out_)throw std::runtime_error("cannot create the input demo");
}
void NativeInput::demo_play(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    std::FILE* file=_wfopen(path.c_str(),L"rb");
    if(!file)throw std::runtime_error("cannot open the input demo");
    std::vector<uint8_t> data; uint8_t chunk[65536]; size_t got;
    while((got=std::fread(chunk,1,sizeof chunk,file))>0)data.insert(data.end(),chunk,chunk+got);
    std::fclose(file);
    const auto need=[&](size_t at,size_t size) { if(at+size>data.size())throw std::runtime_error("the input demo is truncated"); };
    uint8_t host=1; uint64_t generation=0; bool seen=false;
    for(size_t at=0;at<data.size();) {
        const Tag tag=Tag(data[at++]);
        switch(tag) {
        case Tag::sample: { need(at,kDemoSampleBytes); DemoSample item{}; std::memcpy(&item.sample,&data[at],kDemoSampleBytes); at+=kDemoSampleBytes; item.host=host; item.generation=generation; demo_samples_.push_back(item); seen=true; break; }
        case Tag::host: need(at,1); host=data[at++]; if(seen)demo_samples_.back().host=host; else demo_host_=host; break;
        case Tag::generation: need(at,8); std::memcpy(&generation,&data[at],8); at+=8; if(seen)demo_samples_.back().generation=generation; else demo_generation_=generation; break;
        case Tag::actions: {
            need(at,40); uint64_t head[5]; std::memcpy(head,&data[at],40); at+=40;
            DemoActions frame{head[0],head[1],head[2],head[3],{}};
            for(uint64_t i=0;i<head[4];++i) {
                need(at,1+11+1+8); at+=1;   // one tag byte precedes each part
                const uint8_t* event=&data[at]; at+=11; at+=1;
                float direction[2]; std::memcpy(direction,&data[at],8); at+=8;
                if(i<256)frame.events.push_back({Action(event[0]),event[1]!=0,direction[0],direction[1],event[2]!=0});
            }
            demo_actions_.push_back(std::move(frame)); break;
        }
        case Tag::camera: {
            need(at,2); if(demo_actions_.empty())throw std::runtime_error("the input demo has a camera record before any actions");
            std::memcpy(demo_actions_.back().camera.data(),&data[at],2); at+=2; break;
        }
        case Tag::speed: { need(at,12); uint64_t call; uint32_t factor; std::memcpy(&call,&data[at],8); std::memcpy(&factor,&data[at+8],4); at+=12; demo_speeds_.emplace_back(call,factor); break; }
        default: throw std::runtime_error("the input demo has an unknown record");
        }
    }
    replaying_=true;
}
uint32_t NativeInput::demo_speed(uint32_t live) {
    std::lock_guard lock(mutex_);
    const uint64_t call=speed_calls_++;
    if(replaying_) {
        while(demo_speed_at_<demo_speeds_.size() && demo_speeds_[demo_speed_at_].first<=call) speed_now_=demo_speeds_[demo_speed_at_++].second;
        return speed_now_;
    }
    if(demo_out_ && live!=speed_last_) {
        speed_last_=live;
        uint8_t record[12]; std::memcpy(record,&call,8); std::memcpy(record+8,&live,4);
        demo_put(Tag::speed,record,12);
    }
    return live;
}
uint32_t input_demo_speed(uint32_t live) { return input ? input->demo_speed(live) : live; }
void NativeInput::demo_put(Tag tag,const void* data,size_t size) {
    const uint8_t byte=uint8_t(tag);
    std::fwrite(&byte,1,1,demo_out_); std::fwrite(data,1,size,demo_out_);
}

void NativeInput::bindings(const Bindings& bindings) {
    if(auto error=validate_bindings(bindings))throw std::invalid_argument(*error);
    std::lock_guard lock(mutex_);
    auto previous=keyboard_; keyboard_=bindings;
    try { install_bindings(); } catch(...) { keyboard_=std::move(previous); throw; }
}
void NativeInput::install_bindings() {
    const auto merged=merge_bindings(keyboard_,pad_.bindings,wii_.bindings);
    auto game=merged,service=merged;
    for(size_t i=0;i<merged.size();++i) {
        if(i==size_t(Action::menu)) { game[i].clear(); service[i].clear(); continue; }   // a host key (game/host_keys.h): neither the game nor the service table sees it
        const bool is_service=i==size_t(Action::release_cursor) || i==size_t(Action::screenshot);
        (is_service ? game : service)[i].clear();
    }
    auto next_game=actions_,next_service=services_;
    next_game.replace(game); next_service.replace(service);
    actions_=std::move(next_game); services_=std::move(next_service); ++generation_;
}
void NativeInput::extent(uint32_t width,uint32_t height) {
    if(!width || !height)return;
    std::lock_guard lock(mutex_); width_=width; height_=height;
}
void NativeInput::pad_settings(const GamepadSettings& settings) {
    if(auto error=validate_gamepad(settings))throw std::invalid_argument(*error);
    std::lock_guard lock(mutex_);
    auto previous=pad_; pad_=settings;
    try { install_bindings(); } catch(...) { pad_=std::move(previous); throw; }
    update_camera();
}
void NativeInput::update_camera() {
    int x=0,y=0;
    if(!state_.valid) {camera_={};return;}   // a released cursor: the camera stick does nothing
    for(const auto& pad:sticks_) for(unsigned side=0;side<2;++side) {
        if(pad_.camera_stick!=(side==0 ? StickRole::left : StickRole::right))continue;
        const auto axes=camera_axes(pad[side][0],-pad[side][1],pad_.camera_dead_zone);   // SDL y points down, the camera's y up
        x+=axes[0]; y+=axes[1];
    }
    const auto nunchuk=camera_axes(wii_stick_[0],-wii_stick_[1],0);   // the Wii Remote's own stick: its dead zone was applied by the reader
    x+=nunchuk[0]; y+=nunchuk[1];
    camera_={int8_t(std::clamp(x,-127,127)),int8_t(std::clamp(y,-127,127))};
}
void NativeInput::wiimote_settings(const WiimoteSettings& settings) {
    if(auto error=validate_wiimote(settings))throw std::invalid_argument(*error);
    std::lock_guard lock(mutex_);
    auto previous=wii_; wii_=settings;
    try { install_bindings(); } catch(...) { wii_=std::move(previous); throw; }
}
void NativeInput::host_wii_pointer(float x,float y,uint64_t) {
    if(!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1)throw std::invalid_argument("invalid Wii Remote pointer");
    std::lock_guard lock(mutex_);
    if(!state_.valid)return;
    set_wii_pointer(x,y);
}
void NativeInput::set_wii_pointer(float x,float y) {
    // Smoothed like any absolute pointer, but it never feeds the swing detectors: the camera losing and finding the sensor bar again is a jump, not a knife.
    aim_toward(x,y,wii_.smoothing,!have_wii_pointer_);
    have_wii_pointer_=true;
}
// The pointer goes to (x,y): at once, or with `smoothing` (0..0.9, the share of the distance that is left after one 60 Hz step) eased over time. The easing runs on the
// clock, not on the arrival of events: a gun or a camera that stops sending still brings the pointer all the way.
void NativeInput::aim_toward(float x,float y,float smoothing,bool take_now) {
    target_x_=x; target_y_=y; target_smoothing_=smoothing;
    if(take_now || smoothing<=0) { state_.x=x; state_.y=y; target_on_=false; note_aim(); return; }
    if(!target_on_) { target_on_=true; eased_ns_=motion_time(); return; }   // the easing starts from the pointer's place now
    ease_pointer(motion_time());
}
void NativeInput::ease_pointer(uint64_t now_ns) {
    if(!target_on_ || !state_.valid) return;
    const double seconds=eased_ns_ && now_ns>eased_ns_ ? std::min(1.0,double(now_ns-eased_ns_)/1e9) : 0.0;
    eased_ns_=now_ns;
    if(seconds==0) return;
    const float keep=float(std::pow(double(target_smoothing_),seconds*60));   // the share of the distance that remains
    state_.x=target_x_+(state_.x-target_x_)*keep; state_.y=target_y_+(state_.y-target_y_)*keep;
    if(std::abs(state_.x-target_x_)<1e-4f && std::abs(state_.y-target_y_)<1e-4f) { state_.x=target_x_; state_.y=target_y_; target_on_=false; }
    note_aim();
}
void NativeInput::host_wii_stick(float x,float y) {
    if(!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1)throw std::invalid_argument("invalid nunchuk stick");
    std::lock_guard lock(mutex_);
    wii_stick_={x,y};
    update_camera();
}
void NativeInput::host_wii_motion(float dx,float dy,uint64_t timestamp_ns) {
    if(!std::isfinite(dx) || !std::isfinite(dy))throw std::invalid_argument("invalid Wii Remote motion");
    std::lock_guard lock(mutex_);
    try { actions_.motion(dx,dy,timestamp_ns ? timestamp_ns : motion_time()); }
    catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::wii_reset() {
    std::lock_guard lock(mutex_);
    wii_stick_={}; have_wii_pointer_=false; update_camera();
    try { actions_.release_wii(); services_.release_wii(); } catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::host_pad_stick(unsigned pad,PadSide side,float x,float y) {
    if(pad>=kPadSlots || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1)throw std::invalid_argument("invalid gamepad stick");
    std::lock_guard lock(mutex_);
    sticks_[pad][size_t(side)]={x,y};
    update_camera();
}
void NativeInput::step_pointer(uint64_t now_ns) {
    const double dt=step_ns_ && now_ns>step_ns_ ? std::min(.1,double(now_ns-step_ns_)/1e9) : 0.0;   // a hitch or a pause counts as a tenth of a second, never a jump
    step_ns_=now_ns;
    if(!state_.valid || dt==0)return;
    float vx=0,vy=0;
    for(const auto& pad:sticks_) for(unsigned side=0;side<2;++side) {
        if(pad_.aim_stick!=(side==0 ? StickRole::left : StickRole::right))continue;
        const auto v=stick_aim_velocity(pad[side][0],pad[side][1],pad_.aim);   // each pad has its own dead zone: an idle drifting pad adds nothing
        vx+=v[0]; vy+=v[1];
    }
    if(vx==0 && vy==0)return;
    state_.x=std::clamp(state_.x+float(2*vx*dt*height_/width_),-1.0f,1.0f);
    state_.y=std::clamp(state_.y+float(2*vy*dt),-1.0f,1.0f);
    note_aim();
}
void NativeInput::host_pad_gyro(float yaw,float pitch,uint64_t timestamp_ns) {
    if(!std::isfinite(yaw) || !std::isfinite(pitch))throw std::invalid_argument("invalid gyro rate");
    std::lock_guard lock(mutex_);
    try { gyro_motion(yaw,pitch,timestamp_ns); } catch(const std::overflow_error&) { cancel_host(); throw; }
}
void NativeInput::gyro_motion(float yaw,float pitch,uint64_t timestamp_ns) {
    if(pad_.gyro.mode==GyroMode::off || (pad_.gyro.mode==GyroMode::hold && !actions_.down(pad_.gyro_hold))) {gyro_ns_=0;return;}
    const uint64_t gap=gyro_ns_ && timestamp_ns>gyro_ns_ ? timestamp_ns-gyro_ns_ : 0;
    gyro_ns_=timestamp_ns;
    if(!gap || gap>50000000ull)return;   // the first sample of a stream, or one after a pause, has no interval to integrate
    const auto counts=gyro_aim_counts(yaw,pitch,float(gap)/1e9f,pad_.gyro);
    if(counts[0]==0 && counts[1]==0)return;
    if(state_.valid) {
        state_.x=std::clamp(state_.x+2*counts[0]/width_,-1.0f,1.0f);
        state_.y=std::clamp(state_.y+2*counts[1]/height_,-1.0f,1.0f);
        note_aim();
    }
    actions_.motion(counts[0],counts[1],timestamp_ns);   // the same detectors as a mouse swing or shake
}
void NativeInput::pad_reset() {
    std::lock_guard lock(mutex_);
    sticks_={}; gyro_ns_=0; update_camera();
    try { actions_.release_pad(); services_.release_pad(); } catch(const std::overflow_error&) { cancel_host(); throw; }
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
void NativeInput::host_camera(std::array<int8_t,2> camera) {
    if(camera[0]<-127 || camera[1]<-127) throw std::invalid_argument("camera axis out of range");
    std::lock_guard lock(mutex_); camera_=camera;
}
void NativeInput::cancel_host() {
    ++generation_; camera_={}; sticks_={}; wii_stick_={}; gyro_ns_=0; have_absolute_=have_wii_pointer_=target_on_=false;
    actions_.focus(false); services_.focus(false);
    state_.valid=false; state_.released|=state_.held; state_.held=state_.pressed=0;
}
ActionFrame NativeInput::consume_actions() {
    std::lock_guard lock(mutex_);
    if(network_frame_)return network_frame_->actions;
    if(replaying_) {
        ActionFrame result;
        if(demo_action_at_>=demo_actions_.size()) { result.generation=demo_generation_; return result; }
        const auto& frame=demo_actions_[demo_action_at_++];
        result.held=frame.held; result.pressed=frame.pressed; result.released=frame.released; result.generation=frame.generation; result.count=frame.events.size();
        result.camera=frame.camera;
        std::copy(frame.events.begin(),frame.events.end(),result.events.begin());
        return result;
    }
    if(host_sequence_==sequence_)consumed_sequence_=sequence_;
    ease_pointer(motion_time());   // the reload zone looks at where the pointer is now
    auto result=actions_.consume(); result.generation=generation_; result.camera=camera_;
    if(result.pressed&(uint64_t(1)<<unsigned(Action::recenter))) {   // applied here and kept out of the frame: demos and the network carry the resulting pointer, not the action
        state_.x=state_.y=0; note_aim();
    }
    {
        constexpr uint64_t recenter=uint64_t(1)<<unsigned(Action::recenter);
        result.held&=~recenter; result.pressed&=~recenter; result.released&=~recenter;
        result.count=size_t(std::remove_if(result.events.begin(),result.events.begin()+result.count,[](const ActionEvent& e){return e.action==Action::recenter;})-result.events.begin());
    }
    apply_reload_zone(result);   // before the demo and the network see the frame: they carry the reload, not the shot
    if(demo_out_) {
        const uint64_t head[5]={result.held,result.pressed,result.released,result.generation,result.count};
        demo_put(Tag::actions,head,sizeof head);
        for(size_t i=0;i<result.count;++i) {
            const auto& e=result.events[i]; const uint8_t event[11]={uint8_t(e.action),uint8_t(e.down),uint8_t(e.pulse),0,0,0,0,0,0,0,0}; const float direction[2]={e.direction_x,e.direction_y};
            demo_put(Tag::actions,event,sizeof event); demo_put(Tag::actions,direction,sizeof direction);
        }
        if(result.camera[0] || result.camera[1])demo_put(Tag::camera,result.camera.data(),2);
    }
    return result;
}
ActionFrame NativeInput::consume_service_actions() { std::lock_guard lock(mutex_); return services_.consume(); }
void NativeInput::focus(bool focused) {
    std::lock_guard lock(mutex_);
    state_.valid=focused; have_absolute_=have_wii_pointer_=target_on_=false;
    actions_.focus(focused); services_.focus(focused);
    if(!focused) {++generation_;gyro_ns_=0;}   // the sticks keep their state (GamepadInput releases them when the window loses focus): they act again when focus is back
    update_camera();
    if (!focused) { state_.released|=state_.held; state_.held=0; state_.pressed=0; }
}
bool NativeInput::move_raw(float dx,float dy,uint32_t width,uint32_t height) {
    if (!width || !height || !std::isfinite(dx) || !std::isfinite(dy)) return false;
    std::lock_guard lock(mutex_);
    if (!state_.valid) return false;
    target_on_=false;   // the mouse takes the pointer from any easing in progress
    state_.x=std::clamp(state_.x+2*dx/width,-1.0f,1.0f);
    state_.y=std::clamp(state_.y+2*dy/height,-1.0f,1.0f);
    note_aim();
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
    target_on_=false;
    state_.x=x; state_.y=y; note_aim(); return true;
}
bool NativeInput::aim_absolute(float px,float py,uint32_t width,uint32_t height,uint64_t timestamp_ns,float gesture_scale) {
    if(!width || !height || !std::isfinite(px) || !std::isfinite(py) || !std::isfinite(gesture_scale) || gesture_scale<=0)return false;
    std::lock_guard lock(mutex_);
    return absolute_point(px,py,width,height,timestamp_ns,gesture_scale);
}
bool NativeInput::absolute_point(float px,float py,uint32_t width,uint32_t height,uint64_t timestamp_ns,float gesture_scale) {
    if(!state_.valid)return false;
    const float x=std::clamp(2*px/width-1,-1.0f,1.0f),y=std::clamp(2*py/height-1,-1.0f,1.0f);
    if(have_absolute_) {
        try { actions_.motion((px-absolute_px_)*gesture_scale,(py-absolute_py_)*gesture_scale,timestamp_ns ? timestamp_ns : motion_time()); }
        catch(const std::overflow_error&) { cancel_host(); throw; }
    }
    aim_toward(x,y,smoothing_,!have_absolute_);
    have_absolute_=true; absolute_px_=px; absolute_py_=py;
    return true;
}
void NativeInput::pointer_smoothing(float fraction) {
    if(!std::isfinite(fraction) || fraction<0 || fraction>.9f)throw std::invalid_argument("pointer smoothing must be 0..0.9");
    std::lock_guard lock(mutex_); smoothing_=fraction;
}
void NativeInput::reload_zone(ReloadZone zone,float size_fraction) {
    if(!std::isfinite(size_fraction) || size_fraction<=0 || size_fraction>.5f)throw std::invalid_argument("reload zone size must be above 0 and at most 0.5");
    std::lock_guard lock(mutex_); zone_=zone; zone_size_=size_fraction;
}
void NativeInput::apply_reload_zone(ActionFrame& frame) {
    constexpr uint64_t fire=uint64_t(1)<<unsigned(Action::fire),reload=uint64_t(1)<<unsigned(Action::reload);
    const float edge=1-2*zone_size_;   // the pointer runs -1..1 across the window
    const bool inside=zone_==ReloadZone::corner ? std::abs(state_.x)>=edge && std::abs(state_.y)>=edge : zone_==ReloadZone::edge && (std::abs(state_.x)>=edge || std::abs(state_.y)>=edge);
    const bool begin=!fire_hidden_ && inside && context_==InputContext::gameplay && (frame.pressed&fire);
    if(reload_pulse_ && !begin) {   // the reload of the previous frame ends (a new one in this frame simply stays held)
        reload_pulse_=false;
        if(!(frame.held&reload)) {   // unless the player really holds a reload button: that stays held
            frame.released|=reload;
            if(frame.count<frame.events.size()) frame.events[frame.count++]={Action::reload,false};
        }
    }
    if(begin) {
        fire_hidden_=reload_pulse_=true; frame.held|=reload; frame.pressed|=reload;
        if(frame.count<frame.events.size()) frame.events[frame.count++]={Action::reload,true};
    }
    if(!fire_hidden_) return;
    if(frame.released&fire) fire_hidden_=false;   // the shot ended in this frame (a quick click is pressed and released in one)
    frame.held&=~fire; frame.pressed&=~fire; frame.released&=~fire;
    frame.count=size_t(std::remove_if(frame.events.begin(),frame.events.begin()+frame.count,[](const ActionEvent& e){return e.action==Action::fire;})-frame.events.begin());
}
void NativeInput::note_aim() { trail_[trail_head_]={int64_t(motion_time()),state_.x,state_.y}; trail_head_=(trail_head_+1)%trail_.size(); }
std::array<float,2> NativeInput::aim_ago(double seconds) {
    std::lock_guard lock(mutex_);
    const int64_t target=int64_t(motion_time())-int64_t(seconds*1e9);
    // Newest to oldest: the first point at or before the target and the one after it bracket it. Events are sparse while the mouse rests, so a
    // target past the newest event is the latest position; past the oldest kept one, the oldest.
    const AimPoint* after=nullptr;
    for (size_t n=0;n<trail_.size();++n) {
        const auto& point=trail_[(trail_head_+trail_.size()-1-n)%trail_.size()];
        if (!point.ns) break;
        if (point.ns<=target) {
            if (!after) return {point.x,point.y};
            const double span=double(after->ns-point.ns),mix=span>0 ? double(target-point.ns)/span : 1.0;
            return {float(point.x+(after->x-point.x)*mix),float(point.y+(after->y-point.y)*mix)};
        }
        after=&point;
    }
    return after ? std::array<float,2>{after->x,after->y} : std::array<float,2>{state_.x,state_.y};
}
std::array<float,2> NativeInput::latest_aim() { std::lock_guard lock(mutex_); ease_pointer(motion_time()); return {state_.x,state_.y}; }
std::array<float,2> NativeInput::consumed_aim() { std::lock_guard lock(mutex_); return {previous_x_,previous_y_}; }
std::array<float,2> input_consumed_aim() { auto* source=aim_channel==1 ? second_input : input; return source ? source->consumed_aim() : std::array<float,2>{}; }
std::optional<std::array<float,2>> input_consumed_aim_of(unsigned channel) { auto* source=channel==0 ? input : channel==1 ? second_input : nullptr; if(!source) return std::nullopt; return source->consumed_aim(); }
void input_hle_set_aim_channel(unsigned channel) { aim_channel=channel; aim_exact=true; }
bool input_aim_exact() { return aim_exact; }
bool input_demo_playing() { return input && input->demo_playing(); }
unsigned input_aim_channel() { return aim_channel; }
InputSample NativeInput::consume() { return consume_at(motion_time()); }
InputSample NativeInput::consume_at(uint64_t now_ns) {
    std::lock_guard lock(mutex_);
    if(network_frame_)return network_frame_->sample;
    if(replaying_) {
        if(demo_sample_at_>=demo_samples_.size()) { InputSample idle; idle.valid=true; return idle; }   // the demo is over: an idle pad
        const auto& item=demo_samples_[demo_sample_at_++];
        demo_host_=item.host; demo_generation_=item.generation;
        return item.sample;
    }
    step_pointer(now_ns);
    ease_pointer(motion_time());
    auto result=state_;
    if(!gx::view_scene()) {   // a menu on a wider (taller) output: its layout is narrowed to k of the window (renderer/ultrawide.h), so the pointer the game reads grows by 1 / k
        const auto k=gx::view_factors();
        if(k.x<1.f) result.x=std::clamp(result.x/k.x,-1.f,1.f);
        if(k.y<1.f) result.y=std::clamp(result.y/k.y,-1.f,1.f);
    }
    result.dx=result.x-previous_x_; result.dy=result.y-previous_y_;
    previous_x_=result.x; previous_y_=result.y;
    state_.pressed=state_.released=0;
    if(host_sequence_!=sequence_)consumed_sequence_=sequence_;
    if(demo_out_)demo_put(Tag::sample,&result,kDemoSampleBytes);
    return result;
}
bool NativeInput::apply_test_command(uint64_t sequence,float x,float y,uint32_t held) {
    if(!sequence || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1 || (held&~0x1C10u)) return false;
    std::lock_guard lock(mutex_);
    if(!state_.valid || sequence<=sequence_) return false;
    actions_.focus(false); services_.focus(false); actions_.focus(true); services_.focus(true);
    ++generation_;
    legacy_test_input_=true;
    state_.x=x; state_.y=y; note_aim();
    state_.pressed|=held&~state_.held; state_.released|=state_.held&~held;
    state_.held=held; sequence_=sequence; return true;
}
bool NativeInput::using_host_input() {
    std::lock_guard lock(mutex_);
    if(network_frame_)return network_frame_->host;
    uint8_t flag=!legacy_test_input_;
    if(replaying_)flag=demo_host_;
    else if(demo_out_)demo_put(Tag::host,&flag,1);
    return flag!=0;
}
uint64_t NativeInput::generation() {
    std::lock_guard lock(mutex_);
    if(network_frame_)return network_frame_->generation;
    uint64_t value=generation_;
    if(replaying_)value=demo_generation_;
    else if(demo_out_)demo_put(Tag::generation,&value,8);
    return value;
}
bool NativeInput::apply_test_event(uint64_t sequence,const HostEvent& event,uint32_t width,uint32_t height) {
    if(!sequence || !std::isfinite(event.x) || !std::isfinite(event.y))return false;
    const bool analog=event.kind==HostEventKind::motion || event.kind==HostEventKind::pad_stick || event.kind==HostEventKind::pad_gyro || event.kind==HostEventKind::aim
        || event.kind==HostEventKind::wii_pointer || event.kind==HostEventKind::wii_stick || event.kind==HostEventKind::wii_motion;
    if(!analog && (event.x!=0 || event.y!=0))return false;
    switch(event.kind) {
    case HostEventKind::pad_button: if(event.code>=32 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::pad_axis: if(event.code>=16 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::pad_stick: if(event.code>1 || event.value || std::abs(event.x)>1 || std::abs(event.y)>1)return false; break;
    case HostEventKind::pad_gyro: if(event.code || event.value)return false; break;
    case HostEventKind::aim: if(event.code || event.value || !width || !height || std::abs(event.x)>1 || std::abs(event.y)>1)return false; break;
    case HostEventKind::wii_button: if(event.code>=kWiiButtonCount || event.value<0 || event.value>1)return false; break;
    case HostEventKind::wii_pointer:
    case HostEventKind::wii_stick: if(event.code || event.value || std::abs(event.x)>1 || std::abs(event.y)>1)return false; break;
    case HostEventKind::wii_motion: if(event.code || event.value)return false; break;
    case HostEventKind::key: if(!event.code || event.code>=512 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::mouse: if(!event.code || event.code>=256 || event.value<0 || event.value>1)return false; break;
    case HostEventKind::wheel: if((event.code!=1 && event.code!=2) || event.value<=0 || event.value>256)return false; break;
    case HostEventKind::motion: if(event.code || event.value || !width || !height)return false; break;
    case HostEventKind::focus: if(event.code || event.value<0 || event.value>1)return false; break;
    default: return false;
    }
    std::lock_guard lock(mutex_);
    const bool release=(event.kind==HostEventKind::key || event.kind==HostEventKind::mouse || event.kind==HostEventKind::pad_button || event.kind==HostEventKind::pad_axis
        || event.kind==HostEventKind::wii_button) && event.value==0;
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
        case HostEventKind::pad_button:
        case HostEventKind::pad_axis: {
            const PhysicalInput button{event.kind==HostEventKind::pad_button ? PhysicalKind::pad_button : PhysicalKind::pad_axis,uint16_t(event.code)};
            actions_.button(button,event.value!=0); services_.button(button,event.value!=0); break;
        }
        case HostEventKind::pad_stick:
            sticks_[0][event.code]={event.x,event.y}; update_camera(); break;
        case HostEventKind::pad_gyro:
            gyro_motion(event.x,event.y,motion_time()); break;
        case HostEventKind::wii_button: {
            const PhysicalInput button{PhysicalKind::wii_button,uint16_t(event.code)};
            actions_.button(button,event.value!=0); services_.button(button,event.value!=0); break;
        }
        case HostEventKind::wii_pointer: set_wii_pointer(event.x,event.y); break;
        case HostEventKind::wii_stick: wii_stick_={event.x,event.y}; update_camera(); break;
        case HostEventKind::wii_motion: actions_.motion(event.x,event.y,motion_time()); break;
        case HostEventKind::aim:
            absolute_point((event.x+1)/2*width,(event.y+1)/2*height,width,height,motion_time()); break;
        case HostEventKind::motion:
            state_.x=std::clamp(state_.x+2*event.x/width,-1.f,1.f);
            state_.y=std::clamp(state_.y+2*event.y/height,-1.f,1.f);
            actions_.motion(event.x,event.y,motion_time()); break;
        case HostEventKind::focus:
            state_.valid=event.value!=0;
            actions_.focus(state_.valid); services_.focus(state_.valid);
            update_camera();
            if(!state_.valid)cancel_host();
            break;
        }
    } catch(const std::overflow_error&) { cancel_host(); throw; }
    sequence_=host_sequence_=sequence; legacy_test_input_=false;
    return true;
}
uint64_t NativeInput::last_consumed_sequence() { std::lock_guard lock(mutex_); return consumed_sequence_; }
InputFrame NativeInput::capture_frame() {
    InputFrame frame; frame.sample=consume(); frame.host=using_host_input();
    frame.actions=consume_actions(); frame.generation=frame.actions.generation;
    return frame;
}
void NativeInput::install_frame(const InputFrame& frame) { std::lock_guard lock(mutex_); network_frame_=frame; previous_x_=frame.sample.x;previous_y_=frame.sample.y; }
void input_hle_init(NativeInput* source,NativeInput* second) { input=source; second_input=second; sampled_channel=0; aim_channel=0; aim_exact=false; }
void input_hle_set_poll(std::function<void(const ppc::Ctx&)> poll) { input_poll=std::move(poll); }
unsigned input_hle_channel() { return sampled_channel; }
void input_hle_restore_channel(unsigned channel) {
    if(channel>1) throw std::invalid_argument("invalid rollback input channel");
    sampled_channel=channel;
}
}
void hle_KPADRead(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802F21EC,c);
    const auto channel=c.r[3],buffer=c.r[4],count=c.r[5]; c.r[3]=0;
    auto* source=channel==0 ? input : channel==1 ? second_input : nullptr;
    if (!source || !count || count>16) return;
    try { check(c,buffer,132); } catch (const gx::Error&) { return; }
    sampled_channel=channel;
    if(channel==0 && input_poll)input_poll(c);
    auto sample=source->consume();
    if(input_map)sample=input_map(c,sample);
#ifdef UC_TRACE
    if (sample.pressed) std::fprintf(stderr,"[input] pressed %08X aim %.3f %.3f\n",sample.pressed,sample.x,sample.y);
#endif
    std::memset(c.mem+(buffer&0x3FFFFFFF),0,132);
    gx::note_write_range(buffer,132);
    ppc::wr32(c,buffer,sample.held); ppc::wr32(c,buffer+4,sample.pressed); ppc::wr32(c,buffer+8,sample.released);
    ppc::wrf32(c,buffer+0x20,sample.x); ppc::wrf32(c,buffer+0x24,sample.y);
    ppc::wrf32(c,buffer+0x28,sample.dx); ppc::wrf32(c,buffer+0x2C,sample.dy);
    ppc::wrf32(c,buffer+0x30,std::hypot(sample.dx,sample.dy));
    // shortcut: mouse has no motion sensor; neutral gravity/horizon/distance until native action mapping (TD-041).
    ppc::wrf32(c,buffer+0x10,-1); ppc::wrf32(c,buffer+0x18,1);
    ppc::wrf32(c,buffer+0x34,1); ppc::wrf32(c,buffer+0x48,1); ppc::wrf32(c,buffer+0x58,-1);
    ppc::wr8(c,buffer+0x5C,1); // Nunchuk, matching WPADProbe.
    ppc::wr8(c,buffer+0x5E,sample.valid ? 1 : 0); ppc::wr8(c,buffer+0x5F,5);
    ppc::wrf32(c,buffer+0x60,sample.stick_x); ppc::wrf32(c,buffer+0x64,sample.stick_y);
    ppc::wrf32(c,buffer+0x6C,-1); ppc::wrf32(c,buffer+0x74,1); // Neutral Nunchuk gravity.
    c.r[3]=1;
    if(channel==0 && input_probe) input_probe(c,sample);
}
void hle_WPADProbe(ppc::Ctx& c) {
    const auto channel=c.r[3],type=c.r[4];
    const bool connected=channel==0 ? input!=nullptr : channel==1 && second_input!=nullptr;
    if (channel>3) { c.r[3]=uint32_t(-4); return; }
    if (type) {
        try { check(c,type,4); } catch (const gx::Error&) { c.r[3]=uint32_t(-4); return; }
        ppc::wr32(c,type,connected ? 1 : 0xFD);
    }
    c.r[3]=connected ? 0 : uint32_t(-1);
}
