#pragma once
#include "host_input.h"
#include "pointer_devices.h"
namespace uc {
struct PointerWindow { SDL_WindowID id=0; uint32_t width=0,height=0; float density=1; };   // the window in pixels and SDL's pixels per point
// A PointerEvent as the SDL mouse event HostInput reads: absolute positions as window points (pixels divided by density, so HostInput's multiplication gives the exact pixel).
// An event that cannot be a position (NaN or infinite, an absolute one outside 0..1) comes back as SDL_EVENT_USER and is not to be fed.
SDL_Event pointer_event_to_sdl(const PointerEvent&,const PointerWindow&);
// Hands the event to `host` as it would get a mouse event: its pointer mode follows the event (an absolute event is a position, a relative one a move), its extent and density
// follow the window. Returns what HostInput::event returns (true: a click that only recaptures the pointer).
bool feed_pointer_event(HostInput& host,NativeInput& input,const PointerEvent&,const PointerWindow&,bool captured);
}
