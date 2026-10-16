#include "sched.h"
#include "clock.h"
#include "crash_report.h"
#include "fatal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {

namespace {
struct Tramp { Sched* s; Thread* t; };
DWORD WINAPI tramp(LPVOID p) {
    std::unique_ptr<Tramp> tp(static_cast<Tramp*>(p));
    uc::crash_guard_thread();
    uc::mark_guest_thread();
    tp->s->thread_main(tp->t);
    return 0;
}
}  // namespace

Sched::Sched(uint8_t* mem, size_t host_stack) : mem_(mem), host_stack_(host_stack) { irq_ctx_.mem = mem; }

Sched::~Sched() {
    shutdown();
    join_hosts();
}

void Sched::join_hosts() {
    if (!shutdown_) throw std::logic_error("join_hosts requires shutdown");
    std::lock_guard joining(join_m_);
    std::vector<void*> handles;
    { std::lock_guard lock(baton_m_); handles.swap(handles_); }
    for (void* handle : handles) {
        DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle), 5000);
        if (result != WAIT_OBJECT_0) fatal("guest host thread failed to stop: wait result %lu", result);
        if (!CloseHandle(static_cast<HANDLE>(handle))) fatal("cannot close joined guest thread: Windows error %lu", GetLastError());
    }
    for (auto& thread : threads_) if (thread->started) thread->state = Thread::State::Dead;
    running_ = nullptr;
}

Thread* Sched::create(uint32_t guest_addr, int priority, Body body, const ppc::Ctx* inherit) {
    std::lock_guard lock(baton_m_);
    if (shutdown_) throw ThreadExit{};
    auto t = std::make_unique<Thread>();
    t->id = ++next_id_;
    t->guest_addr = guest_addr;
    if (t->id < 32) entry_[t->id] = guest_addr;
    t->priority = priority;
    t->body = std::move(body);
    if (inherit) t->ctx = *inherit;
    t->ctx.mem = mem_;
    threads_.push_back(std::move(t));
    return threads_.back().get();
}

Thread* Sched::find(uint32_t guest_addr) const {   // newest first, finished threads included: joining a finished thread returns its value
    for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) if ((*it)->guest_addr == guest_addr) return it->get();
    return nullptr;
}

void Sched::make_ready(Thread* t, bool front) {
    t->state = Thread::State::Ready;
    t->seq = front ? --front_seq_ : ++back_seq_;
}

Thread* Sched::pick_ready() {
    Thread* best = nullptr;
    for (const auto& up : threads_) {
        Thread* t = up.get();
        if (t->state != Thread::State::Ready) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->seq < best->seq)) best = t;
    }
    return best;
}

void Sched::wake(Thread* t) {
    t->wait_key = 0;
    if (t->suspend_count > 0) t->state = Thread::State::Suspended; else make_ready(t);
}

void Sched::preempt_check() {
    if (sched_lock_ > 0 || in_irq_ || !running_ || running_->state != Thread::State::Running) return;
    Thread* best = pick_ready();
    if (best && best->priority < running_->priority) {
        Thread* self = running_;
        make_ready(self, true);   // a preempted thread keeps its place at the front of its priority
        reschedule(self);
    }
}

int Sched::resume(Thread* t) {
    int prev = t->suspend_count;
    if (t->suspend_count > 0) --t->suspend_count;
    if (t->suspend_count == 0 && t->state == Thread::State::Suspended) { make_ready(t); preempt_check(); }
    return prev;
}

int Sched::suspend(Thread* t) {
    int prev = t->suspend_count++;
    if (t->state == Thread::State::Ready) t->state = Thread::State::Suspended;
    else if (t == running_ && !in_irq_) { t->state = Thread::State::Suspended; reschedule(t); }
    return prev;   // a waiting thread stays waiting and becomes suspended when it is woken
}

void Sched::yield() {
    if (in_irq_) return;
    Thread* self = running_;
    make_ready(self);
    reschedule(self);
}

void Sched::block_on(uint32_t key) {
    if (in_irq_) {
        char b[16];
        std::snprintf(b, sizeof b, "%08X", key);
        fail(std::string("blocking call from interrupt context (wait key ") + b + ")");
    }
    Thread* self = running_;
    self->state = Thread::State::Waiting;
    self->wait_key = key;
    waitq_[key].push_back(self);
    reschedule(self);
}

Thread* Sched::wake_one(uint32_t key) {
    auto it = waitq_.find(key);
    if (it == waitq_.end() || it->second.empty()) return nullptr;
    auto& q = it->second;
    size_t best = 0;
    for (size_t i = 1; i < q.size(); ++i) if (q[i]->priority < q[best]->priority) best = i;
    Thread* t = q[best];
    q.erase(q.begin() + long(best));
    if (q.empty()) waitq_.erase(it);
    wake(t);
    preempt_check();
    return t;
}

