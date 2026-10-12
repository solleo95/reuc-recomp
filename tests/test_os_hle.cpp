#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "guest_call.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include <cstring>
#include <string>
#include <vector>
using namespace ppc;

// hle entry points (declared by the generated decl.h in the real build)
void hle_OSCreateThread(Ctx&); void hle_OSResumeThread(Ctx&); void hle_OSExitThread(Ctx&); void hle_OSJoinThread(Ctx&);
void hle_OSSleepThread(Ctx&); void hle_OSWakeupThread(Ctx&); void hle_OSYieldThread(Ctx&);
void hle_OSInitMutex(Ctx&); void hle_OSLockMutex(Ctx&); void hle_OSUnlockMutex(Ctx&);
void hle_OSInitMessageQueue(Ctx&); void hle_OSSendMessage(Ctx&); void hle_OSReceiveMessage(Ctx&);
void hle_OSDisableInterrupts(Ctx&); void hle_OSRestoreInterrupts(Ctx&); void hle_OSEnableInterrupts(Ctx&);
void hle_OSSetThreadPriority(Ctx&); void hle_OSCancelThread(Ctx&); void hle_OSDisableScheduler(Ctx&); void hle_OSEnableScheduler(Ctx&);
void hle_OSGetTime(Ctx&); void hle_OSGetTick(Ctx&); void hle_OSSetAlarm(Ctx&); void hle_OSSetPeriodicAlarm(Ctx&); void hle_OSCancelAlarm(Ctx&);

static std::vector<std::string> g_log;
static void note(const std::string& s) { g_log.push_back(s); }
static std::string joined() { std::string r; for (auto& s : g_log) { if (!r.empty()) r += ","; r += s; } return r; }

// "Guest" functions: plain C++ functions registered in the dispatch table at fake guest addresses.
static void guest_worker(Ctx& c) { note("worker(" + std::to_string(c.r[3]) + ")"); c.r[3] = c.r[3] * 2; }          // returns param*2
static void guest_alarm(Ctx& c) { note("alarm(" + std::to_string(c.r[3]) + ")"); }

static void call(Ctx& c, void (*fn)(Ctx&), std::initializer_list<uint32_t> args) { int i = 3; for (uint32_t a : args) c.r[i++] = a; fn(c); }

