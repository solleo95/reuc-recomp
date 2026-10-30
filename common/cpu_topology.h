#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace uc {
struct CpuCore {
    int cpu;
    uint64_t mask;
    unsigned efficiency;
    unsigned scheduling_class=0;
};
struct CpuTopology {
    std::string fingerprint;
    std::vector<CpuCore> cores;
};
CpuTopology cpu_topology();   // cpu_topology_win.cpp / cpu_topology_posix.cpp
inline uint64_t cpu_worker_mask(const std::vector<CpuCore>& cores,int cpu,uint64_t allowed) {
    if(cpu<0 || cpu>=63 || !(allowed&(uint64_t(1)<<cpu)))return allowed;
    for(const auto& core:cores)if(core.mask&(uint64_t(1)<<cpu))return allowed&~(core.mask&~(uint64_t(1)<<cpu));
    return allowed;
}
}
