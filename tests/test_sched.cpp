#include "check.h"
#include "clock.h"
#include "sched.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
using namespace uc;

static std::vector<std::string> g_log;
static void note(const char* s) { g_log.push_back(s); }
static std::string joined() { std::string r; for (auto& s : g_log) { if (!r.empty()) r += ","; r += s; } return r; }

static void test_priorities_and_join() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* a = s.create(0, 10, [&](ppc::Ctx&) { note("A1"); s.yield(); note("A2"); });
        Thread* b = s.create(0, 10, [&](ppc::Ctx&) { note("B1"); note("B2"); });
        Thread* c = s.create(0, 20, [&](ppc::Ctx&) { note("C"); });
        s.resume(a); s.resume(b); s.resume(c);   // a and b are more urgent than main and run at once; c is not
        note("m1"); s.join(c); note("m2");
    });
    s.run(m);
    CHECK(joined() == "A1,A2,B1,B2,m1,C,m2");
}

static void test_round_robin() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 5, [&](ppc::Ctx&) {
        Thread* x1 = s.create(0, 10, [&](ppc::Ctx&) { note("x1a"); s.yield(); note("x1b"); });
        Thread* x2 = s.create(0, 10, [&](ppc::Ctx&) { note("x2a"); s.yield(); note("x2b"); });
        s.resume(x1); s.resume(x2);
        s.sleep_ticks(ms_to_ticks(20)); note("m");
    });
    s.run(m);
    CHECK(joined() == "x1a,x2a,x1b,x2b,m");
}

static void test_wake_order() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 1, [&](ppc::Ctx&) {
        Thread* lo = s.create(0, 20, [&](ppc::Ctx&) { s.block_on(0x1000); note("lo"); });
        Thread* hi = s.create(0, 5, [&](ppc::Ctx&) { s.block_on(0x1000); note("hi"); });
        Thread* mid = s.create(0, 10, [&](ppc::Ctx&) { s.block_on(0x1000); note("mid"); });
        s.resume(lo); s.resume(hi); s.resume(mid);
        s.sleep_ticks(ms_to_ticks(10));                 // let all three block
        CHECK(s.wake_one(0x1000) == hi);
        CHECK(s.wake_one(0x1000) == mid);
        CHECK(s.wake_one(0x1000) == lo);
        CHECK(s.wake_one(0x1000) == nullptr);
        note("m");
        s.sleep_ticks(ms_to_ticks(10));
    });
    s.run(m);
    CHECK(joined() == "m,hi,mid,lo");
}

static void test_wake_preempts() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 20, [&](ppc::Ctx&) {
        Thread* h = s.create(0, 5, [&](ppc::Ctx&) { s.block_on(0x2000); note("H"); });
        s.resume(h);                       // H preempts, blocks, main continues
        note("m1"); s.wake_all(0x2000);    // H is more urgent: it runs before m2
        note("m2");
    });
    s.run(m);
    CHECK(joined() == "m1,H,m2");
}

static void test_busy_wait_sees_events() {
    Sched s(nullptr); std::atomic<bool> flag{false}; int spins = 0;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        s.post_at(now_ticks() + ms_to_ticks(10), [&] { flag = true; });
        while (!flag) { ++spins; s.safe_point(); }     // a guest spin loop that polls the clock or a flag
    });
    s.run(m);
    CHECK(flag && spins > 0);
}

static void test_events_wait_for_interrupts() {
    Sched s(nullptr); bool fired = false; bool seen_while_disabled = true, ee_in_handler = true;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        CHECK(s.set_interrupts(false) == true);        // returns the previous state
        s.post_at(now_ticks(), [&] { fired = true; ee_in_handler = s.interrupts_enabled(); });
        s.safe_point();
        seen_while_disabled = fired;                   // nothing may be delivered while interrupts are off
        CHECK(s.set_interrupts(true) == false);        // enabling is a safe point: the event runs now
    });
    s.run(m);
    CHECK(!seen_while_disabled && fired);
    CHECK(!ee_in_handler);                             // handlers are entered with interrupts off, like an exception
}

