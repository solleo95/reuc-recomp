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
CpuTopology cpu_topology();
uint64_t cpu_worker_mask(const std::vector<CpuCore>& cores,int cpu,uint64_t allowed);
}
