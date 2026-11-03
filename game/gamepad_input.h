#pragma once
#include "gamepad_math.h"
#include "gamepad_target.h"
#ifdef _WIN32
#ifdef _WIN32
#include <intrin.h>   // before SDL: SDL's endian header redefines a builtin otherwise
#endif
#endif
#include <SDL3/SDL.h>
#include <array>
#include <functional>
#include <map>
#include <string>
namespace uc {
PadFamily pad_family_from_type(SDL_GamepadType);
// The Steam Deck controller's "simple rumble" feature report (report id 0, 64 bytes), as SDL's HIDAPI driver builds it but with +10 dB on both motors instead of its +2 / 0 dB: that
// felt a good deal stronger on the Deck. level 0..65535 for both motors; 0 stops them.
std::array<unsigned char,65> deck_rumble_report(uint16_t level);
// SDL gamepads (Xbox, PlayStation, Switch and everything SDL maps): events in, GamepadTarget sticks, buttons and gyro out.
// Triggers and stick directions are also "axis presses" (pad_axis inputs): pressed from half a pull or push, released below 40%.
class GamepadInput {
public:
    GamepadInput();    // starts SDL's gamepad subsystem; pads already plugged in arrive as ADDED events
    ~GamepadInput();
    GamepadInput(const GamepadInput&)=delete;
    GamepadInput& operator=(const GamepadInput&)=delete;
    // Every SDL event of the loop; the gamepad ones are used. `input` may be null (the launcher has none and only wants the capture sink).
    void event(const SDL_Event&,GamepadTarget* input);
    // Once per loop. active=false (a menu is open, the game is paused or unfocused) lets go of everything the pads held in `input`; true (again) hands the sticks
    // over again. Buttons held meanwhile never turn into presses.
    void update(GamepadTarget&,bool active);
    // Every pad button and axis press and release goes to the sink as a PhysicalInput, active or not: the binding capture of the menus.
    void capture_sink(std::function<void(PhysicalInput,bool)>);
    PadStatus status() const;
    // Once per loop. `motors` is the game's motor per player (bit 0 the first player's, bit 1 the second's), strength 0..1 how hard they run. The first player's motor
    // runs the pad in use, the second player's another pad. A pad without a motor ignores it; a motor is stopped (strength 0 or its bit clear) the moment it is no longer wanted.
    // On the Steam Deck's own controller the command goes straight to its hidraw node (louder than SDL's, and sent again every 60 ms: the motor fades by itself otherwise; SDL stays the fallback).
    void rumble(unsigned motors,float strength,uint64_t now_ms=SDL_GetTicks());
    unsigned changes() const { return changes_; }   // counts pads added or removed and the pad in use changing: status() is worth asking again
private:
    struct Pad {
        SDL_Gamepad* handle=nullptr; unsigned slot=0; std::string name; SDL_GamepadType type=SDL_GAMEPAD_TYPE_UNKNOWN; float trackpad_peak[2]={}; bool deck=false; /* the Steam Deck's own controller: its prompts are its own buttons, not an Xbox's */ bool gyro=false, gyro_seen=false; unsigned gyro_samples=0; uint64_t gyro_logged_ns=0; float gyro_peak[3]={};
        unsigned yaw_axis=1; float yaw_sign=1;   // which SDL gyro axis turns the aim sideways (see GamepadInput::added)   // gyro_seen: a non-zero sample arrived (logged once)
        std::array<float,6> axes{}; uint32_t buttons=0,axis_down=0; GyroBias bias;
        uint16_t rumble=0; uint64_t rumble_ms=0; bool rumble_worked=true;   // what the motors were last told to do, and when; whether the pad took the last start
        int hid=-1; bool flip=false;   // the Deck's hidraw node, opened when first needed; the repeated command that differs by one (see rumble())
    };
    void added(SDL_JoystickID);
    void removed(SDL_JoystickID,GamepadTarget*);
    void report(PhysicalInput,bool,GamepadTarget*);
    void axis(Pad&,unsigned axis,float value,GamepadTarget*);
    void feed_sticks(const Pad&,GamepadTarget&) const;
    std::map<SDL_JoystickID,Pad> pads_;
    void use(SDL_JoystickID id) { if(last_!=id) {last_=id;++changes_;} }
    SDL_JoystickID last_=0;   // the pad used last: its gyro is the one that aims
    unsigned changes_=0;
    bool active_=true;
    std::function<void(PhysicalInput,bool)> sink_;
};
}
