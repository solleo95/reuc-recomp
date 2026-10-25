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
std::atomic<bool> det_mode{false};
std::atomic<uint64_t> det_ticks{0};
thread_local bool guest_thread = false;
constexpr uint64_t kDetStep = 60;   // about 1 microsecond of guest time per time read
}  // namespace

void set_deterministic_clock(bool on) { det_ticks = 0; det_mode = on; }
bool deterministic_clock() { return det_mode.load(std::memory_order_relaxed); }
void det_clock_advance_to(uint64_t tick) { uint64_t cur = det_ticks.load(); while (cur < tick && !det_ticks.compare_exchange_weak(cur, tick)) {} }
uint64_t det_clock_checkpoint() {
    if(!deterministic_clock()) throw std::logic_error("checkpoint requires deterministic clock");
    return det_ticks.load();
}
void det_clock_restore(uint64_t tick) {
    if(!deterministic_clock() || !is_guest_thread()) throw std::logic_error("clock restore requires deterministic guest baton");
    det_ticks.store(tick);
}
void mark_guest_thread() { guest_thread = true; }
bool is_guest_thread() { return guest_thread; }

uint64_t now_ticks() {
    if (det_mode.load(std::memory_order_relaxed)) return guest_thread ? det_ticks.fetch_add(kDetStep) + kDetStep : det_ticks.load();
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
