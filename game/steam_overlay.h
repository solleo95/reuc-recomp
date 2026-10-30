#pragma once
#include <chrono>
namespace uc {
// Whether Steam's own interface (the Steam button's menu, the "..." quick access menu, Steam's dialogs) is on top of the game on a Steam Deck in Game Mode. The game reads the
// controller itself, so Steam does not take it away and the window keeps its focus: the signs are gamescope's (read from its X server, :0, through libX11 loaded at run time):
// the focused app becomes Steam's own (appid 769) while a menu is up, and the background is blurred while one fades in or out (the "..." menu does not change the focused app
// everywhere). The Steam and "..." buttons themselves reach the game a quarter of a second before the menu shows: hint() covers that gap. Always false off a Deck (no Steam
// game id, no gamescope) and on Windows.
class SteamOverlay {
public:
    bool open();   // polled at most every 50 ms
    void hint();   // the Steam or "..." button was pressed: a menu is about to open (or close): counts as open for a moment
private:
    bool query();
    bool open_=false, failed_=false;
    std::chrono::steady_clock::time_point last_{}, hint_until_{};
    void* display_=nullptr;
};
}
