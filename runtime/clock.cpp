#include "clock.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
uint64_t now_ticks() {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint64_t c = uint64_t(q.QuadPart);
    return (c / freq) * kTimebaseHz + (c % freq) * kTimebaseHz / freq;   // (c % freq) * hz < 2^64 for any real counter frequency
}
uint64_t ms_to_ticks(uint64_t ms) { return ms * (kTimebaseHz / 1000); }
uint64_t ticks_to_ms(uint64_t t) { return t / (kTimebaseHz / 1000); }
}  // namespace uc
