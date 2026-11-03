#include "perf_counters.h"
#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/perf_event.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
namespace uc {
namespace {
struct Group { int instructions = -1, cycles = -1; };
std::mutex lock;
std::vector<Group> groups;
bool want() { static const bool on = [] { const char* v = std::getenv("UC_PERF_COUNTERS"); return v && *v && *v != '0'; }(); return on; }
int open_counter(uint64_t config, int group) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE; attr.size = sizeof attr; attr.config = config;
    attr.exclude_kernel = 1; attr.exclude_hv = 1; attr.disabled = group < 0;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return int(syscall(SYS_perf_event_open, &attr, 0, -1, group, 0));
}
double read_scaled(int fd) {   // multiplexing would make the count a share of the time: scale it up (two counters on a core that has more never multiplex)
    uint64_t v[3] = {0, 0, 0};
    if (fd < 0 || read(fd, v, sizeof v) != sizeof v) return 0;
    return v[2] && v[2] < v[1] ? double(v[0]) * double(v[1]) / double(v[2]) : double(v[0]);
}
}  // namespace
bool perf_counters_enabled() { return want(); }
void perf_register_thread() {
    if (!want()) return;
    Group g;
    g.instructions = open_counter(PERF_COUNT_HW_INSTRUCTIONS, -1);
    if (g.instructions < 0) { static bool reported = false; if (!reported) { reported = true; std::fprintf(stderr, "[perf] perf_event_open failed (%s): no hardware counters\n", std::strerror(errno)); } return; }
    g.cycles = open_counter(PERF_COUNT_HW_CPU_CYCLES, g.instructions);
    ioctl(g.instructions, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP); ioctl(g.instructions, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    std::lock_guard guard(lock);
    groups.push_back(g);
}
PerfTotals perf_totals() {
    PerfTotals t; t.enabled = want();
    if (!t.enabled) return t;
    std::lock_guard guard(lock);
    double instructions = 0, cycles = 0;
    for (const Group& g : groups) { instructions += read_scaled(g.instructions); cycles += read_scaled(g.cycles); }
    t.instructions = uint64_t(instructions); t.cycles = uint64_t(cycles);
    return t;
}
}
#elif defined(_WIN32)
// Windows: the cycles of the guest threads (QueryThreadCycleTime: the cycles the thread ran, whatever the clock speed). There is no user-space instruction counter
// without the performance-monitoring ETW session; docs/testing/performance-measurement.md says how the instruction counts are taken there.
#include <windows.h>
#include <cstdlib>
#include <mutex>
#include <vector>
namespace uc {
namespace {
std::mutex lock;
std::vector<HANDLE> threads;
bool want() { static const bool on = [] { const char* v = std::getenv("UC_PERF_COUNTERS"); return v && *v && *v != '0'; }(); return on; }
}  // namespace
bool perf_counters_enabled() { return want(); }
void perf_register_thread() {
    if (!want()) return;
    HANDLE real = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) return;
    std::lock_guard guard(lock);
    threads.push_back(real);
}
PerfTotals perf_totals() {
    PerfTotals t; t.enabled = want();
    if (!t.enabled) return t;
    std::lock_guard guard(lock);
    for (HANDLE h : threads) { ULONG64 cycles = 0; if (QueryThreadCycleTime(h, &cycles)) t.cycles += cycles; }
    return t;
}
}
#else
namespace uc {
bool perf_counters_enabled() { return false; }
void perf_register_thread() {}
PerfTotals perf_totals() { return {}; }
}
#endif
