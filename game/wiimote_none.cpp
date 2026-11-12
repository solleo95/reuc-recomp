#include "wiimote_transport.h"
// macOS: no Wii Remote transport yet (Windows has wiimote_hid.cpp, Linux wiimote_evdev.cpp). The openers report that no remote is there, which
// leaves the error empty as for a machine without one; gamepads, keyboard and mouse come through SDL3 as on the other platforms.
namespace uc {
std::unique_ptr<WiiTransport> wii_open_first(std::string&) { return nullptr; }
std::unique_ptr<WiiTransport> wii_open_unclaimed(std::set<std::string>&, std::string&) { return nullptr; }
}
