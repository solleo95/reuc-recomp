#pragma once
#include "gamepad_math.h"
#include "gamepad_target.h"
#ifdef _WIN32
#include <intrin.h>   // before SDL: SDL's endian header redefines a builtin otherwise
#endif
#include <SDL3/SDL.h>
#include <array>
#include <functional>
#include <map>
#include <string>
namespace uc {
PadFamily pad_family_from_type(SDL_GamepadType);
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
    unsigned changes() const { return changes_; }   // counts pads added or removed and the pad in use changing: status() is worth asking again
private:
    struct Pad {
        SDL_Gamepad* handle=nullptr; unsigned slot=0; std::string name; SDL_GamepadType type=SDL_GAMEPAD_TYPE_UNKNOWN; bool gyro=false;
        std::array<float,6> axes{}; uint32_t buttons=0,axis_down=0; GyroBias bias;
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