int main() {
    Fix f; uc::Sched s(f.c.mem);
    static const uc::FnEntry table[] = {{0x80100000, guest_worker}, {0x80100004, guest_alarm}};
    uc::dispatch_init(table, 2, 0x80100000, 0x80100008);
    uc::os_hle_init(s, f.c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;

    {   // printf over guest varargs
        Ctx& c = f.c;
        const char* fmt = "%s=%d (%x) %c %5.2f %%";
        for (size_t i = 0; i <= strlen(fmt); ++i) wr8(c, 0x80350000 + uint32_t(i), uint32_t(fmt[i]));
        const char* name = "val";
        for (size_t i = 0; i <= strlen(name); ++i) wr8(c, 0x80350100 + uint32_t(i), uint32_t(name[i]));
        c.r[4] = 0x80350100; c.r[5] = uint32_t(-5); c.r[6] = 0xBEEF; c.r[7] = 'Z'; c.f[1] = 3.14159;
        CHECK(uc::format_guest(c, 0x80350000, 4) == "val=-5 (beef) Z  3.14 %");
    }

    uc::Thread* m = s.create(0, 16, [&](Ctx& c) {
        // thread create/resume/join: entry gets r3 = param; join returns the entry's r3
        call(c, hle_OSCreateThread, {0x80300000, 0x80100000, 21, 0x80420000, 0x4000, 10, 0});
        CHECK(c.r[3] == 1);
        CHECK(rd32(c, 0x80420000 - 0x4000) == 0xDEADBABE);           // stack canary at the lowest address
        CHECK(rd32(c, 0x80300000 + 0x2D0) == 10);                    // priority field of the guest OSThread
        call(c, hle_OSResumeThread, {0x80300000});
        CHECK(c.r[3] == 1);                                          // previous suspend count; the new thread was more urgent and ran already
        call(c, hle_OSJoinThread, {0x80300000, 0x80310000});
        CHECK(c.r[3] == 1 && rd32(c, 0x80310000) == 42);

        // priority: both guest fields change; values outside 0..31 are refused (SDK range check), also at creation
        call(c, hle_OSSetThreadPriority, {0x80300000, 5});
        CHECK(c.r[3] == 1 && rd32(c, 0x80300000 + 0x2D0) == 5 && rd32(c, 0x80300000 + 0x2D4) == 5);
        call(c, hle_OSSetThreadPriority, {0x80300000, 32});
        CHECK(c.r[3] == 0 && rd32(c, 0x80300000 + 0x2D4) == 5);
        call(c, hle_OSCreateThread, {0x80300400, 0x80100000, 0, 0x80430000, 0x4000, uint32_t(-1), 0});
        CHECK(c.r[3] == 0);
        // cancel: a suspended thread that never ran dies; its guest state becomes moribund and a join returns at once
        call(c, hle_OSCreateThread, {0x80300800, 0x80100000, 1, 0x80440000, 0x4000, 10, 0});
        call(c, hle_OSCancelThread, {0x80300800});
        CHECK(rd16(c, 0x80300800 + 0x2C8) == 8);
        call(c, hle_OSJoinThread, {0x80300800, 0});
        CHECK(c.r[3] == 1);
        // scheduler lock: the guest counter (r13-26464) and the scheduler follow
        c.r[13] = 0x80450000;
        wr32(c, c.r[13] - 26464, 0);
        call(c, hle_OSDisableScheduler, {});
        CHECK(c.r[3] == 0 && rd32(c, c.r[13] - 26464) == 1);
        call(c, hle_OSEnableScheduler, {});
        CHECK(c.r[3] == 1 && rd32(c, c.r[13] - 26464) == 0);

        // mutex: recursive for the owner
        call(c, hle_OSInitMutex, {0x80320000});
        call(c, hle_OSLockMutex, {0x80320000}); call(c, hle_OSLockMutex, {0x80320000});
        call(c, hle_OSUnlockMutex, {0x80320000});
        call(c, hle_OSUnlockMutex, {0x80320000});

        // message queue: non-blocking receive on empty fails; full queue refuses a non-blocking send
        call(c, hle_OSInitMessageQueue, {0x80330000, 0x80331000, 2});
        call(c, hle_OSReceiveMessage, {0x80330000, 0x80332000, 0});      // OS_MESSAGE_NOBLOCK
        CHECK(c.r[3] == 0);
        call(c, hle_OSSendMessage, {0x80330000, 77, 0});  CHECK(c.r[3] == 1);
        call(c, hle_OSSendMessage, {0x80330000, 78, 0});  CHECK(c.r[3] == 1);
        call(c, hle_OSSendMessage, {0x80330000, 79, 0});  CHECK(c.r[3] == 0);   // full, non-blocking
        call(c, hle_OSReceiveMessage, {0x80330000, 0x80332000, 1});
        CHECK(c.r[3] == 1 && rd32(c, 0x80332000) == 77);

        // interrupts: Disable returns the previous state, Restore returns the state it replaced
        call(c, hle_OSDisableInterrupts, {});         CHECK(c.r[3] == 1);
        call(c, hle_OSDisableInterrupts, {});         CHECK(c.r[3] == 0);
        call(c, hle_OSRestoreInterrupts, {1});        CHECK(c.r[3] == 0);
        call(c, hle_OSEnableInterrupts, {});          CHECK(c.r[3] == 1);

        // time: 64-bit OSGetTime in r3:r4 never goes backwards, OSGetTick is its low word
        call(c, hle_OSGetTime, {});
        uint64_t t1 = uint64_t(c.r[3]) << 32 | c.r[4];
        call(c, hle_OSGetTick, {});
        uint64_t t2 = uint64_t(c.r[3]);
        CHECK(t1 != 0 && (t2 >= (t1 & 0xFFFFFFFF) || t2 < 0x1000));

        // alarm: handler runs in interrupt context with r3 = the alarm; cancel stops a pending one
        g_log.clear();
        uint64_t rel = uc::ms_to_ticks(10);
        call(c, hle_OSSetAlarm, {0x80340000, uint32_t(rel >> 32), uint32_t(rel), 0x80100004});
        call(c, hle_OSSetAlarm, {0x80341000, uint32_t(rel >> 32), uint32_t(rel), 0x80100004});
        call(c, hle_OSCancelAlarm, {0x80341000});
        s.sleep_ticks(uc::ms_to_ticks(30));
        CHECK(joined() == "alarm(" + std::to_string(0x80340000) + ")");
        // periodic alarm fires repeatedly until cancelled
        g_log.clear();
        uint64_t start = uc::now_ticks() + uc::ms_to_ticks(5), period = uc::ms_to_ticks(10);
        call(c, hle_OSSetPeriodicAlarm, {0x80342000, uint32_t(start >> 32), uint32_t(start), uint32_t(period >> 32), uint32_t(period), 0x80100004});
        s.sleep_ticks(uc::ms_to_ticks(45));
        call(c, hle_OSCancelAlarm, {0x80342000});
        size_t n = g_log.size();
        CHECK(n >= 3 && n <= 6);
        s.sleep_ticks(uc::ms_to_ticks(30));
        CHECK(g_log.size() == n);
        // a start time in the past joins the next period instead of firing at once
        g_log.clear();
        uint64_t past = uc::now_ticks() - uc::ms_to_ticks(100), p2 = uc::ms_to_ticks(40);
        call(c, hle_OSSetPeriodicAlarm, {0x80342100, uint32_t(past >> 32), uint32_t(past), uint32_t(p2 >> 32), uint32_t(p2), 0x80100004});
        s.sleep_ticks(uc::ms_to_ticks(3));
        CHECK(g_log.empty());
        call(c, hle_OSCancelAlarm, {0x80342100});
    });
    g_log.clear();
    s.run(m);
    return 0;
}
