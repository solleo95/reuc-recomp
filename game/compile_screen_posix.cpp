#include "compile_screen.h"
#include <cstdio>
#include <string>

// Linux: no separate window (Windows draws a topmost progress window, compile_screen_win.cpp). The progress goes to the log in steps of 10 %.
// shortcut: no on-screen feedback while the pipelines compile; draw it in the game window itself if the wait is long on the Deck.
namespace uc {
struct CompileScreen::State {
    size_t last_step = 0;
};
CompileScreen::CompileScreen(std::string_view) : state_(new State) {}
CompileScreen::~CompileScreen() { delete state_; }
void CompileScreen::show() { std::fprintf(stderr, "[compile] starting\n"); }
void CompileScreen::wait_for_peer(std::string_view message) { std::fprintf(stderr, "[compile] waiting: %s\n", std::string(message).c_str()); }
void CompileScreen::update(size_t done, size_t total) {
    if (!total) return;
    const size_t step = done * 10 / total;
    if (step == state_->last_step && done != total) return;
    state_->last_step = step;
    std::fprintf(stderr, "[compile] %zu / %zu\n", done, total);
}
}  // namespace uc