int Sched::wake_all(uint32_t key) {
    auto it = waitq_.find(key);
    if (it == waitq_.end()) return 0;
    std::vector<Thread*> q = std::move(it->second);
    waitq_.erase(it);
    for (Thread* t : q) wake(t);
    preempt_check();
    return int(q.size());
}

void Sched::sleep_ticks(uint64_t ticks) {
    uint32_t key = sleep_key(running_);
    post_at(now_ticks() + ticks, [this, key] { wake_all(key); });
    block_on(key);
}

void Sched::set_priority(Thread* t, int priority) {
    t->priority = priority;
    preempt_check();
}

uint32_t Sched::join(Thread* t) {
    while (t->state != Thread::State::Dead) block_on(join_key(t));
    return t->exit_value;
}

void Sched::finish(Thread* self, uint32_t value) {
    self->exit_value = value;
    self->state = Thread::State::Dead;
    if (self == main_) { { std::lock_guard<std::mutex> lk(done_m_); main_done_ = true; } done_cv_.notify_all(); }
    wake_all(join_key(self));
    reschedule(self);
}

void Sched::unlock_scheduler() {
    if (sched_lock_ > 0 && --sched_lock_ == 0) preempt_check();
}

void Sched::cancel(Thread* t) {
    if (t->state == Thread::State::Dead) return;
    if (t == running_) exit_current(uint32_t(-1));
    if (t->wait_key) {
        auto it = waitq_.find(t->wait_key);
        if (it != waitq_.end()) {
            auto& q = it->second;
            q.erase(std::remove(q.begin(), q.end(), t), q.end());
            if (q.empty()) waitq_.erase(it);
        }
        t->wait_key = 0;
    }
    t->exit_value = uint32_t(-1);
    t->state = Thread::State::Dead;
    { std::lock_guard<std::mutex> lk(baton_m_); t->cancelled = true; }
    t->cv.notify_all();                       // a started host thread unwinds; a thread that never ran never starts
    wake_all(join_key(t));
}

void Sched::exit_current(uint32_t value) {
    finish(running_, value);
    throw ThreadExit{};
}

void Sched::post_at(uint64_t tick, Event fn) { timed_.emplace(tick, std::move(fn)); }

void Sched::post_external(Event fn) {
    if (det_ && is_guest_thread()) { timed_.emplace(0, std::move(fn)); return; }   // the baton holder's own event: due at once, in posting order
    {
        std::lock_guard<std::mutex> lk(ext_m_);
        if (shutdown_) return;
        if (det_ && async_pending_ > 0) --async_pending_;   // the announced result of another thread has arrived
        ext_q_.push_back(std::move(fn));
    }
    ext_cv_.notify_all();
}

void Sched::deliver_events() {
    if (in_irq_ || !ee_) return;
    std::vector<Event> ext;
    if (ext_allowed_) { std::lock_guard<std::mutex> lk(ext_m_); ext.swap(ext_q_); }
    for (auto& e : ext) timed_.emplace(0, std::move(e));
    while (!timed_.empty() && timed_.begin()->first <= now_ticks()) {
        if (shutdown_) throw ThreadExit{};
        Event e = std::move(timed_.begin()->second);
        timed_.erase(timed_.begin());
        if (running_) { irq_ctx_.r[2] = running_->ctx.r[2]; irq_ctx_.r[13] = running_->ctx.r[13]; }   // the handler runs on the interrupted thread's SDA bases
        bool saved_ee = ee_;
        ee_ = false;   // an exception enters the handler with EE cleared
        in_irq_ = true;
        e();
        in_irq_ = false;
        ee_ = saved_ee;
    }
}

void Sched::safe_point() {
    if (shutdown_) throw ThreadExit{};
    deliver_events();
    preempt_check();
}

bool Sched::set_interrupts(bool enabled) {
    bool prev = ee_;
    ee_ = enabled;
    if (enabled && !prev) safe_point();
    return prev;
}

std::string Sched::dump_threads() const {
    std::string s;
    static const char* names[] = {"suspended", "ready", "running", "waiting", "dead"};
    for (const auto& t : threads_) {
        if (t->state == Thread::State::Dead) continue;
        char b[160];
        std::snprintf(b, sizeof b, "  thread %d (OSThread %08X) priority %d %s wait key %08X\n", t->id, t->guest_addr, t->priority, names[int(t->state)], t->wait_key);
        s += b;
    }
    return s;
}

