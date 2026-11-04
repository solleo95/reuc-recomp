#pragma once
#include "pointer_devices.h"
#include "wiimote_protocol.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace uc {
// Linux input (evdev) as pure logic: what a device is, and what its events mean. No Linux headers and no I/O, so it builds and is tested everywhere; the Linux layer
// (game/evdev_backend.cpp) only opens, reads and grabs. The values are the kernel's (linux/input-event-codes.h).
namespace evdev {
constexpr uint16_t kSyn=0,kKey=1,kRel=2,kAbs=3;             // event types
constexpr uint16_t kSynReport=0;
constexpr uint16_t kRelX=0,kRelY=1,kRelHWheel=6,kRelWheel=8,kAbsX=0,kAbsY=1;
constexpr uint16_t kBtnLeft=0x110,kBtnRight=0x111,kBtnMiddle=0x112,kBtnSide=0x113,kBtnExtra=0x114;
constexpr uint16_t kAbsRx=3,kAbsRy=4,kAbsRz=5,kAbsHat0X=0x10;   // ABS_HAT0X..ABS_HAT3Y are 0x10..0x17: x of dot n is 0x10+2n, y 0x11+2n
}
struct EvdevEvent { uint16_t type=0,code=0; int32_t value=0; };
struct EvdevCaps {
    std::string name,unique;                // EVIOCGNAME, EVIOCGUNIQ (the Bluetooth address of a Wii Remote: all its nodes share it)
    bool rel_x=false,rel_y=false,abs_x=false,abs_y=false,btn_left=false;
    bool tool_finger=false,tool_pen=false,direct=false;   // touchpads, tablets (BTN_TOOL_FINGER / BTN_TOOL_PEN) and touchscreens (INPUT_PROP_DIRECT) are not pointers of a player
    bool keyboard_keys=false;               // has letter keys
    std::array<int32_t,2> abs_min{0,0},abs_max{0,0},abs_value{0,0};   // ABS_X and ABS_Y: range and value now
};
// The text of /sys/class/input/eventN/device/capabilities/* (readable by everyone, unlike the device node): hex words separated by spaces, the highest word first, `word_bits` bits
// each (the machine's long). True when `bit` is set; anything that is not a bitmap, or a bit beyond it, is false.
bool evdev_bitmap_has(std::string_view words,unsigned bit,unsigned word_bits=64);
// A device's EvdevCaps from that text (key, rel and abs bitmaps and the properties file), without opening it: keyboards never need to be opened. The axis ranges and values
// (EVIOCGABS) are not in sysfs and stay zero until the caller reads them after opening, so a gun is not a gun before that.
EvdevCaps evdev_caps_from_sysfs(std::string name,std::string unique,std::string_view key,std::string_view rel,std::string_view abs,std::string_view properties,unsigned word_bits=64);
// What the nodes of one Wii Remote have in common. hid-wiimote's accelerometer, IR and nunchuk nodes have neither uniq nor phys: their parent is the HID device, so the HID device's
// sysfs path (`hid_parent`, the resolved /sys/class/input/eventN/device/../..) groups them with the button node. A virtual device (uinput; its parent is under /sys/devices/virtual)
// has no HID parent of its own: its phys string groups its nodes. Empty: nothing to group by.
std::string evdev_group_key(std::string_view hid_parent,std::string_view phys);
// hid-wiimote names its player LEDs "<hid device>:blue:p0" .. "p3": the player index 0..3, or -1 for any other LED.
int evdev_led_player(std::string_view led_name);
enum class EvdevRole:uint8_t { ignore,mouse,gun,wiimote_buttons,wiimote_accel,wiimote_ir,wiimote_nunchuk };
// Mouse: relative X and Y and a left button. Gun: absolute X and Y with a usable range and a left button. A Wii Remote is four nodes of the kernel's hid-wiimote driver named
// "Nintendo Wii Remote" plus a suffix; the other nodes of a remote (Motion Plus, Pro Controller, Balance Board, ...) are ignored.
EvdevRole classify_evdev(const EvdevCaps&);
// A mouse's or gun's events as PointerEvents. Per EV_SYN frame: the motion first (relative counts summed, or the new absolute position normalised to 0..1 over the axis range
// and clamped), then the buttons in the order they came, so a shot lands at the position it was fired at. Key repeats are ignored.
struct PointerState {
    float dx=0,dy=0,wheel=0;
    std::array<int32_t,2> abs{0,0}; bool abs_dirty=false;
    std::vector<std::pair<uint8_t,bool>> buttons;   // SDL button numbers and states of the frame so far
    bool started=false;                              // abs holds the device's own value (taken from the caps on the first call)
};
void feed_pointer(PointerState&,const EvdevCaps&,std::span<const EvdevEvent>,uint64_t device,std::vector<PointerEvent>& out);
// The nodes of one Wii Remote folded into the WiiState the shared protocol layer reads: buttons and the nunchuk's Z and C, the accelerometer as the remote's 8-bit bytes, the IR
// dots (an unseen dot is the kernel's 1023,1023), the nunchuk stick as 0..255 around 128.
struct WiiEvdevState {
    WiiState state;
    std::array<bool,4> seen_x{},seen_y{};
};
void feed_wiimote(WiiEvdevState&,EvdevRole,std::span<const EvdevEvent>);
}
