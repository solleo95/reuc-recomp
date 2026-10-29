#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace uc {
// The Wii Remote's Bluetooth HID protocol (Wiibrew "Wiimote"): what the remote sends and the reports that set it up. Pure bytes, no I/O. A report starts with its id.
enum class WiiButton:uint8_t { left,right,down,up,plus,two,one,b,a,minus,home,nunchuk_z,nunchuk_c,count };
struct WiiIrDot { uint16_t x=0,y=0; bool valid=false; };   // the camera sees 1024x768; an unseen dot is invalid
struct WiiState {
    uint32_t buttons=0;                              // bit per WiiButton
    std::array<uint8_t,3> accel{0x80,0x80,0x80};     // 8 bits per axis, rest near 0x80 (z about 0x9A: gravity)
    std::array<WiiIrDot,4> ir{};
    bool has_nunchuk=false;
    std::array<uint8_t,2> stick{128,128};            // the nunchuk's stick, 0..255
    std::array<uint8_t,3> nunchuk_accel{0x80,0x80,0x80};
};
struct WiiStatus { bool extension=false; uint8_t battery=0; };
struct WiiStep { std::vector<uint8_t> report; unsigned delay_ms=0; };   // an output report and the pause to keep after it
constexpr uint16_t kWiimoteVendor=0x057E,kWiimoteProduct=0x0306,kWiimotePlusProduct=0x0330;   // RVL-CNT-01 and RVL-CNT-01-TR; a Dolphin Bar in mode 4 shows the same
constexpr size_t kWiiInputBytes=22;   // every input report, padded by the HID layer
// Data reports 0x30 (buttons), 0x31 (+accelerometer), 0x33 (+extended IR), 0x35 (+extension), 0x37 (+basic IR and extension). True and `state` replaced when the report is
// one of them and long enough; anything else (status, acknowledge, short, unknown) is refused and leaves `state` alone. Never throws.
bool wii_parse_report(std::span<const uint8_t> report,WiiState& state);
std::optional<WiiStatus> wii_parse_status(std::span<const uint8_t> report);   // report 0x20
std::array<uint8_t,2> wii_led_report(unsigned player);   // 0 none, 1..4 the LED of that player; throws std::invalid_argument above 4
std::array<uint8_t,3> wii_reporting_report(uint8_t mode);   // report 0x12: send `mode` continuously
std::vector<uint8_t> wii_write_register(uint32_t address,std::span<const uint8_t> data);   // report 0x16 into the register space; address 24 bits, up to 16 bytes, else std::invalid_argument
std::vector<WiiStep> wii_extension_steps();   // the first two steps of the init sequence: make the plugged-in extension plain
std::array<uint8_t,2> wii_status_request();   // report 0x15: the remote answers with a status (report 0x20)
// Everything to send after connecting: make the extension plain, switch the IR camera on and tune it, choose report 0x37, light LED 1.
std::vector<WiiStep> wii_init_sequence();
}
