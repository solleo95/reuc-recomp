#include "clock.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace uc {
namespace {
uint64_t real_ticks() {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint64_t c = uint64_t(q.QuadPart);
    return (c / freq) * kTimebaseHz + (c % freq) * kTimebaseHz / freq;   // (c % freq) * hz < 2^64 for any real counter frequency
}
// Seqlock: speed changes are rare, now_ticks is hot.
std::atomic<uint32_t> version{0}, speed{1};
std::atomic<uint64_t> real_base{0}, virtual_base{0};
std::mutex writer;
}  // namespace

uint64_t now_ticks() {
    for (;;) {
        const uint32_t v = version.load(std::memory_order_acquire);
        if (v & 1) continue;
        const uint64_t r = real_base.load(std::memory_order_relaxed), b = virtual_base.load(std::memory_order_relaxed);
        const uint32_t s = speed.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (version.load(std::memory_order_relaxed) != v) continue;
        return b + (real_ticks() - r) * s;
    }
}
void set_clock_speed(uint32_t factor) {
    if (factor < 1 || factor > 32) throw std::invalid_argument("clock speed must be 1..32");
    std::lock_guard lock(writer);
    const uint64_t current = now_ticks(), real = real_ticks();
    version.fetch_add(1, std::memory_order_acq_rel);
    real_base.store(real, std::memory_order_relaxed); virtual_base.store(current, std::memory_order_relaxed); speed.store(factor, std::memory_order_relaxed);
    version.fetch_add(1, std::memory_order_release);
}
uint32_t clock_speed() { return speed.load(std::memory_order_acquire); }
uint64_t ms_to_ticks(uint64_t ms) { return ms * (kTimebaseHz / 1000); }
uint64_t ticks_to_ms(uint64_t t) { return t / (kTimebaseHz / 1000); }
}  // namespace uc
