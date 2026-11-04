#pragma once
#include "check.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <vector>
// A virtual SDL gamepad: the real SDL path (events, mapping, sensors) without a device.
struct Virtual {
    SDL_JoystickID id=0; SDL_Joystick* joystick=nullptr;
    std::vector<int> rumbles;   // the strength of every rumble the pad was told to play, 0..65535
    bool refuse=false;          // the pad's driver turns the rumble down (a busy device)
    static bool rumble_to(void* self,Uint16 low,Uint16 high) { auto* pad=static_cast<Virtual*>(self); pad->rumbles.push_back(std::max(low,high)); return !pad->refuse; }
    explicit Virtual(const char* name="Test Pad",uint16_t vendor=0,uint16_t product=0) {
        SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
        desc.userdata=this; desc.Rumble=rumble_to; desc.vendor_id=vendor; desc.product_id=product;
        const SDL_VirtualJoystickSensorDesc sensors[1]={{SDL_SENSOR_GYRO,250.f}};
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        desc.nsensors=1; desc.sensors=sensors; desc.name=name;
        static const SDL_VirtualJoystickTouchpadDesc touchpads[2]={{1,{}},{1,{}}};   // two trackpads (the Steam Deck's), one finger each
        desc.ntouchpads=2; desc.touchpads=touchpads;
        id=SDL_AttachVirtualJoystick(&desc); CHECK(id);
        joystick=SDL_OpenJoystick(id); CHECK(joystick);
    }
    ~Virtual() { detach(); }
    void detach() { if(!joystick)return; SDL_CloseJoystick(joystick); joystick=nullptr; SDL_DetachVirtualJoystick(id); }
    void button(SDL_GamepadButton b,bool down) { CHECK(SDL_SetJoystickVirtualButton(joystick,b,down)); }
    void axis(SDL_GamepadAxis a,int value) { CHECK(SDL_SetJoystickVirtualAxis(joystick,a,Sint16(value))); }
    void trigger(SDL_GamepadAxis a,float pull) { axis(a,int(std::lround(pull*65535.f))-32768); }   // joysticks report triggers from -32768 (released) to 32767; the gamepad layer maps that to 0..32767
    void touch(int touchpad,bool down,float pressure=.05f) { CHECK(SDL_SetJoystickVirtualTouchpad(joystick,touchpad,0,down,.5f,.5f,pressure)); }
    void gyro(float yaw,float pitch,uint64_t timestamp_ns) { const float data[3]={pitch,yaw,0}; CHECK(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,timestamp_ns,data,3)); }
};
