#pragma once
#include <SDL3/SDL_scancode.h>
namespace uc {
// Host keys of the port, never passed to the game: Insert opens/closes the settings menu (Escape also closes it), P pauses/resumes the game.
struct HostKeys { bool menu_open=false, paused=false; };
enum class HostKeyResult { none, consumed, menu_toggled, pause_toggled };   // none: an ordinary key (the game or the menu gets it)
// A key press (no repeats). While the menu captures a binding every key belongs to the capture.
inline HostKeyResult host_key(HostKeys& keys, SDL_Scancode key, bool capturing) {
    if (capturing) return HostKeyResult::none;
    if (key == SDL_SCANCODE_INSERT || (key == SDL_SCANCODE_ESCAPE && keys.menu_open)) { keys.menu_open = !keys.menu_open; return HostKeyResult::menu_toggled; }
    if (key != SDL_SCANCODE_P) return HostKeyResult::none;
    if (keys.menu_open) return HostKeyResult::consumed;   // P does nothing while the menu is open
    keys.paused = !keys.paused; return HostKeyResult::pause_toggled;
}
}
