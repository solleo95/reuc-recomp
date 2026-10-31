#include "focus.h"
#include <algorithm>
namespace uc::deck {
void FocusList::begin_frame(std::span<const DeckKey> keys) {
    drawn_ = std::move(rows_);
    rows_.clear();
    pending_.clear();
    moved_ = false;
    const int count = int(drawn_.size());
    for (const DeckKey key : keys) {
        switch (key) {
        case DeckKey::up: case DeckKey::down: case DeckKey::left: case DeckKey::right:
            hidden_ = false;   // any D-pad press brings the focus back from the left side
            break;
        default: break;
        }
        switch (key) {
        case DeckKey::up:
            for (int i = focus_ - 1; i >= 0 && i < count; --i) if (drawn_[size_t(i)].enabled) { focus_ = i; moved_ = true; break; }
            break;
        case DeckKey::down:
            for (int i = focus_ + 1; i < count; ++i) if (drawn_[size_t(i)].enabled) { focus_ = i; moved_ = true; break; }
            break;
        case DeckKey::left: case DeckKey::right: case DeckKey::accept: case DeckKey::add: case DeckKey::clear:
            pending_.push_back(key);
            break;
        case DeckKey::back: case DeckKey::tab_prev: case DeckKey::tab_next:
            break;
        }
    }
}
RowKeys FocusList::add(RowKind kind, bool enabled, bool* focused) {
    const int index = int(rows_.size());
    rows_.push_back({kind, enabled});
    const bool mine = index == focus_ && !hidden_ && enabled;
    if (focused) *focused = mine;
    RowKeys result;
    if (!mine || pending_.empty()) return result;
    for (const DeckKey key : pending_) {
        if (key == DeckKey::left) --result.delta;
        else if (key == DeckKey::right) ++result.delta;
        else if (key == DeckKey::accept) result.accept = true;
        else if (key == DeckKey::add) result.add = true;
        else if (key == DeckKey::clear) result.clear = true;
    }
    result.delta = std::clamp(result.delta, -1, 1);   // one step per frame, however many presses piled up
    pending_.clear();
    return result;
}
void FocusList::end_frame() {
    const int count = int(rows_.size());
    if (!count) { focus_ = 0; return; }
    focus_ = std::clamp(focus_, 0, count - 1);
    if (rows_[size_t(focus_)].enabled) return;
    for (int step = 1; step < count; ++step)   // the nearest enabled row, below first
        for (const int candidate : {focus_ + step, focus_ - step})
            if (candidate >= 0 && candidate < count && rows_[size_t(candidate)].enabled) { focus_ = candidate; return; }
}
}
