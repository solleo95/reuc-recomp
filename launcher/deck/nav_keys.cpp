#include "nav_keys.h"
#include <SDL3/SDL.h>
namespace uc::deck {
namespace {
constexpr uint64_t kRepeatFirstNs = 380000000ull, kRepeatNs = 70000000ull;
bool direction(DeckKey key) { return key == DeckKey::up || key == DeckKey::down || key == DeckKey::left || key == DeckKey::right; }
}
void KeyQueue::hold(DeckKey key, bool down, uint64_t now_ns) {
    if (down) { queue_.push_back(key); holding_ = true; held_ = key; next_ns_ = now_ns + kRepeatFirstNs; }
    else if (holding_ && held_ == key) holding_ = false;
}
void KeyQueue::press(DeckKey key) { queue_.push_back(key); }
void KeyQueue::clear() { queue_.clear(); holding_ = false; stick_[0] = stick_[1] = 0; }
std::vector<DeckKey> KeyQueue::take(uint64_t now_ns) {
    std::vector<DeckKey> keys;
    keys.swap(queue_);
    if (holding_ && now_ns >= next_ns_) { keys.push_back(held_); next_ns_ = now_ns + kRepeatNs; }
    return keys;
}
bool KeyQueue::event(const SDL_Event& e, uint64_t now_ns) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: hold(DeckKey::up, down, now_ns); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: hold(DeckKey::down, down, now_ns); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: hold(DeckKey::left, down, now_ns); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: hold(DeckKey::right, down, now_ns); return true;
        case SDL_GAMEPAD_BUTTON_SOUTH: if (down) press(DeckKey::accept); return true;
        case SDL_GAMEPAD_BUTTON_EAST: if (down) press(DeckKey::back); return true;
        case SDL_GAMEPAD_BUTTON_WEST: if (down) press(DeckKey::add); return true;
        case SDL_GAMEPAD_BUTTON_NORTH: if (down) press(DeckKey::clear); return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: if (down) press(DeckKey::tab_prev); return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: if (down) press(DeckKey::tab_next); return true;
        default: return false;
        }
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int axis_index;
        DeckKey negative, positive;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) { axis_index = 0; negative = DeckKey::left; positive = DeckKey::right; }
        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) { axis_index = 1; negative = DeckKey::up; positive = DeckKey::down; }
        else return false;
        const float value = float(e.gaxis.value) / 32767.f;   // the left stick is a D-pad with a dead zone and some hysteresis
        const int old = stick_[axis_index];
        const int now = value > (old > 0 ? .35f : .6f) ? 1 : value < -(old < 0 ? .35f : .6f) ? -1 : 0;
        if (now == old) return true;
        if (old) hold(old < 0 ? negative : positive, false, now_ns);
        if (now) hold(now < 0 ? negative : positive, true, now_ns);
        stick_[axis_index] = now;
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        DeckKey key;
        switch (e.key.scancode) {
        case SDL_SCANCODE_UP: key = DeckKey::up; break;
        case SDL_SCANCODE_DOWN: key = DeckKey::down; break;
        case SDL_SCANCODE_LEFT: key = DeckKey::left; break;
        case SDL_SCANCODE_RIGHT: key = DeckKey::right; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: key = DeckKey::accept; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: key = DeckKey::back; break;
        case SDL_SCANCODE_X: key = DeckKey::add; break;
        case SDL_SCANCODE_DELETE: key = DeckKey::clear; break;
        case SDL_SCANCODE_PAGEUP: key = DeckKey::tab_prev; break;
        case SDL_SCANCODE_PAGEDOWN: key = DeckKey::tab_next; break;
        default: return false;
        }
        if (e.key.repeat) return true;   // the repeat is ours, not the system's
        if (direction(key)) hold(key, down, now_ns);
        else if (down) press(key);
        return true;
    }
    default:
        return false;
    }
}
}
