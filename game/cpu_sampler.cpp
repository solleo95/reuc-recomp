#include "cpu_sampler.h"
#if defined(__APPLE__) || defined(__ANDROID__)
// macOS and Android: no sampler (it reads x86-64 registers from Linux's per-thread SIGEV_THREAD_ID timers); profile with Instruments or simpleperf instead.
namespace uc {
void cpu_sampler_start_once(const char*) {}
void cpu_sampler_set_replaying(bool) {}
}
#elif !defined(_WIN32)
// Linux: a flat profile of the whole process. UC_CPU_SAMPLE=1 starts a SIGPROF timer (every millisecond of process CPU time) that records the interrupted instruction and thread
// into a ring; at exit the last UC_CPU_SAMPLE_SECONDS (default 25) go to UC_CPU_SAMPLE_OUT (default cpu-samples.txt): a "# base <hex>" line (the executable's load bias), then
// "<tid> <rip> <ms>". scripts/resolve_linux_samples.py names the functions with llvm-symbolizer. (The Windows build samples one thread with stack unwinding, below.)
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <link.h>
#include <string_view>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
namespace uc {
namespace {
struct Sample { uint64_t rip; uint32_t tid; uint32_t ms; };
constexpr size_t kRing = 1 << 18;
Sample ring[kRing];
std::atomic<uint64_t> taken{0};
uint32_t now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint32_t(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
// UC_CPU_SAMPLE_STACKS=1: every sample also keeps up to kStack return addresses (the callers of the interrupted function), parallel to `ring` (untouched pages cost nothing when off).
constexpr int kStack = 12;
struct StackSample { uint64_t frames[kStack]; uint8_t depth; };
StackSample stack_ring[kRing];
bool want_stacks = false;
uintptr_t exe_low = 0, exe_high = 0;   // the executable's code segment (set when stacks or the wait mode are on)
void find_exe_text() {
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {   // the first object is the executable: its executable segment
        for (int i = 0; i < info->dlpi_phnum; ++i) if (info->dlpi_phdr[i].p_type == PT_LOAD && (info->dlpi_phdr[i].p_flags & PF_X)) { exe_low = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr; exe_high = exe_low + info->dlpi_phdr[i].p_memsz; }
        return 1; }, nullptr);
}
void on_profile(int, siginfo_t*, void* context) {
    const auto* c = static_cast<ucontext_t*>(context);
    const uint64_t i = taken.fetch_add(1, std::memory_order_relaxed);
    const uint64_t rip = uint64_t(c->uc_mcontext.gregs[REG_RIP]);
    ring[i & (kRing - 1)] = {rip, uint32_t(syscall(SYS_gettid)), now_ms()};
    if (!want_stacks) return;
    StackSample& s = stack_ring[i & (kRing - 1)];
    s.depth = 0;
    // backtrace() is not async-signal-safe (libgcc's unwinder and dl_iterate_phdr keep caches and locks): it runs only for a sample taken in our own code, where the interrupted
    // thread cannot be inside them. A sample inside a library keeps its address and at most one caller: the word at the stack pointer when it is a return address into the
    // executable, which names the caller of a frameless leaf (memcpy, memcmp; for other library functions the word is usually not an executable address and is dropped).
    if (rip < exe_low || rip >= exe_high) {
        const uint64_t top = *reinterpret_cast<const uint64_t*>(c->uc_mcontext.gregs[REG_RSP]);
        if (top >= exe_low && top < exe_high) s.frames[s.depth++] = top;
        return;
    }
    void* frames[kStack + 5];
    const int n = backtrace(frames, kStack + 5);
    int k = 0;
    while (k < n && k < 5 && uint64_t(uintptr_t(frames[k])) != rip) ++k;   // the handler and the signal trampoline come first, then the interrupted pc
    if (k < n && k < 5) for (int j = k + 1; j < n && s.depth < kStack; ++j) s.frames[s.depth++] = uint64_t(uintptr_t(frames[j]));
}
// UC_CPU_SAMPLE=wait: the guest thread sampled by wall-clock time (every 2 ms), blocked or not: the top frame (named at exit with dladdr when it is outside the executable: a futex
// wait, a memcpy) and the first frame inside the executable (what called it).
struct WaitSample { uint64_t top, caller; uint32_t ms; };
constexpr size_t kWaitRing = 1 << 16;
WaitSample wait_ring[kWaitRing];
std::atomic<uint64_t> wait_taken{0};
void on_wait(int, siginfo_t*, void* context) {
    void* frames[24];
    const int n = backtrace(frames, 24);
    const auto top = uint64_t(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    uint64_t caller = 0;
    for (int i = 1; i < n && !caller; ++i) if (uintptr_t(frames[i]) >= exe_low && uintptr_t(frames[i]) < exe_high) caller = uint64_t(uintptr_t(frames[i]));
    wait_ring[wait_taken.fetch_add(1, std::memory_order_relaxed) & (kWaitRing - 1)] = {top, caller, now_ms()};
}
void write_waits(std::FILE* file) {
    const uint64_t total = wait_taken.load(), count = total < kWaitRing ? total : kWaitRing;
    for (uint64_t i = total - count; i < total; ++i) {
        const WaitSample& s = wait_ring[i & (kWaitRing - 1)];
        const bool inside = s.top >= exe_low && s.top < exe_high;
        Dl_info info {};
        const char* name = inside ? "exe" : (dladdr(reinterpret_cast<void*>(s.top), &info) && info.dli_sname ? info.dli_sname : "lib?");
        std::fprintf(file, "w %s %lx %u\n", name, (unsigned long)s.caller, s.ms);
    }
}
void write_samples() {
    struct itimerval off {}; setitimer(ITIMER_PROF, &off, nullptr);
    const char* out = std::getenv("UC_CPU_SAMPLE_OUT");
    const char* seconds = std::getenv("UC_CPU_SAMPLE_SECONDS");
    std::FILE* file = std::fopen(out ? out : "cpu-samples.txt", "w");
    if (!file) return;
    uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) { *static_cast<uintptr_t*>(data) = info->dlpi_addr; return 1; }, &base);   // the first object is the executable
    std::fprintf(file, "# base %lx\n", (unsigned long)base);
    const uint64_t total = taken.load(), count = total < kRing ? total : kRing;
    if (total) {
        const uint32_t last = ring[(total - 1) & (kRing - 1)].ms, keep = uint32_t(1000 * (seconds ? std::atoi(seconds) : 25));
        for (uint64_t i = total - count; i < total; ++i) {
            const Sample& s = ring[i & (kRing - 1)];
            if (last - s.ms > keep) continue;
            Dl_info info {};   // outside the executable (libc, the driver): named here, the symbolizer only knows the executable
            if (dladdr(reinterpret_cast<void*>(s.rip), &info) && info.dli_fbase != reinterpret_cast<void*>(base) && info.dli_sname) std::fprintf(file, "x %u %s %u\n", s.tid, info.dli_sname, s.ms);
            else if (info.dli_fbase && info.dli_fbase != reinterpret_cast<void*>(base) && info.dli_fname) std::fprintf(file, "x %u %s %u\n", s.tid, std::strrchr(info.dli_fname, '/') ? std::strrchr(info.dli_fname, '/') + 1 : info.dli_fname, s.ms);
            else std::fprintf(file, "%u %lx %u\n", s.tid, (unsigned long)s.rip, s.ms);
            if (want_stacks) {   // s <tid> <ms> <rip> <return addresses...> (scripts/inclusive_profile.py)
                const StackSample& st = stack_ring[i & (kRing - 1)];
                std::fprintf(file, "s %u %u %lx", s.tid, s.ms, (unsigned long)s.rip);
                for (int j = 0; j < st.depth; ++j) std::fprintf(file, " %lx", (unsigned long)st.frames[j]);
                std::fputc('\n', file);
            }
        }
    }
    write_waits(file);
    std::fclose(file);
}
}
void cpu_sampler_start_once(const char* thread) {
    static bool started = false, waiting = false;
    const char* mode = std::getenv("UC_CPU_SAMPLE");
    if (!mode) return;
    if (std::string_view(mode) == "wait") {   // the guest thread by wall-clock time
        if (waiting || std::string_view(thread) != "guest") return;
        waiting = true;
        void* warm[2]; backtrace(warm, 2);   // loads libgcc's unwinder now: it must not happen inside the signal handler
        find_exe_text();
        struct sigaction action {}; action.sa_sigaction = on_wait; action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, nullptr);
        sigevent event {}; event.sigev_notify = SIGEV_THREAD_ID; event.sigev_signo = SIGPROF; event._sigev_un._tid = int(syscall(SYS_gettid));
        timer_t timer; timer_create(CLOCK_MONOTONIC, &event, &timer);
        itimerspec spec {}; spec.it_interval.tv_nsec = 2000000; spec.it_value.tv_nsec = 2000000;
        timer_settime(timer, 0, &spec, nullptr);
        std::atexit(write_samples);
        return;
    }
    if (started || std::string_view(thread) != "main") return;
    started = true;
    if (const char* stacks = std::getenv("UC_CPU_SAMPLE_STACKS"); stacks && stacks[0] == '1') {
        void* warm[2]; backtrace(warm, 2);   // loads libgcc's unwinder now: it must not happen inside the signal handler
        find_exe_text();
        want_stacks = true;
    }
    struct sigaction action {}; action.sa_sigaction = on_profile; action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &action, nullptr);
    struct itimerval timer {}; timer.it_interval.tv_usec = 1000; timer.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &timer, nullptr);
    std::atexit(write_samples);
}
void cpu_sampler_set_replaying(bool) {}
}
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