static void test_external_wakes_idle() {
    g_log.clear(); Sched s(nullptr); std::thread t;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        t = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); s.post_external([&] { s.wake_all(0x3000); }); });
        s.block_on(0x3000); note("woken");           // every guest thread is blocked: the idle driver must pick up the post
    });
    s.run(m); t.join();
    CHECK(joined() == "woken");
}

static void test_deadlock_report() {
    g_log.clear(); Sched s(nullptr); std::string msg;
    s.set_deadlock_timeout_ms(100);
    s.set_error_handler([&](const std::string& m) { msg = m; s.shutdown(); });
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) { s.block_on(0xABCD); note("never"); });
    s.run(m);
    CHECK(msg.find("deadlock") != std::string::npos && msg.find("0000ABCD") != std::string::npos);
    CHECK(g_log.empty());
}

static void test_irq_block_is_fatal() {
    Sched s(nullptr); std::string msg;
    s.set_error_handler([&](const std::string& m) { msg = m; s.shutdown(); });
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        s.post_at(now_ticks(), [&] { s.block_on(0x4000); });
        s.sleep_ticks(ms_to_ticks(50));
    });
    s.run(m);
    CHECK(msg.find("interrupt context") != std::string::npos);
}

static void test_exit_and_join_values() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* t = s.create(0, 10, [&](ppc::Ctx&) { note("t"); s.exit_current(7); note("unreachable"); });
        Thread* u = s.create(0, 10, [&](ppc::Ctx& c) { c.r[3] = 9; });   // returning from the entry function exits with r3
        s.resume(t); s.resume(u);
        CHECK(s.join(t) == 7); CHECK(s.join(u) == 9); note("m");
    });
    s.run(m);
    CHECK(joined() == "t,m");
}

static void test_suspend_resume() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* t = s.create(0, 10, [&](ppc::Ctx&) { note("t"); });
        CHECK(s.resume(t) == 1);   // created suspended; resume returns the previous count
    });
    s.run(m);
    CHECK(joined() == "t");
    g_log.clear(); Sched s2(nullptr);
    Thread* m2 = s2.create(0, 5, [&](ppc::Ctx&) {
        Thread* t = s2.create(0, 10, [&](ppc::Ctx&) { note("t"); });
        s2.resume(t);                    // less urgent than m2: ready but not running
        CHECK(s2.suspend(t) == 0);
        s2.sleep_ticks(ms_to_ticks(10)); note("m");
        CHECK(s2.resume(t) == 1);
        s2.sleep_ticks(ms_to_ticks(10)); note("m2");
    });
    s2.run(m2);
    CHECK(joined() == "m,t,m2");
}

static void test_sleep_is_at_least_requested() {
    Sched s(nullptr); uint64_t waited = 0;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        uint64_t a = now_ticks(); s.sleep_ticks(ms_to_ticks(25)); waited = ticks_to_ms(now_ticks() - a);
    });
    s.run(m);
    CHECK(waited >= 24 && waited < 400);
}

static void test_idle_wait_scales() {   // a guest sleep of 200 virtual ms at speed 4 completes in roughly 50 real ms
    set_clock_speed(4);
    Sched s(nullptr); bool done = false;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) { s.sleep_ticks(ms_to_ticks(200)); done = true; });
    const auto start = std::chrono::steady_clock::now(); s.run(m);
    const auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    set_clock_speed(1);
    CHECK(done && real_ms < 150);
}

static void test_blocked_with_interrupts_off() {   // the idle loop runs with interrupts on, and every thread keeps its own interrupt state
    g_log.clear(); Sched s(nullptr); bool b_saw_enabled = false, a_back_disabled = false;
    s.set_deadlock_timeout_ms(500);
    s.set_error_handler([&](const std::string& m) { note(("error: " + m).c_str()); s.shutdown(); });
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* b = s.create(0, 10, [&](ppc::Ctx&) { b_saw_enabled = s.interrupts_enabled(); s.block_on(0x7001); });
        s.resume(b);                                   // b has the higher priority: runs now, blocks, control returns here
        s.set_interrupts(false);
        s.post_at(now_ticks() + ms_to_ticks(20), [&] { s.wake_all(0x7000); });
        s.block_on(0x7000);                            // blocks with interrupts off: the timer event must still arrive
        a_back_disabled = !s.interrupts_enabled();
        note("woken");
    });
    s.run(m);
    CHECK(joined() == "woken" && b_saw_enabled && a_back_disabled);
}

