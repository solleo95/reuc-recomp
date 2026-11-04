#pragma once
#include "evdev_logic.h"
#include <string>
#include <vector>
namespace uc {
// Linux only. The input nodes /dev/input/eventN as /sys/class/input says they are (name, unique id, capabilities, properties: readable by everyone), classified without
// opening anything: a keyboard's node is never opened.
struct EvdevNode { int index=0; std::string path; EvdevCaps caps; EvdevRole role=EvdevRole::ignore; };
std::vector<EvdevNode> scan_evdev_nodes();   // ordered by index
// A mouse, or a gun still without its axis range: worth opening to read the range (EVIOCGABS) and then classify again.
bool evdev_pointer_candidate(const EvdevCaps&);
}
