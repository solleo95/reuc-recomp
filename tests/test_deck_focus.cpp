// The Deck launcher's focus model: movement over rows, what the focused row receives (host test).
#include "check.h"
#include "deck/focus.h"
using namespace uc::deck;
namespace {
// One frame: begin, register the rows (kind, enabled), end; returns what the row at `watch` received.
struct Frame {
    FocusList& list;
    std::vector<std::pair<RowKind, bool>> rows;
    RowKeys run(std::vector<DeckKey> keys, int watch = -1) {
        list.begin_frame(keys);
        RowKeys got;
        for (size_t i = 0; i < rows.size(); ++i) {
            bool focused = false;
            auto keys_for_row = list.add(rows[i].first, rows[i].second, &focused);
            CHECK(focused == (int(i) == list.focus() && !list.hidden() && rows[i].second));
            if (int(i) == watch) got = keys_for_row;
            else CHECK(!keys_for_row.any() || int(i) == list.focus());
        }
        list.end_frame();
        return got;
    }
};
}
int main() {
    FocusList list;
    Frame f{list, {{RowKind::button, true}, {RowKind::toggle, true}, {RowKind::slider, true}, {RowKind::choice, true}}};
    f.run({});                                                 // the first frame: the first row is focused
    CHECK(list.focus() == 0 && list.count() == 4);
    f.run({DeckKey::down}); CHECK(list.focus() == 1 && list.moved());
    f.run({}); CHECK(!list.moved());
    f.run({DeckKey::down, DeckKey::down}); CHECK(list.focus() == 3);   // two presses in one frame
    f.run({DeckKey::down}); CHECK(list.focus() == 3);                  // no wrap
    f.run({DeckKey::up, DeckKey::up, DeckKey::up, DeckKey::up}); CHECK(list.focus() == 0);
    // the focused row gets the keys at once (no block to enter first): a slider changes with left/right, a toggle with A
    f.run({DeckKey::down, DeckKey::down});
    auto slider = f.run({DeckKey::right, DeckKey::right, DeckKey::left}, 2);
    CHECK(slider.delta == 1 && !slider.accept);                // +1 +1 -1
    f.run({DeckKey::up});
    auto toggle = f.run({DeckKey::accept}, 1);
    CHECK(toggle.accept && toggle.delta == 0);
    auto binding = f.run({DeckKey::add, DeckKey::clear}, 1);
    CHECK(binding.add && binding.clear && !binding.accept);
    CHECK(!f.run({DeckKey::accept}, 2).any());                 // another row gets nothing
    // keys that are not for rows are not handed out
    CHECK(!f.run({DeckKey::back, DeckKey::tab_next}, 1).any());
    // disabled rows are skipped by the movement and never focused
    FocusList skipping;
    Frame g{skipping, {{RowKind::button, true}, {RowKind::toggle, false}, {RowKind::toggle, false}, {RowKind::slider, true}}};
    g.run({}); g.run({DeckKey::down}); CHECK(skipping.focus() == 3);
    g.run({DeckKey::up}); CHECK(skipping.focus() == 0);
    // a row that disappears (a slider hidden by another setting): the focus stays on a row that exists
    FocusList shrinking;
    Frame h{shrinking, {{RowKind::button, true}, {RowKind::button, true}, {RowKind::button, true}}};
    h.run({}); h.run({DeckKey::down, DeckKey::down}); CHECK(shrinking.focus() == 2);
    h.rows.pop_back(); h.run({}); CHECK(shrinking.focus() == 1);
    h.rows.clear(); h.run({}); CHECK(shrinking.focus() == 0);   // an empty list is fine
    // hidden: the selection is on the left; rows show no focus and take no keys; the first D-pad press brings the focus back
    FocusList hidden;
    Frame k{hidden, {{RowKind::toggle, true}, {RowKind::toggle, true}}};
    k.run({});   // movement applies to the rows of the previous frame: the first frame has none
    k.run({DeckKey::down}); CHECK(hidden.focus() == 1);
    hidden.hide();
    CHECK(!k.run({DeckKey::accept}, 1).any() && hidden.hidden());      // A is not for the rows now
    k.run({DeckKey::up}); CHECK(!hidden.hidden() && hidden.focus() == 0);   // shown again, and the press moved it
    hidden.hide(); k.run({DeckKey::left}); CHECK(!hidden.hidden());         // any D-pad press shows it
    std::puts("deck focus: passed");
}