namespace uc {
namespace {
constexpr int kDepth = 10;
struct Stack { uint32_t frame[kDepth]; uint32_t count; };
// Open-addressing table of distinct stacks, sized up front: the target may hold the heap lock while it is suspended.
constexpr size_t kSlots = 1u << 17;
std::atomic<bool> replaying{false};
}  // namespace

void cpu_sampler_set_replaying(bool value) { replaying.store(value,std::memory_order_relaxed); }

void cpu_sampler_start_once(const char* thread) {
    const char* want = std::getenv("UC_CPU_SAMPLE");
    if (want && want[0] == '1') want = "guest";
    static bool started = false;
    if (!want || started || std::string_view(want) != thread) return;
    started = true;
    HANDLE target = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    std::thread([target] {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::vector<Stack> table(kSlots);
        auto last = std::chrono::steady_clock::now();
        const char* delay = std::getenv("UC_CPU_SAMPLE_DELAY");   // seconds to skip first (boot, menus)
        const auto begin = last + std::chrono::seconds(delay ? std::atoi(delay) : 0);
        const char* flag = std::getenv("UC_CPU_SAMPLE_FLAG");   // sample only once this file exists (a test scenario creates it at the scene of interest)
        const bool only_replay=std::getenv("UC_CPU_SAMPLE_REPLAY")!=nullptr;
        bool armed = !flag;
        for (unsigned tick = 0;; ++tick) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            if (std::chrono::steady_clock::now() < begin) continue;
            if (!armed) { if (tick % 100 == 0) armed = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES; if (!armed) continue; }
            if(only_replay && !replaying.load(std::memory_order_relaxed))continue;
            if (SuspendThread(target) == DWORD(-1)) continue;
            // Recheck after suspension so the sample cannot slip into forward simulation.
            if(only_replay && !replaying.load(std::memory_order_relaxed)) { ResumeThread(target);continue; }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            Stack stack{};
            if (GetThreadContext(target, &context)) {
                int depth = 0;
                for (; depth < kDepth && context.Rip; ++depth) {
                    stack.frame[depth] = uint32_t(context.Rip - base);   // outside the image (ntdll, drivers) becomes a garbage rva, filtered by the resolver
                    DWORD64 image = 0;
                    if (auto* entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr)) {
                        void* handler = nullptr; DWORD64 frame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &handler, &frame, nullptr);
                    } else break;   // leaf without unwind data
                }
                stack.count = uint32_t(depth);
            }
            ResumeThread(target);
            if (!stack.count) continue;
            uint64_t hash = 1469598103934665603ull;
            for (int i = 0; i < kDepth; ++i) hash = (hash ^ stack.frame[i]) * 1099511628211ull;
            for (size_t probe = 0, slot = hash & (kSlots - 1); probe < 64; ++probe, slot = (slot + 1) & (kSlots - 1)) {
                Stack& entry = table[slot];
                if (!entry.count) { entry = stack; entry.count = 1 | (stack.count << 24); break; }
                if (std::equal(entry.frame, entry.frame + kDepth, stack.frame) && (entry.count >> 24) == stack.count) { ++entry.count; break; }
            }
            if (std::chrono::steady_clock::now() - last >= std::chrono::seconds(1)) {
                last = std::chrono::steady_clock::now();
                if (FILE* file = std::fopen("cpu-samples.txt", "w")) {
                    for (const Stack& entry : table) {
                        if (!entry.count) continue;
                        for (uint32_t i = 0; i < (entry.count >> 24); ++i) std::fprintf(file, "%x ", entry.frame[i]);
                        std::fprintf(file, "= %u\n", entry.count & 0xFFFFFF);
                    }
                    std::fclose(file);
                }
            }
        }
    }).detach();
}
}  // namespace uc
#endif
