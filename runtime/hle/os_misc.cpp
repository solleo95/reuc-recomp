#include "hle/os.h"
#include "clock.h"
#include "fatal.h"
#include "guest_call.h"
#include <cstdio>
#include <map>

using namespace uc;

namespace {
struct Alarm { uint64_t gen = 0; };
std::map<uint32_t, Alarm> g_alarms;

void arm(uint32_t alarm, uint64_t at, uint64_t period, uint32_t handler, uint64_t gen) {
    os_sched().post_at(at, [=] {
        auto it = g_alarms.find(alarm);
        if (it == g_alarms.end() || it->second.gen != gen) return;   // cancelled or re-armed
        if (period) arm(alarm, next_periodic(at, period, now_ticks()), period, handler, gen);   // absolute schedule: a late tick must not shift later ticks
        call_guest(os_sched().irq_ctx(), handler, {alarm, os_dummy_context()});
    });
}
}  // namespace

void hle_OSDisableInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(false) ? 1 : 0; }
void hle_OSEnableInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(true) ? 1 : 0; }
void hle_OSRestoreInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(c.r[3] != 0) ? 1 : 0; }

void hle_OSGetTime(ppc::Ctx& c) {
    uint64_t t = now_ticks();
    c.r[3] = uint32_t(t >> 32);
    c.r[4] = uint32_t(t);
    os_sched().safe_point();   // a guest that polls the time may be waiting for an event
}
void hle_OSGetTick(ppc::Ctx& c) {
    c.r[3] = uint32_t(now_ticks());
    os_sched().safe_point();
}

// OSSetAlarm(alarm, relative_ticks(r4:r5), handler(r6)); OSSetPeriodicAlarm(alarm, absolute_start(r4:r5), period(r6:r7), handler(r8))
void hle_OSSetAlarm(ppc::Ctx& c) {
    uint64_t rel = uint64_t(c.r[4]) << 32 | c.r[5];
    uint64_t gen = ++g_alarms[c.r[3]].gen;
    arm(c.r[3], now_ticks() + rel, 0, c.r[6], gen);
}
void hle_OSSetPeriodicAlarm(ppc::Ctx& c) {
    uint64_t start = uint64_t(c.r[4]) << 32 | c.r[5], period = uint64_t(c.r[6]) << 32 | c.r[7];
    if (!period) return;
    uint64_t gen = ++g_alarms[c.r[3]].gen;
    uint64_t now = now_ticks();
    arm(c.r[3], start > now ? start : next_periodic(start, period, now), period, c.r[8], gen);   // a start in the past joins the next period
}
void hle_OSCancelAlarm(ppc::Ctx& c) { ++g_alarms[c.r[3]].gen; }

void hle_OSReport(ppc::Ctx& c) {
    std::string s = format_guest(c, c.r[3], 4);
    std::fprintf(stderr, "[OSReport] %s", s.c_str());
    if (s.empty() || s.back() != '\n') std::fputc('\n', stderr);
}
void hle_OSPanic(ppc::Ctx& c) {   // (file, line, fmt, ...)
    std::string file;
    for (uint32_t a = c.r[3]; file.size() < 200; ++a) { char ch = char(ppc::rd8(c, a)); if (!ch) break; file.push_back(ch); }
    std::string msg = format_guest(c, c.r[5], 6);
    uc::fatal("OSPanic %s:%u: %s", file.c_str(), c.r[4], msg.c_str());
}
