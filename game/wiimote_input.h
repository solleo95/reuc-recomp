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
    using Syncer=std::function<std::string()>;
    explicit WiimoteInput(Opener opener=wii_open_first,Syncer syncer=wii_sync_devices);
    ~WiimoteInput();
    WiimoteInput(const WiimoteInput&)=delete;
    WiimoteInput& operator=(const WiimoteInput&)=delete;
    // Once per loop. active=false (a menu is open, the game is paused or unfocused), a lost remote or settings.enabled=false let go of everything it held in `target`;
    // buttons held meanwhile never turn into presses. `target` may be null (the launcher has none and only wants the capture sink).
    void update(WiimoteTarget* target,bool active,const WiimoteSettings&);
    // Every remote button press and release goes to the sink, active or not: the binding capture of the menus.
    void capture_sink(std::function<void(PhysicalInput,bool)>);
    WiimoteStatus status() const;
    void request_sync();  // pair/discover now instead of waiting for the next background HID search
    // Local two-player play. The host loop asks which remote pressed something before it decides whose target the remote feeds (the join press itself reaches that target).
    bool pending_press() const;               // a button press is queued that update() has not handed on yet (any thread)
    // Once per loop before pending_press(): takes what has arrived so far. The next update() hands on only that, so a press the reader thread publishes after the host looked is
    // seen by pending_press() before any target gets it. Without a collect() update() takes everything. Main thread.
    void collect();
    std::string device_id() const;            // the connected remote's WiiTransport::id(); empty without a connection
    void set_player(unsigned player);         // 0 or 1: the remote lights that player's LED (the set-up lights LED 1); any thread
    void release(WiimoteTarget&);             // lets go of what `target` holds of this remote (called before the remote's target changes); main thread
    // The remote's motor (it has no strength). Any thread; kept for a remote that connects later, and in every report the thread sends (the motor is bit 0 of each).
    void rumble(bool on) { rumble_=on; }
    unsigned changes() const { return changes_; }   // counts connections made and lost: status() is worth asking again
    unsigned activity() const { return activity_; } // counts button presses that update() handed on: the player is using the remote (main thread)
private:
    struct Event { bool button; uint16_t code; bool down; float x,y; uint64_t ns; };
    void run();
    bool handshake(WiiTransport&);
    void publish(WiiState,bool extension,WiiState& previous,WiiSwing&);
    void set_link(bool connected,const std::string& error,const std::string& id={});
    bool nap(unsigned ms);   // false when the thread must end
    bool send(WiiTransport&,std::span<const uint8_t> report);   // write() with the motor bit of the report set while the motor is wanted
    Opener opener_;
    Syncer syncer_;
    mutable std::mutex mutex_;
    std::deque<Event> events_,held_;   // held_: collect()ed, waiting for update()
    bool collected_=false;
    WiiState state_;
    uint64_t reports_=0;
    bool connected_=false;
    std::string error_,id_;
    std::function<void(PhysicalInput,bool)> sink_;
    std::atomic<unsigned> player_{0};
    std::atomic<bool> stop_{false},enabled_{true},rumble_{false};
    std::atomic<bool> sync_requested_{false},syncing_{false},sync_finished_{false};
    std::thread sync_thread_;  // Bluetooth discovery must not stall a currently connected remote's HID reader
    std::string sync_message_;
    std::atomic<float> threshold_{1.5f};
    std::atomic<unsigned> changes_{0};
    bool engaged_=false;     // main thread: the target holds something of the remote
    uint64_t seen_reports_=0;
    unsigned activity_=0,seen_changes_=0;
    std::array<float,2> last_stick_{};
    std::thread thread_;
};
}
