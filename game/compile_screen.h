#pragma once
#include <cstddef>
#include <string_view>

namespace uc {
// A small topmost window with a progress bar, shown while pipelines compile before the game starts.
// It stays hidden for the first 400 ms, so a start with a warm driver cache never flashes it.
class CompileScreen {
public:
    CompileScreen();
    ~CompileScreen();
    CompileScreen(const CompileScreen&)=delete;
    CompileScreen& operator=(const CompileScreen&)=delete;
    void show();   // at once, with a start message until the first update(); the renderer's start blocks the thread for seconds
    void wait_for_peer(std::string_view message); // UTF-8; stays responsive through the game's normal event pump.
    void update(size_t done,size_t total);   // also pumps this thread's window messages, so the game window stays responsive
    struct State;
private:
    State* state_=nullptr;
};
}
