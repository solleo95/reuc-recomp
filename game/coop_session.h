#pragma once
#include "coop_roster.h"
#include "gamepad_input.h"
#include "host_input.h"
#include "pointer_devices.h"
#include "pointer_feed.h"
#include "wiimote_input.h"
#include <array>
#include <map>
#include <string>
namespace uc {
// Local two-player play on one computer. The game takes a second player only from its menus, never in the middle of play. A button pressed in a menu on a device nobody has
// used yet raises a question (pending()): does the first player use this device now, or does a second player join with it? The host shows it (a modal window of the menu) and
// calls answer_first() or answer_second(). The very first device used (and a keyboard player's own mouse) is the first player's without a question; in a mission an unknown
// device just feeds the first player as it always did. This decides, once per loop, whose NativeInput each device feeds: a gamepad by its id (GamepadInput::assign_second), a
// Wii Remote by the target its reader's update() is given, a mouse or light gun by the PointerBackend's device events (read as a device of its own and grabbed, so the first
// player does not see it).
// `enabled=false` (the Steam Deck's menu, network play, demos, headless and harness runs) makes every call a no-op: nothing joins.
class CoopSession {
public:
    struct Parts {
        NativeInput& first; NativeInput& second;
        GamepadInput* gamepad=nullptr;
        std::array<WiimoteInput*,2> remotes{};
        PointerBackend* pointers=nullptr;
    };
    CoopSession(Parts,bool enabled);
    ~CoopSession();
    CoopSession(const CoopSession&)=delete;
    CoopSession& operator=(const CoopSession&)=delete;
    void sensitivity(uint32_t percent);   // the mouse sensitivity of the pointers the session feeds (percent, as Options::mouse_sensitivity)
    struct Frame {
        bool in_gameplay=false;     // a game is running, not paused, not in a menu: only then a press joins a second player
        bool game_in_play=false;    // the game itself is in a mission (even if the port's menu or a pause is over it): disconnecting the second player then leaves the game as it is
        bool captured=false;        // the window has the pointer (HostInput's `captured`)
        bool pointer_absolute=false;   // SDL reads the pointer as the cursor's position (light gun mode) rather than as raw motion
        uint64_t now_ns=0;          // SDL_GetTicksNS(): the clock of SDL's event timestamps
        PointerWindow window;
    };
    void note_keyboard() { keyboard_used_=true; }   // any key press of the loop: a keyboard player aims with a mouse, so a lone (or the busiest) pointer device is theirs, not a second player's
    // Windows, where SDL's mouse id is the Raw Input device handle the backend uses too: SDL's own mouse events tell the devices apart while the backend does not listen (see
    // RawInputBackend). Motion counts as use, a button press may join. Not for platforms where the ids differ (Linux).
    void sdl_mouse(const SDL_Event&);
    void tick(const Frame&);        // once per loop: pointer devices, device losses
    // Once per loop instead of calling the remotes' update() yourself: finds which remote a press came from, routes each remote to its player's input, and updates them.
    void update_remotes(bool active,const WiimoteSettings&);
    // An SDL mouse event the first player must not see: while the pointer backend hands over every pointer device (Windows, a pointer captured) SDL's copies are ignored.
    bool swallows_mouse(const SDL_Event&) const;
    // The question about a new device. `replaces`: a second player is there already and would be replaced.
    struct NewDevice { bool asking=false,replaces=false; DeviceKind kind=DeviceKind::gamepad; };
    NewDevice pending() const;
    void answer_first();            // the first player uses the device: it is theirs and is not asked about again
    void answer_second();           // a second player joins with the device (replacing the one who is there)
    void cancel_pending();          // "not now": nothing is decided, the device asks again at its next press
    // The menu's command: the second player leaves and the device is nobody's. In a menu of the game (not over a mission) the game goes back to one player: it stops seeing a second
    // pad, which is what lets it drop the second player and the second reticle. Over a mission the second pad stays (idle): leaving mid-mission is not announced to the game.
    void release_second();
    struct Status { bool joined=false; DeviceKind kind=DeviceKind::keyboard; std::string problem; };
    Status status() const;
    bool second_is_remote(size_t reader) const { return second_ && second_->kind==DeviceKind::wiimote && reader<remotes_.size() && second_remote_==reader; }
private:
    void pointer_pressed(DeviceId);
    bool busier_pointer(uint64_t id) const;
    void pressed(DeviceId);
    void unmute_pending();
    void join(DeviceId);
    void leave();
    DeviceId remote_id(size_t reader) const;
    NativeInput& first_; NativeInput& second_input_;
    GamepadInput* gamepad_; std::array<WiimoteInput*,2> remotes_; PointerBackend* pointers_;
    bool enabled_;
    CoopRoster roster_;
    std::optional<DeviceId> second_;
    std::optional<DeviceId> pending_;   // the device a question is open about
    size_t second_remote_=0;
    Frame frame_;
    HostInput first_host_,second_host_;   // the pointer paths: first_host_ only while the backend hands over every device
    std::array<NativeInput*,2> remote_target_{};   // what each reader feeds now (null: nobody)
    bool two_player_mode_=false;   // a second player has joined in this run: the game sees a second pad, so a new device is asked about in the game too
    bool keyboard_used_=false;
    std::map<uint64_t,uint64_t> motion_;   // how much each pointer device has moved: who is the first player's mouse
    uint64_t swallow_until_=0;             // SDL's copies of the click that made a pointer the second player's are ignored until this time (ns)
};
}
