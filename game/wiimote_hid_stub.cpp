#include "wiimote_transport.h"

// Linux: no Wii Remote support yet (the Windows build reads it over HID in wiimote_hid.cpp). A native one would use hidraw or the kernel's hid-wiimote driver.
namespace uc {
std::unique_ptr<WiiTransport> wii_open_first(std::string& error) {
    error.clear();   // "none found", not a failure
    return nullptr;
}
}
