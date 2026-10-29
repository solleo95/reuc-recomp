#pragma once
#include <cstdint>
#include <memory>
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
};
// The first Wii Remote HID device (VID 057E, PID 0306 or 0330) that opens, or null. `error` stays empty when there is none; it says why when one was found but did not open.
std::unique_ptr<WiiTransport> wii_open_first(std::string& error);
}
