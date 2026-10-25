#include "sched.h"
#include "clock.h"
#include "crash_report.h"
#include "fatal.h"
#include "dispatch.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
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

PendingHle::PendingHle(Sched& scheduler,ppc::Ctx& c,void (*retry)(ppc::Ctx&),uint32_t entry)
    : thread_(scheduler.current()) {
    if(!thread_ || scheduler.in_irq() || &thread_->ctx!=&c) { thread_=nullptr; return; }
    previous_=thread_->continuation;
    thread_->continuation={};
    thread_->continuation.pc=entry ? entry : c.lr;
    thread_->continuation.return_lr=c.lr;
    thread_->continuation.retry=retry;
    thread_->continuation.entry_ee=scheduler.interrupts_enabled();
    std::copy_n(c.r+3,8,thread_->continuation.args.begin());
}
PendingHle::~PendingHle() { if(thread_) thread_->continuation=previous_; }

struct Sched::Snapshot {
    struct SavedThread {
        Thread* thread;
        int id;
        uint32_t guest_addr,entry;
        int priority,suspend_count;
        Thread::State state;
        uint32_t wait_key,exit_value;
        int64_t seq;
        bool cancelled,ee,entered;
        ppc::Ctx ctx;
        Thread::Continuation continuation;
    };
    const Sched* owner;
    Thread* main;
    std::vector<SavedThread> threads;
    std::map<uint32_t,std::vector<Thread*>> waitq;
    std::multimap<uint64_t,EventRecord> timed;
    std::vector<EventRecord> external;
    ppc::Ctx irq;
    uint64_t ticks;
    bool ee,external_allowed;
    int scheduler_lock,next_id;
    int64_t front_seq,back_seq;
};

std::shared_ptr<const Sched::Snapshot> Sched::checkpoint() {
    if(!det_ || !is_guest_thread() || running_!=main_ || in_irq_ || shutdown_)
        throw std::logic_error("scheduler checkpoint requires deterministic main-thread boundary");
    auto out=std::make_shared<Snapshot>();
    std::lock_guard external(ext_m_);
    if(async_pending_!=0) throw std::logic_error("checkpoint has in-flight external I/O");
    for(const auto& [tick,event]:timed_)
        if(event.kind==EventKind::Opaque) throw std::logic_error("checkpoint contains an unregistered timer");
    for(const auto& event:ext_q_)
        if(event.kind==EventKind::Opaque) throw std::logic_error("checkpoint contains an unregistered external event");
    out->owner=this; out->main=main_;
    for(const auto& ptr:threads_) {
        const auto& t=*ptr;
        if(&t!=main_ && t.entered && t.state!=Thread::State::Dead &&
           (!t.continuation.pc || !has_function(t.continuation.pc) || t.ctx.native_depth>1))
        {
            char detail[160];
            std::snprintf(detail,sizeof(detail),"thread %d has no restorable continuation: pc=%08X lr=%08X native_depth=%u mapped=%d",
                          t.id,t.continuation.pc,t.ctx.lr,t.ctx.native_depth,int(has_function(t.continuation.pc)));
            throw std::logic_error(detail);
        }
        out->threads.push_back({ptr.get(),t.id,t.guest_addr,thread_entry(t.id),t.priority,t.suspend_count,t.state,t.wait_key,t.exit_value,
                               t.seq,t.cancelled,t.ee,t.entered,t.ctx,t.continuation});
    }
    out->waitq=waitq_; out->timed=timed_; out->external=ext_q_; out->irq=irq_ctx_;
    out->ticks=det_clock_checkpoint(); out->ee=ee_; out->external_allowed=ext_allowed_;
    out->scheduler_lock=sched_lock_; out->next_id=next_id_;
    out->front_seq=front_seq_; out->back_seq=back_seq_;
    return out;
}

