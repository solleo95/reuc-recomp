// Linux CPU topology: one entry per usable physical core, its hardware threads in the mask, a stable machine fingerprint.
#ifndef __linux__
int main() { return 0; }
#else
#include "check.h"
#include "cpu_topology.h"
#include <cstdint>
#include <sched.h>

int main() {
    const auto topology = uc::cpu_topology();
    CHECK(!topology.fingerprint.empty());
    CHECK(topology.fingerprint == uc::cpu_topology().fingerprint);   // the same machine state gives the same fingerprint
    CHECK(!topology.cores.empty());
    cpu_set_t set;
    CHECK(sched_getaffinity(0, sizeof set, &set) == 0);
    uint64_t allowed = 0;
    for (int cpu = 0; cpu < 64; ++cpu) if (CPU_ISSET(cpu, &set)) allowed |= uint64_t(1) << cpu;
    for (const auto& core : topology.cores) {
        CHECK(core.cpu >= 0 && core.cpu < 63);
        CHECK(core.mask & (uint64_t(1) << core.cpu));      // the chosen thread belongs to its core
        CHECK(allowed & (uint64_t(1) << core.cpu));        // and may run here
        // a worker pinned to this thread leaves the core's other threads to someone else
        const uint64_t mask = uc::cpu_worker_mask(topology.cores, core.cpu, allowed);
        CHECK(mask & (uint64_t(1) << core.cpu));
        CHECK((mask & core.mask) == (uint64_t(1) << core.cpu));
    }
    return 0;
}
#endif
