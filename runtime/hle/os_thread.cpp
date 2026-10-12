#include "hle/os.h"
#include "dispatch.h"
#include "hle/os_layout.h"
#include "trace.h"
#include <cstdio>

namespace uc {
static Sched* g_sched;
static uint32_t g_dummy_ctx;
static ppc::Ctx g_memctx;   // only used to write guest memory from the scheduler hook
void os_hle_init(Sched& s, uint8_t* guest_mem, uint32_t dummy_context_addr) {
    g_sched = &s;
    g_dummy_ctx = dummy_context_addr;
    g_memctx.mem = guest_mem;
    s.set_switch_hook([](Thread* t) { ppc::wr32(g_memctx, layout::kCurrentThreadPtr, t->guest_addr); });   // OSGetCurrentThread() reads this word
}
Sched& os_sched() { return *g_sched; }
uint32_t os_dummy_context() { return g_dummy_ctx; }
}  // namespace uc

using namespace uc;

// OSCreateThread(thread, entry, param, stack_top, stack_size, priority, attr) -> 1
void hle_OSCreateThread(ppc::Ctx& c) {
    uint32_t th = c.r[3], entry = c.r[4], param = c.r[5], top = c.r[6], size = c.r[7];
    int prio = int(c.r[8]);
    if (prio < 0 || prio > 31) { c.r[3] = 0; return; }                 // the SDK refuses priorities outside 0..31
    ppc::wr16(c, th + layout::kThreadState, 1);
    ppc::wr16(c, th + layout::kThreadAttr, c.r[9] & 1);
    ppc::wr32(c, th + layout::kThreadSuspend, 1);
    ppc::wr32(c, th + layout::kThreadPriority, uint32_t(prio));
    ppc::wr32(c, th + layout::kThreadBasePriority, uint32_t(prio));
    ppc::wr32(c, th + layout::kThreadStackBase, top);
    ppc::wr32(c, th + layout::kThreadStackEnd, top - size);
    ppc::wr32(c, top - size, 0xDEADBABE);   // stack-overflow canary the SDK checks
    // The new thread inherits r2/r13 (small-data bases), GQRs and FPSCR from its creator, as OSContextInit does.
    os_sched().create(th, prio, [=](ppc::Ctx& tc) {
        tc.r[1] = (top & ~7u) - 8;
        ppc::wr32(tc, tc.r[1], 0);
        tc.r[3] = param;
        tc.lr = 0;
        call_indirect(tc, entry);   // returns with the entry function's r3 as the thread's exit value
    }, &c);
    c.r[3] = 1;
}

void hle_OSResumeThread(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    c.r[3] = t ? uint32_t(os_sched().resume(t)) : uint32_t(-1);
}
void hle_OSSuspendThread(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    c.r[3] = t ? uint32_t(os_sched().suspend(t)) : uint32_t(-1);
}
namespace uc { static bool g_log_sleep; void os_log_sleep(bool on) { g_log_sleep = on; } }
void hle_OSSleepThread(ppc::Ctx& c) {   // r3 = OSThreadQueue*
    if (uc::g_log_sleep) { std::fprintf(stderr, "[os] OSSleepThread queue %08X from:\n", c.r[3]); uc::trace_dump(stderr, 8);
        std::fprintf(stderr, "  lr %08X\n", c.lr);
        for (uint32_t sp = c.r[1], i = 0; i < 8 && sp >= 0x80000000u && sp < 0x81800000u; ++i) {   // stack back chain
            uint32_t prev = ppc::rd32(c, sp);
            std::fprintf(stderr, "  frame %08X: caller %08X\n", sp, prev >= 0x80000000u && prev < 0x81800000u ? ppc::rd32(c, prev + 4) : 0u);
            if (prev <= sp) break;
            sp = prev;
        } }
    os_sched().block_on(c.r[3]);
}
void hle_OSWakeupThread(ppc::Ctx& c) { os_sched().wake_all(c.r[3]); }
void hle_OSYieldThread(ppc::Ctx&) { os_sched().yield(); }
void hle_OSExitThread(ppc::Ctx& c) { os_sched().exit_current(c.r[3]); }
void hle_OSJoinThread(ppc::Ctx& c) {                                          // (thread, void** value) -> 1
    Thread* t = os_sched().find(c.r[3]);
    if (!t) { c.r[3] = 0; return; }
    uint32_t v = os_sched().join(t);
    if (c.r[4]) ppc::wr32(c, c.r[4], v);
    c.r[3] = 1;
}
void hle_OSSetThreadPriority(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    int prio = int(c.r[4]);
    if (!t || prio < 0 || prio > 31) { c.r[3] = 0; return; }
    ppc::wr32(c, c.r[3] + layout::kThreadPriority, uint32_t(prio));
    ppc::wr32(c, c.r[3] + layout::kThreadBasePriority, uint32_t(prio));   // OSGetThreadPriority (translated) reads the base priority
    os_sched().set_priority(t, prio);
    c.r[3] = 1;
}
void hle_OSCancelThread(ppc::Ctx& c) {   // (thread); a cancelled thread is moribund and its joiners wake
    Thread* t = os_sched().find(c.r[3]);
    if (!t) return;
    ppc::wr16(c, c.r[3] + layout::kThreadState, 8);
    os_sched().cancel(t);
}
// The SDK's scheduler-lock counter lives in a small-data word (r13-26464); both functions also drive the scheduler.
void hle_OSDisableScheduler(ppc::Ctx& c) {
    uint32_t a = c.r[13] - 26464, prev = ppc::rd32(c, a);
    ppc::wr32(c, a, prev + 1);
    os_sched().lock_scheduler();
    c.r[3] = prev;
}
void hle_OSEnableScheduler(ppc::Ctx& c) {
    uint32_t a = c.r[13] - 26464, prev = ppc::rd32(c, a);
    ppc::wr32(c, a, prev - 1);
    os_sched().unlock_scheduler();
    c.r[3] = prev;
}
