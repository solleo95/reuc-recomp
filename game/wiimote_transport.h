#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>
namespace uc {
// A connection to one Wii Remote: whole HID reports in and out (the report id first). The real one is wiimote_hid.cpp; tests bring their own.
struct WiiTransport {
    virtual ~WiiTransport()=default;
    // Waits up to timeout_ms for a report. True with an empty `report` on a timeout; false when the remote is gone.
    virtual bool read(std::vector<uint8_t>& report,unsigned timeout_ms)=0;
    virtual bool write(std::span<const uint8_t> report)=0;   // false when the remote refused or is gone
    virtual std::string id() const { return {}; }            // the same text for the same physical remote (the HID path, the Bluetooth address); empty when the transport has none
};
// The first Wii Remote HID device (VID 057E, PID 0306 or 0330) that opens, or null. `error` stays empty when there is none; it says why when one was found but did not open.
std::unique_ptr<WiiTransport> wii_open_first(std::string& error);
// Start an explicit pairing/discovery pass. Windows pairs a remote through the native Bluetooth API;
// Linux leaves Bluetooth pairing to the desktop and uses this request to trigger an immediate HID rescan.
std::string wii_sync_devices();
// The same, but a remote whose id is in `claimed` is passed over, and the id of the one that opens is added to it (the transport takes it out again when it is destroyed): two
// WiimoteInputs sharing one set open two different remotes. `claimed` must outlive the transports; any thread.
std::unique_ptr<WiiTransport> wii_open_unclaimed(std::set<std::string>& claimed,std::string& error);
}