uint64_t Sched::checkpoint_hash(const Snapshot& saved) {
    uint64_t hash=14695981039346656037ull;
    const auto add=[&](uint64_t value) {
        for(unsigned byte=0;byte<8;++byte) { hash^=uint8_t(value); hash*=1099511628211ull; value>>=8; }
    };
    const auto context=[&](const ppc::Ctx& c) {
        for(auto value:c.r) add(value);
        for(auto value:c.f) add(std::bit_cast<uint64_t>(value));
        for(auto value:c.ps1) add(std::bit_cast<uint64_t>(value));
        for(auto value:c.cr) add(value);
        add(c.xer_ca); add(c.xer_ov); add(c.xer_so);
        add(c.lr); add(c.ctr); add(c.msr); add(c.fpscr);
        for(auto value:c.gqr) add(value);
        for(auto value:c.spr) add(value);
        add(c.reserve_addr); add(c.poll_count);
        // native_depth counts host scopes; restore rebuilds worker scopes from zero.
    };
    const auto thread_id=[&](const Thread* thread) -> uint64_t {
        if(!thread) return 0;
        const auto found=std::find_if(saved.threads.begin(),saved.threads.end(),[&](const auto& t) { return t.thread==thread; });
        if(found==saved.threads.end()) throw std::logic_error("snapshot references an unknown thread");
        return uint64_t(found->id);
    };
    add(saved.threads.size());
    for(const auto& t:saved.threads) {
        add(t.id); add(t.guest_addr); add(t.entry);
        add(uint32_t(t.state)); add(t.priority); add(t.suspend_count);
        add(t.wait_key); add(t.exit_value); add(t.seq);
        add(t.cancelled); add(t.ee); add(t.entered);
        context(t.ctx);
        add(t.continuation.pc); add(t.continuation.return_lr);
        for(auto value:t.continuation.args) add(value);
        add(t.continuation.entry_ee); add(t.continuation.retry!=nullptr);
    }
    add(thread_id(saved.main)); context(saved.irq);
    add(saved.ticks); add(saved.ee); add(saved.external_allowed);
    add(saved.scheduler_lock); add(saved.next_id); add(saved.front_seq); add(saved.back_seq);
    add(saved.waitq.size());
    for(const auto& [key,queue]:saved.waitq) {
        add(key); add(queue.size());
        for(const auto* thread:queue) add(thread_id(thread));
    }
    // shortcut: event/body captures are opaque; add typed payload records when complete callback-state hashing is required.
    add(saved.timed.size());
    for(const auto& [tick,event]:saved.timed) { add(tick); add(uint32_t(event.kind)); }
    add(saved.external.size());
    for(const auto& event:saved.external) add(uint32_t(event.kind));
    return hash;
}

void Sched::restore(const Snapshot& saved) {
    if(!det_ || !is_guest_thread() || running_!=main_ || in_irq_ || shutdown_ ||
       saved.owner!=this || saved.main!=main_ || saved.threads.size()>threads_.size())
        throw std::logic_error("invalid scheduler restore boundary or checkpoint");
    for(size_t i=0;i<saved.threads.size();++i)
        if(saved.threads[i].thread!=threads_[i].get()) throw std::logic_error("checkpoint belongs to discarded thread history");
    // Destructors finish on every old host stack before any snapshot state changes.
    std::unique_lock baton(baton_m_);
    for(auto& t:threads_) if(t.get()!=main_ && t->host_alive) {
        t->rewind_requested=true; t->cv.notify_all();
    }
    if(!rewind_cv_.wait_for(baton,std::chrono::seconds(5),[&] {
        return shutdown_ || std::all_of(threads_.begin(),threads_.end(),[&](const auto& t) {
            return t.get()==main_ || !t->host_alive || t->rewound;
        });
    })) throw std::runtime_error("guest continuation failed to unwind for rollback");
    if(shutdown_) throw ThreadExit{};
    {
        std::lock_guard external(ext_m_);
        ++generation_; async_pending_=0;
        ext_q_=saved.external;
    }
    while(threads_.size()>saved.threads.size()) {
        auto t=std::move(threads_.back()); threads_.pop_back();
        t->cancelled=true; t->rewind_requested=false; t->cv.notify_all();
        retired_.push_back(std::move(t));
    }
    for(const auto& old:saved.threads) {
        auto& t=*old.thread;
        t.priority=old.priority; t.suspend_count=old.suspend_count; t.state=old.state;
        t.wait_key=old.wait_key; t.exit_value=old.exit_value; t.seq=old.seq;
        t.cancelled=old.cancelled || old.state==Thread::State::Dead;
        t.ee=old.ee; t.entered=old.entered; t.ctx=old.ctx; t.continuation=old.continuation;
        if(&t!=main_) t.ctx.native_depth=0;
        t.restart=old.entered && &t!=main_; t.rewind_requested=false;
        if(!t.host_alive) t.started=false;
        if(t.cancelled) t.cv.notify_all();
    }
    waitq_=saved.waitq; timed_=saved.timed; irq_ctx_=saved.irq;
    ee_=saved.ee; ext_allowed_=saved.external_allowed; sched_lock_=saved.scheduler_lock;
    next_id_=saved.next_id; front_seq_=saved.front_seq; back_seq_=saved.back_seq;
    det_clock_restore(saved.ticks);
    if(switch_hook_) switch_hook_(main_);
    run_started_ns_=steady_ns(); cycle_started_=thread_cycles();
}

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
    t->ctx.native_depth = 0; // Host call depth belongs to this stack, never to the creating thread.
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
    // Finish a native operation before parking a worker at a resumable SDK boundary.
    if(rollback_ && running_!=main_ && (!running_->continuation.pc || running_->ctx.native_depth>1)) return;
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
    post_at(now_ticks() + ticks, [this, key] { wake_all(key); },EventKind::Sleep);
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

void Sched::post_at(uint64_t tick, Event fn,EventKind kind) { timed_.emplace(tick,EventRecord{std::move(fn),kind}); }

