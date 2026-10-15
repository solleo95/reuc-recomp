#pragma once
#include "ppc_rt.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uc {

struct ThreadExit {};   // thrown to unwind a guest thread's host stack; carries no guest-visible meaning

struct Thread {
    enum class State { Suspended, Ready, Running, Waiting, Dead };
    int id = 0;
    uint32_t guest_addr = 0;   // OSThread* in guest memory (0 for host-made threads)
    int priority = 16;         // 0 is the most urgent, 31 the least
    State state = State::Suspended;
    int suspend_count = 1;     // OSCreateThread leaves a thread suspended
    uint32_t wait_key = 0;
    uint32_t exit_value = 0;
    int64_t seq = 0;           // FIFO order inside one priority
    bool started = false;
    bool cancelled = false;    // OSCancelThread: the host thread unwinds instead of resuming
    bool ee = true;            // this thread's interrupt-enable state while it is not running (OSContext MSR)
    ppc::Ctx ctx{};
    std::function<void(ppc::Ctx&)> body;
    std::condition_variable cv;
};

class Sched {
public:
    using Body = std::function<void(ppc::Ctx&)>;
    using Event = std::function<void()>;

    explicit Sched(uint8_t* guest_mem, size_t host_stack = 8u << 20);
    ~Sched();
    Sched(const Sched&) = delete;
    Sched& operator=(const Sched&) = delete;

    Thread* create(uint32_t guest_addr, int priority, Body body, const ppc::Ctx* inherit = nullptr);
    int resume(Thread* t);
    int suspend(Thread* t);
    void yield();
    void block_on(uint32_t key);
    Thread* wake_one(uint32_t key);
    int wake_all(uint32_t key);
    void sleep_ticks(uint64_t ticks);
    [[noreturn]] void exit_current(uint32_t value);
    uint32_t join(Thread* t);
    void set_priority(Thread* t, int priority);
    void cancel(Thread* t);                      // OSCancelThread: the thread dies with exit value -1, its joiners wake
    void lock_scheduler() { ++sched_lock_; }     // OSDisableScheduler: no thread switch caused by a wake-up, priority change or resume
    void unlock_scheduler();                     // OSEnableScheduler
    Thread* current() const { return running_; }
    Thread* find(uint32_t guest_addr) const;

    // Events run on the thread that holds the baton, only while interrupts are enabled (the "interrupt context").
    void post_at(uint64_t tick, Event fn);       // baton holder only
    void post_external(Event fn);                // any host thread
    void safe_point();                           // deliver due events, then let a more urgent ready thread run
    bool set_interrupts(bool enabled);           // returns the previous state; enabling is a safe point
    bool interrupts_enabled() const { return ee_; }
    bool in_irq() const { return in_irq_; }
    ppc::Ctx& irq_ctx() { return irq_ctx_; }

    void run(Thread* main_thread);               // returns when the main thread has exited or shutdown() was called
    void shutdown();
    void set_shutdown_hook(Event hook) { shutdown_hook_ = std::move(hook); } // configure before run
    void join_hosts();                            // after shutdown; waits for every guest host stack to unwind
    void set_error_handler(std::function<void(const std::string&)> h) { err_ = std::move(h); }
    void set_deadlock_timeout_ms(int ms) { deadlock_ms_ = ms; }
    const Thread* running() const { return running_; }
    std::string dump_threads() const;           // baton holder or after join_hosts only
    std::string dump_contexts() const;          // crash bundle: every live thread with its registers (threads.txt of the bundle); same restriction
    void set_switch_hook(std::function<void(Thread*)> h) { switch_hook_ = std::move(h); }

    void thread_main(Thread* t);                 // host thread entry (internal)

private:
    static constexpr uint32_t join_key(const Thread* t) { return 0xF0000000u + uint32_t(t->id); }
    static constexpr uint32_t sleep_key(const Thread* t) { return 0xF8000000u + uint32_t(t->id); }
    void make_ready(Thread* t, bool front = false);
    Thread* pick_ready();
    void wake(Thread* t);
    void preempt_check();
    void reschedule(Thread* self);
    void handoff(Thread* self, Thread* next);
    void finish(Thread* self, uint32_t value);
    void start_host(Thread* t);
    void deliver_events();
    void idle_wait();
    [[noreturn]] void fail(const std::string& msg);

    uint8_t* mem_;
    size_t host_stack_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::map<uint32_t, std::vector<Thread*>> waitq_;
    std::multimap<uint64_t, Event> timed_;
    std::vector<Event> ext_q_;
    std::mutex baton_m_, ext_m_, done_m_, join_m_;
    std::condition_variable ext_cv_, done_cv_;
    std::vector<void*> handles_;
    Thread* running_ = nullptr;
    Thread* main_ = nullptr;
    ppc::Ctx irq_ctx_{};
    std::atomic<bool> shutdown_{false}, main_done_{false};
    bool ee_ = true, in_irq_ = false;
    int sched_lock_ = 0;
    int next_id_ = 0, deadlock_ms_ = 5000;
    int64_t front_seq_ = 0, back_seq_ = 0;
    std::function<void(const std::string&)> err_;
    std::function<void(Thread*)> switch_hook_;
    Event shutdown_hook_;
    // --profile: thread switches, how long the woken thread took to start running, and the time each guest thread held the baton.
    std::atomic<uint64_t> switches_{0}, wake_ns_{0}, notified_ns_{0}, run_started_ns_{0};
    std::atomic<uint64_t> run_ns_[32] = {};
    std::atomic<uint32_t> entry_[32] = {};
    static uint64_t steady_ns();
public:
    struct SwitchStats { uint64_t switches, wake_ns; };
    SwitchStats switch_stats() const { return {switches_.load(), wake_ns_.load()}; }
    uint64_t thread_run_ns(int id) const { return id > 0 && id < 32 ? run_ns_[id].load() : 0; }
    uint32_t thread_entry(int id) const { return id > 0 && id < 32 ? entry_[id].load() : 0; }
};

}  // namespace uc
