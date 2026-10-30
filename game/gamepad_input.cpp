#include "gamepad_input.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#ifdef __linux__
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
namespace uc {
namespace {
#ifdef __linux__
// The Steam Deck controller sends no gyro until something asks for it (Steam does for "Gyro as mouse"; SDL's driver does not): its settings message
// ID_SET_SETTINGS_VALUES (0x87) with SETTING_IMU_MODE (48) = raw accel (8) | raw gyro (0x10), as in Valve's protocol that SDL's HIDAPI driver speaks.
// Steam may reset it (opening its menu does), so the caller repeats it every couple of seconds.
void enable_deck_imu(SDL_Gamepad* handle) {
    const char* path=SDL_GetGamepadPath(handle);
    if(SDL_GetGamepadVendor(handle)!=0x28de || SDL_GetGamepadProduct(handle)!=0x1205 || !path || std::string_view(path).substr(0,11)!="/dev/hidraw") return;
    const int fd=::open(path,O_RDWR|O_CLOEXEC);
    if(fd<0) {std::fprintf(stderr,"[gamepad] cannot open %s to enable the gyro\n",path);return;}
    unsigned char report[65]={};   // report id 0, then the 64-byte message
    report[1]=0x87; report[2]=3;   // type, payload length (one 3-byte setting)
    report[3]=48; report[4]=0x18; report[5]=0;   // SETTING_IMU_MODE, value little-endian
    if(ioctl(fd,HIDIOCSFEATURE(sizeof report),report)!=int(sizeof report)) std::fprintf(stderr,"[gamepad] enabling the gyro of %s failed\n",path);
    ::close(fd);
}
#endif
constexpr float kPress=.5f,kRelease=.4f;   // an axis counts as pressed from half and is released below 40%
constexpr float kMoved=.3f;                // a stick push that makes a pad the one in use
}
PadFamily pad_family_from_type(SDL_GamepadType type) {
    switch(type) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return PadFamily::xbox;
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return PadFamily::playstation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PadFamily::nintendo;
    default: return PadFamily::xbox;   // standard, unknown and clones (an XInput pad of any make): the Xbox layout; "generic" is only chosen by hand
    }
}
GamepadInput::GamepadInput() { if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError()); }
GamepadInput::~GamepadInput() {
    for(auto& [id,pad]:pads_) SDL_CloseGamepad(pad.handle);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
void GamepadInput::capture_sink(std::function<void(PhysicalInput,bool)> sink) { sink_=std::move(sink); }
PadStatus GamepadInput::status() const {
    const auto at=pads_.find(last_);
    if(at==pads_.end()) return {};
    return {at->second.name,true,at->second.gyro,at->second.deck ? PadFamily::steam_deck : pad_family_from_type(at->second.type)};
}
void GamepadInput::added(SDL_JoystickID id) {
    if(pads_.count(id) || !SDL_IsGamepad(id)) return;
    unsigned slot=0;
    while(slot<kPadSlots && std::any_of(pads_.begin(),pads_.end(),[&](const auto& item){return item.second.slot==slot;})) ++slot;
    if(slot==kPadSlots) return;   // more pads than slots: the extra ones are ignored
    SDL_Gamepad* handle=SDL_OpenGamepad(id);
    if(!handle) return;
    // The same physical pad can show up twice (the Steam Deck: its HID interface with the gyro, and an evdev copy without it): the copy without a gyro is dropped.
    if(!SDL_GamepadHasSensor(handle,SDL_SENSOR_GYRO) && std::any_of(pads_.begin(),pads_.end(),[&](const auto& item){return item.second.gyro && SDL_GetGamepadVendor(item.second.handle)==SDL_GetGamepadVendor(handle) && SDL_GetGamepadProduct(item.second.handle)==SDL_GetGamepadProduct(handle);})) {
        std::fprintf(stderr,"[gamepad] ignoring the gyro-less copy of a pad that is already open\n");
        SDL_CloseGamepad(handle); return;
    }
    Pad pad; pad.handle=handle; pad.slot=slot;
    if(const char* name=SDL_GetGamepadName(handle)) pad.name=name;
    pad.type=SDL_GetGamepadType(handle);
    pad.deck=SDL_GetGamepadVendor(handle)==0x28de && SDL_GetGamepadProduct(handle)==0x1205;
    pad.gyro=SDL_GamepadHasSensor(handle,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(handle,SDL_SENSOR_GYRO,true);
#ifdef __linux__
    if(pad.gyro) enable_deck_imu(handle);
    // SDL's Steam Deck driver reports the axis about the screen's normal as "yaw" (data[1], right for a flat controller), but a Deck is held upright: turning it left and
    // right is the axis SDL calls data[2] (measured on the Deck: 2.2-2.9 rad/s on data[2] for sideways turns, data[1] only for steering-wheel rolls). Sign: SDL's -gyroY.
    if(SDL_GetGamepadVendor(handle)==0x28de && SDL_GetGamepadProduct(handle)==0x1205) {pad.yaw_axis=2; pad.yaw_sign=-1;}
#endif
    std::fprintf(stderr,"[gamepad] %s type=%d slot=%u gyro=%s vendor=%04x product=%04x path=%s\n",pad.name.c_str(),int(pad.type),slot,pad.gyro ? "on" : "none",
                 SDL_GetGamepadVendor(handle),SDL_GetGamepadProduct(handle),SDL_GetGamepadPath(handle) ? SDL_GetGamepadPath(handle) : "?");
    pads_.emplace(id,std::move(pad));
    last_=id; ++changes_;
}
void GamepadInput::removed(SDL_JoystickID id,GamepadTarget* input) {
    const auto at=pads_.find(id);
    if(at==pads_.end()) return;
    auto& pad=at->second;
    if(input) {   // whatever the pad held is let go
        input->host_pad_stick(pad.slot,PadSide::left,0,0); input->host_pad_stick(pad.slot,PadSide::right,0,0);
        for(unsigned b=0;b<32;++b) if(pad.buttons&(1u<<b)) report({PhysicalKind::pad_button,uint16_t(b)},false,input);
        for(unsigned a=0;a<16;++a) if(pad.axis_down&(1u<<a)) report({PhysicalKind::pad_axis,uint16_t(a)},false,input);
    }
    SDL_CloseGamepad(pad.handle);
    pads_.erase(at);
    if(last_==id) last_=pads_.empty() ? 0 : pads_.begin()->first;
    ++changes_;
}
void GamepadInput::report(PhysicalInput input,bool down,GamepadTarget* target) {
    if(sink_) sink_(input,down);
    if(active_ && target) target->host_button(input,down);
}
void GamepadInput::axis(Pad& pad,unsigned axis,float value,GamepadTarget* input) {
    pad.axes[axis]=value;
    if(axis<4 && active_ && input) feed_sticks(pad,*input);
    for(unsigned direction=0;direction<2;++direction) {   // 0: negative, 1: positive (the code is axis*2+direction)
        const float along=direction ? value : -value;
        const unsigned code=axis*2+direction; const uint32_t mask=1u<<code;
        const bool down=(pad.axis_down&mask)!=0;
        if(!down && along>=kPress) {pad.axis_down|=mask; report({PhysicalKind::pad_axis,uint16_t(code)},true,input);}
        else if(down && along<kRelease) {pad.axis_down&=~mask; report({PhysicalKind::pad_axis,uint16_t(code)},false,input);}
    }
}
void GamepadInput::feed_sticks(const Pad& pad,GamepadTarget& input) const {
    input.host_pad_stick(pad.slot,PadSide::left,pad.axes[SDL_GAMEPAD_AXIS_LEFTX],pad.axes[SDL_GAMEPAD_AXIS_LEFTY]);
    input.host_pad_stick(pad.slot,PadSide::right,pad.axes[SDL_GAMEPAD_AXIS_RIGHTX],pad.axes[SDL_GAMEPAD_AXIS_RIGHTY]);
}
void GamepadInput::event(const SDL_Event& event,GamepadTarget* input) {
    switch(event.type) {
    case SDL_EVENT_GAMEPAD_ADDED: added(event.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: removed(event.gdevice.which,input); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const auto at=pads_.find(event.gbutton.which);
        if(at==pads_.end() || event.gbutton.button>=32) break;
        const bool down=event.gbutton.down; const uint32_t mask=1u<<event.gbutton.button;
        if(down==((at->second.buttons&mask)!=0)) break;
        at->second.buttons=down ? at->second.buttons|mask : at->second.buttons&~mask;
        if(down) use(at->first);
        report({PhysicalKind::pad_button,event.gbutton.button},down,input);
        break;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {   // a trackpad pressed past the soft threshold is a button of its own (a touch is not, and the physical click is a different button)
        const auto at=pads_.find(event.gtouchpad.which);
        if(at==pads_.end() || event.gtouchpad.touchpad<0 || event.gtouchpad.touchpad>1 || event.gtouchpad.finger!=0) break;
        const uint16_t code=uint16_t(kPadTrackpadPress+event.gtouchpad.touchpad); const uint32_t mask=1u<<code;
        const float pressure=event.type==SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? 0.f : event.gtouchpad.pressure;
        auto& peak=at->second.trackpad_peak[event.gtouchpad.touchpad];
        peak=pressure>0 ? std::max(peak,pressure) : peak;
        if(event.type==SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {   // diagnostics: how hard the finger pressed at most (tunes the threshold)
            std::fprintf(stderr,"[gamepad] trackpad %d lifted, peak pressure %.3f (press at %.2f)\n",int(event.gtouchpad.touchpad),peak,kTrackpadPressOn);
            peak=0;
        }
        const bool was=(at->second.buttons&mask)!=0;
        const bool down=was ? pressure>=kTrackpadPressOff : pressure>=kTrackpadPressOn;
        if(down==was) break;
        at->second.buttons=down ? at->second.buttons|mask : at->second.buttons&~mask;
        if(down) use(at->first);
        report({PhysicalKind::pad_button,code},down,input);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto at=pads_.find(event.gaxis.which);
        if(at==pads_.end() || event.gaxis.axis>=at->second.axes.size()) break;
        const float value=std::clamp(float(event.gaxis.value)/32767.f,-1.f,1.f);
        if(std::fabs(value)>=kMoved) use(at->first);
        axis(at->second,event.gaxis.axis,value,input);
        break;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        const auto at=pads_.find(event.gsensor.which);
        if(at==pads_.end() || event.gsensor.sensor!=SDL_SENSOR_GYRO) break;
        // SDL's gyro: data[0] about the right axis (pitch, positive tilting up), data[1] about the up axis (yaw, positive turning left), rad/s.
        const uint64_t timestamp=event.gsensor.sensor_timestamp ? event.gsensor.sensor_timestamp : event.common.timestamp;   // a backend without a device clock: SDL's own
        if(!at->second.gyro_seen && (event.gsensor.data[0]!=0.f || event.gsensor.data[1]!=0.f || event.gsensor.data[2]!=0.f)) {
            at->second.gyro_seen=true;
            std::fprintf(stderr,"[gamepad] gyro data arrives from %s\n",at->second.name.c_str());   // proves the platform delivers the sensor (Steam Input may hold it back)
        }
        const auto rates=at->second.bias.filter(at->second.yaw_sign*event.gsensor.data[at->second.yaw_axis],event.gsensor.data[0],timestamp);
        ++at->second.gyro_samples;
        for(int axis=0;axis<3;++axis) at->second.gyro_peak[axis]=std::max(at->second.gyro_peak[axis],std::fabs(event.gsensor.data[axis]));
        if(timestamp-at->second.gyro_logged_ns>=2000000000ull) {   // diagnostics, every 2 s: is the gyro calibrated, and does it reach the aim
            at->second.gyro_logged_ns=timestamp;
            std::fprintf(stderr,"[gyro] peaks in the last 2 s (rad/s): data0=%.2f data1=%.2f data2=%.2f\n",at->second.gyro_peak[0],at->second.gyro_peak[1],at->second.gyro_peak[2]);
            at->second.gyro_peak[0]=at->second.gyro_peak[1]=at->second.gyro_peak[2]=0;
#ifdef __linux__
            enable_deck_imu(at->second.handle);   // again: Steam resets the mode when its menu opens
#endif
            std::fprintf(stderr,"[gyro] samples=%u calibrated=%d raw=(%.3f %.3f %.3f) rates=(%.3f %.3f) active=%d last=%d\n",at->second.gyro_samples,int(at->second.bias.calibrated()),
                         event.gsensor.data[0],event.gsensor.data[1],event.gsensor.data[2],rates[0],rates[1],int(active_),int(at->first==last_));
        }
        if(input && active_ && at->first==last_) input->host_pad_gyro(rates[0],rates[1],timestamp);
        break;
    }
    default: break;
    }
}
void GamepadInput::update(GamepadTarget& input,bool active) {
    if(active==active_) return;
    active_=active;
    if(!active) {input.pad_reset();return;}
    for(const auto& [id,pad]:pads_) feed_sticks(pad,input);
}
}
