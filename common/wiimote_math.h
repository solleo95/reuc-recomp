#pragma once
#include "wiimote_protocol.h"
#include <array>
#include <cstdint>
#include <optional>
namespace uc {
// How far the pointer travels for a turn of the remote: the IR camera sees the sensor bar move by about this fraction of a screen for each screen's width of aim.
constexpr float kWiiPointerRange=1.8f;
// The pointer the IR camera gives: the middle of the two dots furthest apart (the ends of the sensor bar), turned back by the roll of the remote and mirrored, as -1..1
// of the window (y down), clamped. Nothing while fewer than two dots, or two closer than 6 camera pixels, are seen.
std::optional<std::array<float,2>> wii_ir_pointer(const std::array<WiiIrDot,4>& dots);
// The nunchuk's stick as -1..1 (x right, y down like SDL's sticks): centre 128, +-95 to full, a radial dead zone (0..1 of the full deflection) rescaled past it.
// shortcut: no calibration from the nunchuk's EEPROM; read it when sticks are seen to be off-centre.
std::array<float,2> wii_stick(std::array<uint8_t,2> raw,float dead_zone);
// Swings and shakes from the accelerometer. Gravity is tracked slowly, what remains is the hand's push; a push in the screen plane (x right, z up) stronger than
// `threshold_g` comes back as mouse counts (x right, y down) pointing the way it was pushed, the size of the push: the same input the knife and shake detectors read
// from a mouse. Axis signs follow +x right, +y forward, +z up; shortcut: not confirmed on a real remote, flip them in wiimote_math.cpp if a swing goes the wrong way.
class WiiSwing {
public:
    std::array<float,2> feed(std::array<uint8_t,3> accel,uint64_t timestamp_ns,float threshold_g);
private:
    bool have_=false;
    std::array<float,3> gravity_{};
    uint64_t last_ns_=0;
};
}
