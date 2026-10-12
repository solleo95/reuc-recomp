#pragma once
#include "clock.h"
#include "sched.h"
#include <functional>

namespace uc {
constexpr uint64_t kViPeriodNtsc = kTimebaseHz * 1001 / 60000;   // 59.94 Hz
constexpr uint64_t kViPeriodPal = kTimebaseHz / 50;

struct ViStats { uint64_t retraces = 0; uint32_t current_fb = 0, next_fb = 0, tv_mode = 0; };

void vi_hle_init(Sched& s, uint64_t period_ticks, std::function<void()> on_retrace);   // after os_hle_init
ViStats vi_stats();
}