std::string Sched::idle_report() const {
    std::lock_guard<std::mutex> lg(idle_report_m_);
    std::string s; char b[160];
    for (const auto& [key, st] : idle_by_) {
        std::snprintf(b, sizeof b, "# idle %7.1f ms in %5llu waits (max %.1f ms, %llu ended by an event, rest by a timer): ", st.total_ns / 1e6, (unsigned long long)st.count, st.max_ns / 1e6, (unsigned long long)st.by_event);
        s += b; s += key; s += "\n";
    }
    for (const auto& e : idle_events_) {
        std::snprintf(b, sizeof b, "# idle-wait end %.1f ms steady, %.1f ms, ended by %s: ", e.end_ns / 1e6, e.duration_ns / 1e6, e.by_event ? "event" : "timer");
        s += b; s += e.key; s += "\n";
    }
    return s;
}

std::string Sched::dump_contexts() const {
    std::string s;
    static const char* names[] = {"suspended", "ready", "running", "waiting", "dead"};
    char b[96];
    for (const auto& t : threads_) {
        if (t->state == Thread::State::Dead) continue;
        std::snprintf(b, sizeof b, "thread %d osthread %08X state %s priority %d waitkey %08X current %d\n", t->id, t->guest_addr, names[int(t->state)], t->priority, t->wait_key, t.get() == running_);
        s += b;
        const ppc::Ctx& c = t->ctx;
        for (unsigned i = 0; i < 32; ++i) { std::snprintf(b, sizeof b, "r%u=%08X%c", i, c.r[i], i % 8 == 7 ? '\n' : ' '); s += b; }
        uint32_t cr = 0; for (unsigned i = 0; i < 8; ++i) cr = cr << 4 | (c.cr[i] & 15);
        std::snprintf(b, sizeof b, "lr=%08X ctr=%08X cr=%08X msr=%08X\n", c.lr, c.ctr, cr, c.msr); s += b;
        for (unsigned i = 0; i < 32; ++i) { uint64_t bits; std::memcpy(&bits, &c.f[i], 8); std::snprintf(b, sizeof b, "f%u=%016llX%c", i, (unsigned long long)bits, i % 4 == 3 ? '\n' : ' '); s += b; }
    }
    return s;
}

void Sched::fail(const std::string& msg) {
    if (err_) { err_(msg); throw ThreadExit{}; }
    fatal("%s", msg.c_str());
}

void Sched::idle_wait() {
    std::unique_lock<std::mutex> lk(ext_m_);
    if (!ext_q_.empty() || shutdown_) return;
    if (det_) {
        if (async_pending_ > 0) {   // a DVD read or a GPU frame is on its way: its arrival, not the host speed, decides what happens next
            if (!ext_cv_.wait_for(lk, std::chrono::milliseconds(deadlock_ms_), [&] { return !ext_q_.empty() || shutdown_; })) { lk.unlock(); fail("deterministic mode: an announced asynchronous result never arrived\n" + dump_threads()); }
            return;
        }
        if (!timed_.empty()) { det_clock_advance_to(timed_.begin()->first); return; }   // idle: time jumps to the next event
        lk.unlock();
        fail("deadlock: all guest threads are blocked and no event is pending\n" + dump_threads());
        return;
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlock_ms_);
    bool timed = false;
    if (!timed_.empty()) {   // the idle loop of the real OS runs with interrupts on, whatever state the blocked threads had
        uint64_t t = timed_.begin()->first, now = now_ticks();
        until = std::chrono::steady_clock::now() + std::chrono::milliseconds(t > now ? ticks_to_ms(t - now) / clock_speed() + 1 : 0);
        timed = true;
    }
    const uint64_t idle_start = steady_ns();
    bool woke = ext_cv_.wait_until(lk, until, [&] { return !ext_q_.empty() || shutdown_; });
    const uint64_t idle_duration = steady_ns() - idle_start;
    idle_ns_ += idle_duration;
    if (idle_duration >= 1000000) {   // diagnosis of frame-time spikes: which threads were blocked on what
        std::string key; char b[48];
        for (const auto& t : threads_) if (t->state == Thread::State::Waiting) { std::snprintf(b, sizeof b, "t%d:%08X@%08X ", t->id, t->wait_key, t->ctx.lr); key += b; }
        std::lock_guard<std::mutex> lg(idle_report_m_);
        auto& st = idle_by_[key]; ++st.count; st.total_ns += idle_duration; if (idle_duration > st.max_ns) st.max_ns = idle_duration; st.by_event += woke;
        if (idle_duration >= 2000000 && idle_events_.size() < 4000) idle_events_.push_back({steady_ns(), idle_duration, woke, key});
    }
    if (!woke && !timed) {
        lk.unlock();
        fail("deadlock: all guest threads are blocked and no event is pending\n" + dump_threads());
    }
}

