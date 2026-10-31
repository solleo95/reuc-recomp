#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace uc::deck {
// The keys of the Steam Deck launcher, whatever produced them (the D-pad, the left stick, a keyboard, a tap).
enum class DeckKey { up, down, left, right, accept, back, add, clear, tab_prev, tab_next };
enum class RowKind { toggle, choice, slider, color, button, binding };
// What the focused row is told this frame: left/right as a change of value, A (accept), X (add) and Y (clear).
struct RowKeys { int delta = 0; bool accept = false, add = false, clear = false; bool any() const { return delta || accept || add || clear; } };
// The controls of one screen (or one modal) are rows, registered in the order they are drawn. The D-pad moves between rows and acts on the focused one at once: there
// are no blocks to enter first. Movement keys are applied when a frame starts (to the rows of the previous frame, which is what the user saw); the other keys are
// handed to the focused row while it is drawn.
class FocusList {
public:
    void begin_frame(std::span<const DeckKey> keys);
    // Registers the next row. Returns what the focused row is to do with the keys of this frame (nothing for other rows or while the focus is hidden).
    RowKeys add(RowKind kind, bool enabled, bool* focused = nullptr);
    void end_frame();                    // keeps the focus on an existing, enabled row
    int focus() const { return focus_; }
    void set_focus(int row) { focus_ = row; hidden_ = false; moved_ = true; }
    void follow(int row) { focus_ = row; hidden_ = false; }   // the focused row's content moved to another place: the focus goes with it, as if it had not moved (no scrolling, no reset of the row's parts)
    size_t count() const { return rows_.size(); }
    bool moved() const { return moved_; }   // the focus changed this frame: scroll it into view
    // Hidden: no row shows the focus or takes keys (the left side holds the selection). The first D-pad press shows it again and moves as usual.
    bool hidden() const { return hidden_; }
    void hide() { hidden_ = true; }
private:
    struct Row { RowKind kind; bool enabled; };
    std::vector<Row> rows_, drawn_;
    std::vector<DeckKey> pending_;
    int focus_ = 0;
    bool hidden_ = false, moved_ = false;
};
}
