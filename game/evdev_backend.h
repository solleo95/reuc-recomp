#pragma once
#include "pointer_devices.h"
#include <memory>
namespace uc {
// Linux only. Mice and light guns read as devices of their own from /dev/input/eventN, for local two-player play: a pointer device is opened read-only only when sysfs says it is a
// mouse or a gun (keyboards, touchpads, tablets and touchscreens never are), looked for again every two seconds, and grabbed (EVIOCGRAB) only on capture(). Nothing is written or logged.
std::unique_ptr<PointerBackend> make_evdev_pointer_backend();
}
