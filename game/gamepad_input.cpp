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
// The motor command straight to the Deck controller's hidraw node (opened once, kept in `fd`). False: not a hidraw pad, not writable, or the node refused: the caller falls back to SDL.
bool deck_rumble(int& fd,SDL_Gamepad* handle,uint16_t level) {
    if(fd<0) {
        const char* path=SDL_GetGamepadPath(handle);
        if(!path || std::string_view(path).substr(0,11)!="/dev/hidraw") return false;
        fd=::open(path,O_RDWR|O_CLOEXEC);
        if(fd<0) return false;
    }
    auto report=deck_rumble_report(level);
    return ioctl(fd,HIDIOCSFEATURE(65),report.data())==int(report.size());
}
void close_hid(int& fd) { if(fd>=0) ::close(fd); fd=-1; }
#else
bool deck_rumble(int&,SDL_Gamepad*,uint16_t) { return false; }
void close_hid(int&) {}
#endif
constexpr uint64_t kDeckRumbleRefreshMs=60;   // the Deck's motor command is sent again this often while it runs
constexpr int8_t kDeckGainDb=10;
constexpr float kPress=.5f,kRelease=.4f;   // an axis counts as pressed from half and is released below 40%
constexpr float kMoved=.3f;                // a stick push that makes a pad the one in use
constexpr uint64_t kRumbleRefreshMs=200;   // a running motor is told again this often...
constexpr uint32_t kRumbleDurationMs=500;  // ...and SDL stops it this long after the last time, when its event pump runs (a main thread that hangs keeps a motor on in drivers without a hardware timeout)
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
std::array<unsigned char,65> deck_rumble_report(uint16_t level) {
    std::array<unsigned char,65> report{};
    report[1]=0xEB;   // ID_TRIGGER_RUMBLE_CMD; the header's length stays 0 and the rumble type and intensity 0 (HAPTIC_INTENSITY_SYSTEM), as in SDL's driver
    report[6]=report[8]=level&0xFF; report[7]=report[9]=level>>8;   // left and right motor speed
    report[10]=report[11]=level ? kDeckGainDb : 0;                  // left and right gain in dB
    return report;
}
GamepadInput::GamepadInput() { if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError()); }
GamepadInput::~GamepadInput() {
    rumble(0,0);
    for(auto& [id,pad]:pads_) {close_hid(pad.hid); SDL_CloseGamepad(pad.handle);}
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
void GamepadInput::capture_sink(std::function<void(PhysicalInput,bool)> sink) { sink_=std::move(sink); }
PadStatus GamepadInput::status() const {
    const auto at=pads_.find(last_);
    if(at==pads_.end()) return {};
    return {at->second.name,true,at->second.gyro,at->second.deck ? PadFamily::steam_deck : pad_family_from_type(at->second.type)};
}
void GamepadInput::rumble(unsigned motors,float strength,uint64_t now) {
    const auto level=uint16_t(std::clamp(strength,0.f,1.f)*65535.f);
    SDL_JoystickID other=pads_.count(second_) ? second_ : 0;   // the second player's pad when one is assigned, else any pad that is not the one in use
    if(!other) for(const auto& [id,pad]:pads_) if(id!=last_) {other=id;break;}
    for(auto& [id,pad]:pads_) {
        const bool on=(id==last_ ? motors&1 : id==other ? motors&2 : 0)!=0;
        const uint16_t want=on ? level : 0;
        if(want==pad.rumble && (!want || now-pad.rumble_ms<(pad.deck ? kDeckRumbleRefreshMs : kRumbleRefreshMs))) continue;
        pad.flip=pad.deck && want && want==pad.rumble && !pad.flip;   // the Deck's repeated command differs by one, so that neither SDL nor the pad takes it for the same command
        const uint16_t sent=pad.flip ? uint16_t(want-1) : want;
        const bool played=(pad.deck && deck_rumble(pad.hid,pad.handle,sent)) || SDL_RumbleGamepad(pad.handle,sent,sent,sent ? kRumbleDurationMs : 0);   // false: no motor in this pad, or its driver is busy or refuses
        if(played) {pad.rumble=want; pad.rumble_ms=now;}   // a refused command is sent again on the next pass: it must not be taken for done
        if(want && played!=pad.rumble_worked) {pad.rumble_worked=played; std::fprintf(stderr,"[gamepad] %s: rumble %s%s%s\n",pad.name.c_str(),played ? "works" : "was refused: ",played ? "" : SDL_GetError(),"");}   // told once, and again if it changes
    }
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
    close_hid(pad.hid); SDL_CloseGamepad(pad.handle);
    pads_.erase(at);
    if(second_==id) second_=0;
    if(muted_==id) muted_=0;
    if(last_==id) {   // another pad becomes the one in use, never the second player's
        last_=0;
        for(const auto& [other,pad]:pads_) if(other!=second_) {last_=other;break;}
    }
    ++changes_;
}
void GamepadInput::release_pad(const Pad& pad,GamepadTarget* target) {
    if(!target) return;
    target->host_pad_stick(pad.slot,PadSide::left,0,0); target->host_pad_stick(pad.slot,PadSide::right,0,0);
    for(unsigned b=0;b<32;++b) if(pad.buttons&(1u<<b)) target->host_button({PhysicalKind::pad_button,uint16_t(b)},false);
    for(unsigned a=0;a<16;++a) if(pad.axis_down&(1u<<a)) target->host_button({PhysicalKind::pad_axis,uint16_t(a)},false);
}
void GamepadInput::assign_second(SDL_JoystickID id) {
    if(id==second_) return;
    if(const auto old=pads_.find(second_); old!=pads_.end()) {   // the old second pad goes back to the first player: it presses nothing it held
        release_pad(old->second,second_seen_);
        if(active_ && first_seen_) feed_sticks(old->second,*first_seen_);
    }
    second_=id;
    if(const auto next=pads_.find(id); next!=pads_.end()) {
        release_pad(next->second,first_seen_);
        if(active_ && second_seen_) feed_sticks(next->second,*second_seen_);
        if(last_==id) {last_=0; for(const auto& [other,pad]:pads_) if(other!=id) {last_=other;break;}}
        ++changes_;
    }
}
void GamepadInput::press_sink(std::function<void(SDL_JoystickID)> sink) { press_sink_=std::move(sink); }
void GamepadInput::mute(SDL_JoystickID id) {
    if(id==muted_) return;
    const auto before=pads_.find(muted_);
    muted_=id;
    if(before!=pads_.end() && active_) if(auto* target=target_of(before->first,first_seen_,second_seen_)) feed_sticks(before->second,*target);   // the pad that was muted plays again: its sticks are where they are
    if(const auto now=pads_.find(id); now!=pads_.end()) release_pad(now->second,id==second_ ? second_seen_ : first_seen_);   // the pad that is muted now lets go of what it held (only its target knew)
}
void GamepadInput::report(PhysicalInput input,bool down,GamepadTarget* target) {
    if(sink_) sink_(input,down);
    if(active_ && target) target->host_button(input,down);
}
void GamepadInput::axis(SDL_JoystickID id,Pad& pad,unsigned axis,float value,GamepadTarget* first,GamepadTarget* second) {
    pad.axes[axis]=value;
    if(axis<4 && active_) if(auto* input=target_of(id,first,second)) feed_sticks(pad,*input);
    for(unsigned direction=0;direction<2;++direction) {   // 0: negative, 1: positive (the code is axis*2+direction)
        const float along=direction ? value : -value;
        const unsigned code=axis*2+direction; const uint32_t mask=1u<<code;
        const bool down=(pad.axis_down&mask)!=0;
        if(!down && along>=kPress) {   // a trigger pulled or a stick pushed past half is use of the pad (drift stays below it); the press sink may mute the pad or hand it to the second player first
            pressed(id);
            pad.axis_down|=mask; report({PhysicalKind::pad_axis,uint16_t(code)},true,target_of(id,first,second));
        }
        else if(down && along<kRelease) {pad.axis_down&=~mask; report({PhysicalKind::pad_axis,uint16_t(code)},false,target_of(id,first,second));}
    }
}
void GamepadInput::feed_sticks(const Pad& pad,GamepadTarget& input) const {
    input.host_pad_stick(pad.slot,PadSide::left,pad.axes[SDL_GAMEPAD_AXIS_LEFTX],pad.axes[SDL_GAMEPAD_AXIS_LEFTY]);
    input.host_pad_stick(pad.slot,PadSide::right,pad.axes[SDL_GAMEPAD_AXIS_RIGHTX],pad.axes[SDL_GAMEPAD_AXIS_RIGHTY]);
}
void GamepadInput::event(const SDL_Event& event,GamepadTarget* first,GamepadTarget* second) {
    first_seen_=first; second_seen_=second;
    switch(event.type) {
    case SDL_EVENT_GAMEPAD_ADDED: added(event.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: removed(event.gdevice.which,target_of(event.gdevice.which,first,second)); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const auto at=pads_.find(event.gbutton.which);
        if(at==pads_.end() || event.gbutton.button>=32) break;
        const bool down=event.gbutton.down; const uint32_t mask=1u<<event.gbutton.button;
        if(down==((at->second.buttons&mask)!=0)) break;
        if(down) pressed(at->first);   // before the state changes: the sink may hand this pad to the second player, which lets go of what it held
        at->second.buttons=down ? at->second.buttons|mask : at->second.buttons&~mask;
        if(down) use(at->first);
        report({PhysicalKind::pad_button,event.gbutton.button},down,target_of(at->first,first,second));
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
        if(down) pressed(at->first);
        at->second.buttons=down ? at->second.buttons|mask : at->second.buttons&~mask;
        if(down) use(at->first);
        report({PhysicalKind::pad_button,code},down,target_of(at->first,first,second));
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto at=pads_.find(event.gaxis.which);
        if(at==pads_.end() || event.gaxis.axis>=at->second.axes.size()) break;
        const float value=std::clamp(float(event.gaxis.value)/32767.f,-1.f,1.f);
        if(std::fabs(value)>=kMoved) use(at->first);
        axis(at->first,at->second,event.gaxis.axis,value,first,second);
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
        if(active_ && (at->first==second_ || at->first==last_)) if(auto* target=target_of(at->first,first,second)) target->host_pad_gyro(rates[0],rates[1],timestamp);   // each player's own pad aims
        break;
    }
    default: break;
    }
}
void GamepadInput::update(GamepadTarget& first,bool active,GamepadTarget* second) {
    first_seen_=&first; second_seen_=second;
    if(active==active_) return;
    active_=active;
    if(!active) {first.pad_reset(); if(second) second->pad_reset(); return;}
    for(const auto& [id,pad]:pads_) if(auto* target=target_of(id,&first,second)) feed_sticks(pad,*target);
}
}
