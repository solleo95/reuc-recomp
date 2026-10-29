#pragma once
#include "input_bindings.h"
#include <string>
namespace uc {
struct WiimoteStatus { bool connected=false,has_nunchuk=false; std::string error; };   // error: why a remote that was found cannot be used (empty: none found, or all well)
// What the Wii Remote reader hands its events to: the game's NativeInput (the launcher reads the remote too, to show it and to capture bindings, and has no target).
// Implemented next to GamepadTarget by the same class; host_button is the one function both declare.
class WiimoteTarget {
public:
    virtual ~WiimoteTarget()=default;
    virtual void host_button(PhysicalInput,bool)=0;
    virtual void host_wii_pointer(float x,float y,uint64_t timestamp_ns)=0;   // the IR pointer, -1..1 of the window (y down): throws std::invalid_argument for NaN
    virtual void host_wii_stick(float x,float y)=0;                           // the nunchuk's stick after its dead zone, -1..1, y down positive like a gamepad's
    virtual void host_wii_motion(float dx,float dy,uint64_t timestamp_ns)=0;  // a swing or shake as mouse counts: feeds the knife and shake detectors, moves nothing
    virtual void wii_reset()=0;                                               // the stick at rest, every held Wii Remote button released
};
}
