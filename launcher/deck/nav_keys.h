#pragma once
#include "focus.h"
#include <cstdint>
#include <vector>
union SDL_Event;
namespace uc::deck {
// SDL events in, DeckKeys out. The D-pad, the left stick (with a dead zone and hysteresis) and the arrow keys repeat while held: at once, after 380 ms, then every 70 ms.
class KeyQueue {
public:
    bool event(const SDL_Event&, uint64_t now_ns);   // true when the event is one of ours (the caller keeps it from other consumers)
    void press(DeckKey);                              // a tap or a scripted key
    std::vector<DeckKey> take(uint64_t now_ns);      // the keys since the last call, plus the repeat of the held direction
    void clear();
private:
    void hold(DeckKey, bool down, uint64_t now_ns);
    std::vector<DeckKey> queue_;
    bool holding_ = false;
    DeckKey held_ = DeckKey::up;
    uint64_t next_ns_ = 0;
    int stick_[2] = {0, 0};
};
}
