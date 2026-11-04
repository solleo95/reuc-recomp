#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
namespace uc {
enum class DeviceKind:uint8_t { keyboard,pointer,gamepad,wiimote };   // pointer: a mouse or a light gun
struct DeviceId {
    DeviceKind kind; uint64_t id;   // id: SDL_JoystickID, evdev or HID identifier, SDL_MouseID / Raw Input handle
    bool operator==(const DeviceId&) const=default;
    bool operator<(const DeviceId& other) const { return std::tie(kind,id)<std::tie(other.kind,other.id); }
};
enum class Player:uint8_t { one,two };
// Who plays. Every device feeds the first player except the one the second player joined with. A device the first player has pressed a button on (in a menu, or
// while a second player already plays) belongs to the first player; a device nobody has pressed joins the second player with its first press in gameplay.
// Presses only: the caller never reports motion. No I/O.
class CoopRoster {
public:
    // A button press on `device`. Player::two exactly when this press made the device the second player's; nullopt otherwise.
    std::optional<Player> press(DeviceId device,bool in_gameplay);
    Player route(DeviceId device) const;
    std::optional<DeviceId> second() const { return second_; }
    bool known(DeviceId device) const { return first_.contains(device) || second_==device; }   // a player has this device
    bool has_first() const { return !first_.empty(); }                                           // the first player has a device (the keyboard is not one: it is never in the roster)
    bool remove(DeviceId device);   // the device went away: true when it was the second player's (the second player is gone); it is forgotten either way
    void release_second();          // the second player leaves: nobody has the device, it is unknown again (until somebody decides it still feeds the first player)
private:
    std::set<DeviceId> first_;
    std::optional<DeviceId> second_;
};
}
