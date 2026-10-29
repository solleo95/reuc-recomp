#pragma once
#include "wiimote_config.h"
#include "wiimote_math.h"
#include "wiimote_target.h"
#include "wiimote_transport.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
namespace uc {
// The Wii Remote: a thread keeps one connection alive (finds the remote every two seconds while there is none, sets it up, reads its reports, notices when it goes), and
// update() hands what arrived to a WiimoteTarget from the main loop: button presses and releases in order, swings as motion, the IR pointer when the camera sees the
// sensor bar, the nunchuk stick. Nothing here can stop the game: a remote that cannot be opened or written to is reported in status() and left alone.
class WiimoteInput {
public:
    using Opener=std::function<std::unique_ptr<WiiTransport>(std::string& error)>;
    explicit WiimoteInput(Opener opener=wii_open_first);
    ~WiimoteInput();
    WiimoteInput(const WiimoteInput&)=delete;
    WiimoteInput& operator=(const WiimoteInput&)=delete;
    // Once per loop. active=false (a menu is open, the game is paused or unfocused), a lost remote or settings.enabled=false let go of everything it held in `target`;
    // buttons held meanwhile never turn into presses. `target` may be null (the launcher has none and only wants the capture sink).
    void update(WiimoteTarget* target,bool active,const WiimoteSettings&);
    // Every remote button press and release goes to the sink, active or not: the binding capture of the menus.
    void capture_sink(std::function<void(PhysicalInput,bool)>);
    WiimoteStatus status() const;
    unsigned changes() const { return changes_; }   // counts connections made and lost: status() is worth asking again
    unsigned activity() const { return activity_; } // counts button presses that update() handed on: the player is using the remote (main thread)
private:
    struct Event { bool button; uint16_t code; bool down; float x,y; uint64_t ns; };
    void run();
    bool handshake(WiiTransport&);
    void publish(WiiState,bool extension,WiiState& previous,WiiSwing&);
    void set_link(bool connected,const std::string& error);
    bool nap(unsigned ms);   // false when the thread must end
    Opener opener_;
    mutable std::mutex mutex_;
    std::deque<Event> events_;
    WiiState state_;
    uint64_t reports_=0;
    bool connected_=false;
    std::string error_;
    std::function<void(PhysicalInput,bool)> sink_;
    std::atomic<bool> stop_{false},enabled_{true};
    std::atomic<float> threshold_{1.5f};
    std::atomic<unsigned> changes_{0};
    bool engaged_=false;     // main thread: the target holds something of the remote
    uint64_t seen_reports_=0;
    unsigned activity_=0,seen_changes_=0;
    std::array<float,2> last_stick_{};
    std::thread thread_;
};
}
