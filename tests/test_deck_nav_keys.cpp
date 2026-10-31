// The Deck launcher's key queue: SDL events in, DeckKeys out, with the repeat of a held direction (host test).
#include "check.h"
#include "deck/nav_keys.h"
#include <SDL3/SDL.h>
using namespace uc::deck;
namespace {
constexpr uint64_t ms = 1000000ull;
SDL_Event button(SDL_GamepadButton which, bool down) {
    SDL_Event e{}; e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP; e.gbutton.button = Uint8(which); e.gbutton.down = down; return e;
}
SDL_Event axis(SDL_GamepadAxis which, int value) { SDL_Event e{}; e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION; e.gaxis.axis = Uint8(which); e.gaxis.value = Sint16(value); return e; }
SDL_Event key(SDL_Scancode which, bool down, bool repeat = false) {
    SDL_Event e{}; e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP; e.key.scancode = which; e.key.down = down; e.key.repeat = repeat; return e;
}
using Keys = std::vector<DeckKey>;
}
int main() {
    KeyQueue q;
    // the face buttons and shoulders are single presses
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_SOUTH, true), 0) && q.take(0) == Keys{DeckKey::accept});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_SOUTH, false), 0) && q.take(1) == Keys{});         // releasing is nothing
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_EAST, true), 0) && q.take(0) == Keys{DeckKey::back});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_WEST, true), 0) && q.take(0) == Keys{DeckKey::add});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_NORTH, true), 0) && q.take(0) == Keys{DeckKey::clear});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true), 0) && q.take(0) == Keys{DeckKey::tab_prev});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true), 0) && q.take(0) == Keys{DeckKey::tab_next});
    // a held D-pad direction repeats: once at once, then after 380 ms, then every 70 ms; releasing stops it
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true), 0));
    CHECK(q.take(0) == Keys{DeckKey::down});
    CHECK(q.take(300 * ms) == Keys{});
    CHECK(q.take(380 * ms) == Keys{DeckKey::down});
    CHECK(q.take(420 * ms) == Keys{});
    CHECK(q.take(450 * ms) == Keys{DeckKey::down});
    CHECK(q.take(520 * ms) == Keys{DeckKey::down});
    CHECK(q.event(button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false), 530 * ms));
    CHECK(q.take(2000 * ms) == Keys{});
    // another direction replaces the held one
    q.event(button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true), 0); q.take(0);
    q.event(button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true), 10 * ms);
    CHECK((q.take(10 * ms) == Keys{DeckKey::right}));
    CHECK(q.take(400 * ms) == Keys{DeckKey::right});                                          // the right one repeats from its own press
    q.event(button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false), 401 * ms); q.event(button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, false), 401 * ms); q.take(401 * ms);
    // the left stick acts as a D-pad with a dead zone and some hysteresis
    CHECK(q.event(axis(SDL_GAMEPAD_AXIS_LEFTX, 10000), 0) && q.take(0) == Keys{});            // 0.3: nothing
    q.event(axis(SDL_GAMEPAD_AXIS_LEFTX, 25000), 0);
    CHECK(q.take(0) == Keys{DeckKey::right});                                                  // 0.76: right
    q.event(axis(SDL_GAMEPAD_AXIS_LEFTX, 15000), 0);                                           // 0.46: still held (hysteresis)
    CHECK(q.take(500 * ms) == Keys{DeckKey::right});                                           // ...and repeating
    q.event(axis(SDL_GAMEPAD_AXIS_LEFTX, 0), 510 * ms);
    CHECK(q.take(5000 * ms) == Keys{});
    q.event(axis(SDL_GAMEPAD_AXIS_LEFTY, -30000), 6000 * ms);
    CHECK(q.take(6000 * ms) == Keys{DeckKey::up});
    q.event(axis(SDL_GAMEPAD_AXIS_LEFTY, 0), 6001 * ms); q.take(6001 * ms);
    CHECK(!q.event(axis(SDL_GAMEPAD_AXIS_RIGHTX, 30000), 0));                                  // the right stick is not ours
    // a keyboard stands in for the pad (a desktop test run, a dock); auto-repeat comes from our own repeat, not the system's
    CHECK(q.event(key(SDL_SCANCODE_DOWN, true), 0) && q.take(0) == Keys{DeckKey::down});
    CHECK(q.event(key(SDL_SCANCODE_DOWN, true, true), 100 * ms) && q.take(100 * ms) == Keys{});
    q.event(key(SDL_SCANCODE_DOWN, false), 120 * ms); q.take(120 * ms);
    CHECK(q.event(key(SDL_SCANCODE_RETURN, true), 0) && q.take(0) == Keys{DeckKey::accept});
    CHECK(q.event(key(SDL_SCANCODE_SPACE, true), 0) && q.take(0) == Keys{DeckKey::accept});
    CHECK(q.event(key(SDL_SCANCODE_ESCAPE, true), 0) && q.take(0) == Keys{DeckKey::back});
    CHECK(q.event(key(SDL_SCANCODE_PAGEUP, true), 0) && q.take(0) == Keys{DeckKey::tab_prev});
    CHECK(q.event(key(SDL_SCANCODE_PAGEDOWN, true), 0) && q.take(0) == Keys{DeckKey::tab_next});
    CHECK(!q.event(key(SDL_SCANCODE_A, true), 0));
    // press() feeds a tap or a script as the same keys; clear() forgets everything, held directions included
    q.press(DeckKey::accept); q.press(DeckKey::down);
    CHECK((q.take(0) == Keys{DeckKey::accept, DeckKey::down}));
    q.event(button(SDL_GAMEPAD_BUTTON_DPAD_UP, true), 0); q.clear();
    CHECK(q.take(1000 * ms) == Keys{});
    SDL_Event unrelated{}; unrelated.type = SDL_EVENT_WINDOW_RESIZED;
    CHECK(!q.event(unrelated, 0));
    std::puts("deck nav keys: passed");
}
