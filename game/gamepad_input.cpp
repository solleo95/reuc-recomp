#include "gamepad_input.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
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
    return {at->second.name,true,at->second.gyro,pad_family_from_type(at->second.type)};
}
void GamepadInput::added(SDL_JoystickID id) {
    if(pads_.count(id) || !SDL_IsGamepad(id)) return;
    unsigned slot=0;
    while(slot<kPadSlots && std::any_of(pads_.begin(),pads_.end(),[&](const auto& item){return item.second.slot==slot;})) ++slot;
    if(slot==kPadSlots) return;   // more pads than slots: the extra ones are ignored
    SDL_Gamepad* handle=SDL_OpenGamepad(id);
    if(!handle) return;
    Pad pad; pad.handle=handle; pad.slot=slot;
    if(const char* name=SDL_GetGamepadName(handle)) pad.name=name;
    pad.type=SDL_GetGamepadType(handle);
    pad.gyro=SDL_GamepadHasSensor(handle,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(handle,SDL_SENSOR_GYRO,true);
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
        const auto rates=at->second.bias.filter(event.gsensor.data[1],event.gsensor.data[0],timestamp);
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
