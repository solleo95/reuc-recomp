#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace uc {
// A mouse or light gun that is read as a device of its own (local two-player play: each player's pointer is a different device). Platform backends produce these (evdev on
// Linux, Raw Input on Windows); the game turns them into the SDL mouse events HostInput already understands, so a second player's pointer follows the same path as the first's.
struct PointerEvent {
    enum class Kind:uint8_t { button,relative,absolute,wheel } kind=Kind::button;   // wheel: x is the notches turned, positive away from the user (a mouse read as a device of its own loses the system's wheel message)
    uint64_t device=0;     // DeviceId::id of a DeviceKind::pointer
    uint8_t button=0;      // SDL_BUTTON_* numbering: 1 left, 2 middle, 3 right, 4 x1, 5 x2
    bool down=false;
    float x=0,y=0;         // relative: counts; absolute: 0..1 of the window, y down (the position the gun points at, not a distance moved)
};
class PointerBackend {
public:
    virtual ~PointerBackend()=default;
    // Everything that arrived since the last call, in order, and the ids of devices that went away (their held buttons are released in `out` first).
    virtual void poll(std::vector<PointerEvent>& out,std::vector<uint64_t>& removed)=0;
    virtual bool capture(uint64_t device,bool on)=0;          // keep the device away from the OS and SDL (EVIOCGRAB on Linux); false when that is not possible
    virtual std::string status() const=0;                     // "" when well; why devices cannot be read ("no access to /dev/input")
    // True while the backend hands over every pointer device, not only the captured ones (Windows: capturing takes the legacy mouse messages away from everybody): the caller then
    // ignores the mouse events SDL still delivers and feeds the first player's pointer from poll() as well.
    virtual bool all_devices() const { return false; }
    // Once per loop: whether SDL is reading the pointer as the cursor's position (light gun mode) and whether the window has the pointer (the port's menu is closed; the game's
    // own menus count: they read the pointer like a mission does). Windows needs both: SDL's own mouse reading (relative mode) must not be replaced by ours unless a pointer is
    // captured, and the legacy mouse messages are only taken away while the window has the pointer (the port's menu needs the cursor; otherwise any gun would move it).
    virtual void hint(bool /*sdl_absolute*/,bool /*pointer_held*/) {}
};
}