void Sched::reschedule(Thread* self) {
    self->ee = ee_;   // a thread keeps its interrupt state across a switch, like the MSR saved in its OSContext
    for (;;) {
        if (shutdown_) {
            if (self->state == Thread::State::Dead) return;
            throw ThreadExit{};
        }
        deliver_events();
        ext_allowed_ = !det_;
        if (Thread* n = pick_ready()) { handoff(self, n); return; }
        if (main_done_) { if (self->state == Thread::State::Dead) return; throw ThreadExit{}; }
        ee_ = true;   // idle
        ext_allowed_ = true;
        idle_wait();
    }
}

uint64_t Sched::thread_cycles() { ULONG64 cycles = 0; QueryThreadCycleTime(GetCurrentThread(), &cycles); return cycles; }

uint64_t Sched::steady_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

void Sched::handoff(Thread* self, Thread* next) {
    if (next == self) { self->state = Thread::State::Running; ee_ = self->ee; return; }
    const uint64_t now = steady_ns();
    if (self->id < 32 && run_started_ns_) run_ns_[self->id] += now - run_started_ns_;
    if (self->id < 32 && cycle_started_) run_cycles_[self->id] += thread_cycles() - cycle_started_;
    ++switches_; notified_ns_ = now;
    next->state = Thread::State::Running;
    ee_ = next->ee;
    if (switch_hook_) switch_hook_(next);
    {
        std::lock_guard<std::mutex> lk(baton_m_);
        running_ = next;
        if (!next->started) start_host(next);
    }
    next->cv.notify_one();
    if (self->state == Thread::State::Dead) return;
    std::unique_lock<std::mutex> lk(baton_m_);
    self->cv.wait(lk, [&] { return running_ == self || shutdown_ || self->cancelled; });
    if (shutdown_ || self->cancelled || running_ != self) throw ThreadExit{};
    run_started_ns_ = steady_ns(); cycle_started_ = thread_cycles(); wake_ns_ += run_started_ns_ - notified_ns_;
}

void Sched::start_host(Thread* t) {
    t->started = true;
    HANDLE h = CreateThread(nullptr, host_stack_, tramp, new Tramp{this, t}, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("cannot create a host thread for guest thread %d", t->id);
    if (pin_cpu_ >= 0) SetThreadAffinityMask(h, DWORD_PTR(1) << pin_cpu_);
    handles_.push_back(h);
}

void Sched::thread_main(Thread* t) {
    {
        std::unique_lock<std::mutex> lk(baton_m_);
        t->cv.wait(lk, [&] { return running_ == t || shutdown_ || t->cancelled; });
        if (shutdown_ || t->cancelled || running_ != t) return;
        run_started_ns_ = steady_ns(); cycle_started_ = thread_cycles(); wake_ns_ += run_started_ns_ - notified_ns_;
    }
    try {
        t->body(t->ctx);
        if (!shutdown_) finish(t, t->ctx.r[3]);   // returning from the entry function is OSExitThread(r3)
    } catch (const ThreadExit&) {
        return; // Expected cancellation unwinds the host stack; join_hosts publishes final thread states.
    } catch (const std::exception& e) {   // anything else is a runtime bug: report it with the guest trace instead of dying silently
        fatal("uncaught C++ exception in guest thread %d: %s", t->id, e.what());
    } catch (...) {
        fatal("uncaught non-standard exception in guest thread %d", t->id);
    }
}

void Sched::run(Thread* m) {
    {
        std::lock_guard<std::mutex> lk(baton_m_);
        if (shutdown_) return;
        main_ = m;
        m->suspend_count = 0;
        if (switch_hook_) switch_hook_(m);
        m->state = Thread::State::Running;
        running_ = m;
        start_host(m);
    }
    m->cv.notify_one();
    std::unique_lock<std::mutex> lk(done_m_);
    done_cv_.wait(lk, [&] { return main_done_ || shutdown_; });
    lk.unlock();
    shutdown();
    join_hosts();
}

void Sched::shutdown() {
    bool first;
    { std::lock_guard<std::mutex> lk(baton_m_); first = !shutdown_.exchange(true); for (auto& t : threads_) t->cv.notify_all(); }
    if (first && shutdown_hook_) shutdown_hook_();
    { std::lock_guard<std::mutex> lk(ext_m_); ext_q_.clear(); }
    ext_cv_.notify_all();
    { std::lock_guard<std::mutex> lk(done_m_); }
    done_cv_.notify_all();
}

}  // namespace uc
