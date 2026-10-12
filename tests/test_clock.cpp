#include "check.h"
#include "clock.h"
#include <chrono>
#include <thread>

int main() {
    CHECK(uc::ms_to_ticks(1000) == uc::kTimebaseHz);
    CHECK(uc::ticks_to_ms(uc::kTimebaseHz / 2) == 500);
    uint64_t a = uc::now_ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    uint64_t b = uc::now_ticks();
    CHECK(b > a);
    uint64_t ms = uc::ticks_to_ms(b - a);
    CHECK(ms >= 25 && ms < 500);   // monotonic and scaled to the 60.75 MHz time base
    // periodic schedule: the phase is kept, missed slots are skipped, no drift
    CHECK(uc::next_periodic(1000, 100, 1050) == 1100);
    CHECK(uc::next_periodic(1000, 100, 1100) == 1200);
    CHECK(uc::next_periodic(1000, 100, 1350) == 1400);
    uint64_t t = 1000;
    for (int i = 0; i < 1000; ++i) t = uc::next_periodic(t, 100, t + 3);
    CHECK(t == 1000 + 1000 * 100);
    return 0;
}
