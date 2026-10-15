#pragma once
#include <cstdint>
namespace uc {
constexpr uint64_t kTimebaseHz = 60750000ull;   // bus clock 243 MHz / 4
uint64_t now_ticks();
void set_clock_speed(uint32_t factor);   // test mode: 1..32, virtual time = base + real elapsed * factor
uint32_t clock_speed();
uint64_t ms_to_ticks(uint64_t ms);
uint64_t ticks_to_ms(uint64_t ticks);

// Next point of the schedule `scheduled + k*period` that lies after `now`; slots missed while busy are skipped, the phase is kept.
inline uint64_t next_periodic(uint64_t scheduled, uint64_t period, uint64_t now) {
    uint64_t next = scheduled + period;
    if (next <= now) next += ((now - next) / period + 1) * period;
    return next;
}
}