static void test_irq_context_uses_small_data_bases() {   // interrupt handlers run with the interrupted thread's r2/r13 (SDA bases)
    Sched s(nullptr); uint32_t seen2 = 0, seen13 = 0;
    Thread* m = s.create(0, 16, [&](ppc::Ctx& c) {
        c.r[2] = 0x806C0000; c.r[13] = 0x806C4700;
        s.post_at(now_ticks(), [&] { seen2 = s.irq_ctx().r[2]; seen13 = s.irq_ctx().r[13]; });
        s.safe_point();
    });
    s.run(m);
    CHECK(seen2 == 0x806C0000 && seen13 == 0x806C4700);
}

static void test_scheduler_lock() {   // OSDisableScheduler: a more urgent thread that becomes ready waits until the scheduler is enabled again
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* t = s.create(0, 10, [&](ppc::Ctx&) { note("t"); });
        s.lock_scheduler();
        s.resume(t);
        note("m-locked");
        s.unlock_scheduler();                          // the switch happens here
        note("m-after");
    });
    s.run(m);
    CHECK(joined() == "m-locked,t,m-after");
}

static void test_cancel() {   // OSCancelThread: a waiting or never-started thread dies, joiners wake, its wait is dropped
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* w = s.create(0, 10, [&](ppc::Ctx&) { note("w1"); s.block_on(0x8000); note("w-never"); });
        Thread* n = s.create(0, 10, [&](ppc::Ctx&) { note("n-never"); });
        s.resume(w);                                   // runs, blocks
        s.cancel(w); s.cancel(n);
        CHECK(s.wake_all(0x8000) == 0);                // the cancelled thread no longer waits
        CHECK(s.join(w) == uint32_t(-1) && s.join(n) == uint32_t(-1));
        note("m-done");
    });
    s.run(m);
    CHECK(joined() == "w1,m-done");
}

static void test_dump_contexts() {   // crash bundle: every live thread with its registers, the running one marked
    Sched s(nullptr);
    Thread* m = s.create(0x80123400, 16, [&](ppc::Ctx& mc) {
        Thread* t = s.create(0x80567800, 10, [&](ppc::Ctx& c) { c.r[5] = 0xDEADBEEF; c.lr = 0x80001000; c.f[3] = 1.0; c.cr[0] = 8; c.cr[7] = 2; s.block_on(0x2000); });
        s.resume(t);                                   // more urgent than main: runs until it blocks
        mc.r[1] = 0x8070B398;
        const std::string text = s.dump_contexts();
        CHECK(text.find("thread 1 osthread 80123400 state running priority 16 waitkey 00000000 current 1") != std::string::npos);
        CHECK(text.find("thread 2 osthread 80567800 state waiting priority 10 waitkey 00002000 current 0") != std::string::npos);
        CHECK(text.find("r1=8070B398") != std::string::npos && text.find("r5=DEADBEEF") != std::string::npos);
        CHECK(text.find("lr=80001000") != std::string::npos && text.find("cr=80000002") != std::string::npos);
        CHECK(text.find("f3=3FF0000000000000") != std::string::npos);
        s.wake_one(0x2000);
    });
    s.run(m);
}

int main() {
    test_dump_contexts();
    test_priorities_and_join(); test_round_robin(); test_wake_order(); test_wake_preempts();
    test_busy_wait_sees_events(); test_events_wait_for_interrupts(); test_external_wakes_idle();
    test_deadlock_report(); test_irq_block_is_fatal(); test_exit_and_join_values();
    test_suspend_resume(); test_sleep_is_at_least_requested(); test_idle_wait_scales(); test_blocked_with_interrupts_off(); test_irq_context_uses_small_data_bases(); test_scheduler_lock(); test_cancel();
    return 0;
}
