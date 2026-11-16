#include "cpu_topology.h"
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>

// macOS counterpart of cpu_topology_win.cpp. Apple Silicon has no SMT, so every logical CPU is a core of its own; hw.perflevel0 counts the
// performance cores and hw.perflevel1 the efficiency cores. macOS cannot pin threads, so the CPU numbers only order the cores: the performance
// cores come first and get efficiency 1 (higher is faster, as Windows' EfficiencyClass), the efficiency cores 0.
namespace uc {
namespace {
long sysctl_number(const char* name, long fallback) {
    int64_t value = 0;
    size_t size = sizeof value;
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return fallback;
    return size == sizeof(int32_t) ? long(*reinterpret_cast<int32_t*>(&value)) : long(value);
}
std::string sysctl_text(const char* name) {
    char text[256]{};
    size_t size = sizeof text - 1;
    return sysctlbyname(name, text, &size, nullptr, 0) == 0 ? std::string(text) : std::string();
}
}  // namespace
CpuTopology cpu_topology() {
    const long count = sysctl_number("hw.logicalcpu", sysconf(_SC_NPROCESSORS_ONLN));
    if (count < 1) throw std::runtime_error("CPU topology unavailable");
    const long performance = sysctl_number("hw.perflevel0.logicalcpu", count);
    char host[256]{};
    gethostname(host, sizeof host - 1);
    std::ostringstream identity;
    identity << sysctl_text("machdep.cpu.brand_string") << ',' << host << '/' << count << '/' << performance;
    CpuTopology result{identity.str(), {}};
    for (int cpu = 0; cpu < count && cpu < 63; ++cpu)
        result.cores.push_back(CpuCore{cpu, uint64_t(1) << cpu, cpu < performance ? 1u : 0u});
    return result;
}
}  // namespace uc