void Sched::drain_async() {
    std::unique_lock lock(ext_m_);
    if(!ext_cv_.wait_for(lock,std::chrono::seconds(5),[&] { return shutdown_ || async_pending_==0; }))
        throw std::runtime_error("checkpoint timed out waiting for external I/O");
    if(shutdown_) throw ThreadExit{};
}

void Sched::post_external(Event fn,EventKind kind,std::optional<uint64_t> generation) {
    if (generation && *generation!=generation_.load()) return;
    if (det_ && is_guest_thread()) { post_at(0,std::move(fn),kind); return; }
    {
        std::lock_guard<std::mutex> lk(ext_m_);
        if (shutdown_) return;
        if (generation && *generation!=generation_.load()) return;
        if (det_ && async_pending_ > 0) --async_pending_;   // the announced result of another thread has arrived
        ext_q_.push_back({std::move(fn),kind});
    }
    ext_cv_.notify_all();
}

void Sched::deliver_events() {
    if (in_irq_ || !ee_) return;
    std::vector<EventRecord> ext;
    if (ext_allowed_) { std::lock_guard<std::mutex> lk(ext_m_); ext.swap(ext_q_); }
    for (auto& e : ext) timed_.emplace(0, std::move(e));
    while (!timed_.empty() && timed_.begin()->first <= now_ticks()) {
        if (shutdown_) throw ThreadExit{};
        Event e = std::move(timed_.begin()->second.fn);
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
    self->cv.wait(lk, [&] { return running_ == self || shutdown_ || self->cancelled || self->rewind_requested; });
    if(self->rewind_requested && !shutdown_) throw ThreadRewind{};
    if (shutdown_ || self->cancelled || running_ != self) throw ThreadExit{};
    run_started_ns_ = steady_ns(); cycle_started_ = thread_cycles(); wake_ns_ += run_started_ns_ - notified_ns_;
}

void Sched::start_host(Thread* t) {
    t->started = true;
    t->host_alive = true;
    HANDLE h = CreateThread(nullptr, host_stack_, tramp, new Tramp{this, t}, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("cannot create a host thread for guest thread %d", t->id);
    if (pin_cpu_ >= 0) SetThreadAffinityMask(h, DWORD_PTR(1) << pin_cpu_);
    handles_.push_back(h);
}

void Sched::thread_main(Thread* t) {
    struct HostExit {
        Sched& s; Thread& t;
        ~HostExit() { std::lock_guard lock(s.baton_m_); t.host_alive=false; s.rewind_cv_.notify_all(); }
    } host_exit{*this,*t};
    for(;;) {
        {
            std::unique_lock<std::mutex> lk(baton_m_);
            t->cv.wait(lk, [&] { return (running_==t && !t->rewind_requested) || shutdown_ || t->cancelled; });
            if(shutdown_ || t->cancelled) return;
            t->rewound=false;
            run_started_ns_=steady_ns(); cycle_started_=thread_cycles(); wake_ns_+=run_started_ns_-notified_ns_;
        }
        try {
            if(t->restart) {
                const auto pending=t->continuation;
                  t->restart=false; t->continuation={}; t->ctx.lr=pending.return_lr;
                if(pending.retry) {
                    std::copy(pending.args.begin(),pending.args.end(),t->ctx.r+3);
                      ee_=pending.entry_ee;
                      struct NativeScope {
                          ppc::Ctx& c;
                          explicit NativeScope(ppc::Ctx& c):c(c) { ++c.native_depth; }
                          ~NativeScope() { --c.native_depth; }
                      } native(t->ctx);
                    pending.retry(t->ctx);
                }
                for(uint32_t pc=pending.pc;pc;pc=t->ctx.lr) call_indirect(t->ctx,pc);
            } else { t->entered=true; t->body(t->ctx); }
            if(!shutdown_) finish(t,t->ctx.r[3]);
            return;
        } catch(const ThreadRewind&) {
            std::lock_guard lock(baton_m_);
            t->rewound=true; rewind_cv_.notify_all();
        } catch(const ThreadExit&) { return; }
        catch(const std::exception& e) { fatal("uncaught C++ exception in guest thread %d: %s",t->id,e.what()); }
        catch(...) { fatal("uncaught non-standard exception in guest thread %d",t->id); }
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
    { std::lock_guard<std::mutex> lk(baton_m_); first = !shutdown_.exchange(true); for (auto& t : threads_) t->cv.notify_all(); for(auto& t:retired_)t->cv.notify_all(); rewind_cv_.notify_all(); }
    if (first && shutdown_hook_) shutdown_hook_();
    { std::lock_guard<std::mutex> lk(ext_m_); ext_q_.clear(); }
    ext_cv_.notify_all();
    { std::lock_guard<std::mutex> lk(done_m_); }
    done_cv_.notify_all();
}

}  // namespace uc
