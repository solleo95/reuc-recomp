#pragma once
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <span>
namespace uc {
// Host keys of the port, never passed to the game: the menu key (the "menu" action of the controls, Insert by default) opens/closes the settings menu (Escape also
// closes it), P pauses/resumes the game.
struct HostKeys { bool menu_open=false, paused=false; };
enum class HostKeyResult { none, consumed, menu_toggled, pause_toggled };   // none: an ordinary key (the game or the menu gets it)
// Whether `key` opens the menu: one of the keys bound to the menu action; with none bound it is Insert (a menu nobody can open would lock the player out of the settings).
inline bool is_menu_key(SDL_Scancode key, std::span<const SDL_Scancode> menu_keys) {
    return menu_keys.empty() ? key == SDL_SCANCODE_INSERT : std::find(menu_keys.begin(), menu_keys.end(), key) != menu_keys.end();
}
// A key press (no repeats). While the menu captures a binding every key belongs to the capture.
inline HostKeyResult host_key(HostKeys& keys, SDL_Scancode key, bool capturing, std::span<const SDL_Scancode> menu_keys = {}) {
    if (capturing) return HostKeyResult::none;
    if (is_menu_key(key, menu_keys) || (key == SDL_SCANCODE_ESCAPE && keys.menu_open)) { keys.menu_open = !keys.menu_open; return HostKeyResult::menu_toggled; }
    if (key != SDL_SCANCODE_P) return HostKeyResult::none;
    if (keys.menu_open) return HostKeyResult::consumed;   // P does nothing while the menu is open
    keys.paused = !keys.paused; return HostKeyResult::pause_toggled;
}
}
