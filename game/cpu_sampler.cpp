#include "cpu_sampler.h"
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
