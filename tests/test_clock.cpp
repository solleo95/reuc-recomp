#include "check.h"
#include "clock.h"
#include <chrono>
#include <stdexcept>
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
    // Virtual clock: speed scales elapsed time, changes keep it monotonic.
    {
        const auto a = uc::now_ticks(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); const auto b = uc::now_ticks();
        uc::set_clock_speed(4); CHECK(uc::clock_speed() == 4);
        const auto c = uc::now_ticks(); CHECK(c >= b);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); const auto d = uc::now_ticks();
        const double base = double(b - a), fast = double(d - c);
        CHECK(fast > base * 3.0 && fast < base * 5.5);
        uc::set_clock_speed(1); CHECK(uc::now_ticks() >= d);   // speed_change_is_monotonic
        bool rejected = false; try { uc::set_clock_speed(0); } catch (const std::invalid_argument&) { rejected = true; } CHECK(rejected);
        uc::set_clock_speed(16); CHECK(uc::clock_speed() == 16); uc::set_clock_speed(32); CHECK(uc::clock_speed() == 32); uc::set_clock_speed(1);
        rejected = false; try { uc::set_clock_speed(33); } catch (const std::invalid_argument&) { rejected = true; } CHECK(rejected);
    }
    return 0;
}
