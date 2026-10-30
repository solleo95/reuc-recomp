// Frame pacing at 120 Hz and above needs 1 ms sleeps that last about 1 ms, and a monotonic game clock.
#include "check.h"
#include "clock.h"
#include <chrono>
#include <thread>

int main() {
    uint64_t last = uc::now_ticks();
    for (int i = 0; i < 1000; ++i) {
        const uint64_t now = uc::now_ticks();
        CHECK(now >= last);
        last = now;
    }
#ifdef __linux__   // Windows sleeps are made precise by timeBeginPeriod/waitable timers in game/uc_main.cpp
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    for (int i = 0; i < 200; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const double average_ms = std::chrono::duration<double, std::milli>(clock::now() - start).count() / 200;
    std::printf("average 1 ms sleep: %.3f ms\n", average_ms);
    CHECK(average_ms < 1.6);
#endif
    return 0;
}
