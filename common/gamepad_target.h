#pragma once
#include "input_bindings.h"
#include <string>
namespace uc {
struct PadStatus { std::string name; bool connected=false,has_gyro=false; PadFamily family=PadFamily::xbox; };   // the gamepad used last
enum class PadSide:uint8_t { left,right };
constexpr unsigned kPadSlots=8;   // gamepads that can push at once; each keeps its own sticks
// What a gamepad reader hands its events to: the game's NativeInput. The launcher reads gamepads too (to show the connected one and capture bindings) and has no target.
class GamepadTarget {
public:
    virtual ~GamepadTarget()=default;
    virtual void host_button(PhysicalInput,bool)=0;
    // A stick of gamepad `pad` (0..kPadSlots-1), -1..1 per axis, y down positive.
    virtual void host_pad_stick(unsigned pad,PadSide,float x,float y)=0;
    virtual void host_pad_gyro(float yaw_rate,float pitch_rate,uint64_t timestamp_ns)=0;   // rad/s, offset removed
    virtual void pad_reset()=0;   // every stick back to rest, every held gamepad input released
};
}
