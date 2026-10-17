#include "cpu_topology.h"
#include <algorithm>
#include <bit>
#include <map>
#include <sstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
namespace uc {
CpuTopology cpu_topology() {
    // shortcut: guest affinity accepts a group-local 0..62 index; add group-aware
    // guest scheduling before calibrating machines with multiple processor groups.
    if(GetActiveProcessorGroupCount()!=1)return {};
    DWORD_PTR allowed=0,system=0;
    if(!GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system))throw std::runtime_error("cannot read CPU affinity");
    ULONG bytes=0;
    GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    if(!bytes)throw std::runtime_error("CPU topology unavailable");
    std::vector<unsigned char> storage(bytes);
    if(!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),bytes,&bytes,GetCurrentProcess(),0))
        throw std::runtime_error("cannot read CPU topology");
    ULONG count=0;
    GetProcessDefaultCpuSets(GetCurrentProcess(),nullptr,0,&count);
    std::vector<ULONG> defaults(count);
    if(count && !GetProcessDefaultCpuSets(GetCurrentProcess(),defaults.data(),count,&count))throw std::runtime_error("cannot read CPU set restrictions");
    std::map<unsigned,CpuCore> cores;
    std::ostringstream identity;
    int registers[4];__cpuid(registers,0);for(int value:registers)identity<<value<<',';
    __cpuid(registers,1);identity<<registers[0]<<',';
    __cpuid(registers,int(0x80000000u));
    if(unsigned(registers[0])>=0x80000004u)for(unsigned leaf=0x80000002u;leaf<=0x80000004u;++leaf) {
        __cpuid(registers,int(leaf));for(int value:registers)identity<<value<<',';
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH+1];DWORD length=MAX_COMPUTERNAME_LENGTH+1;
    if(!GetComputerNameW(computer,&length))throw std::runtime_error("machine identity unavailable");
    for(DWORD i=0;i<length;++i)identity<<unsigned(computer[i])<<',';
    SYSTEM_POWER_STATUS power{};identity<<'/'<<(GetSystemPowerStatus(&power) ? int(power.ACLineStatus) : -1)<<'/'<<allowed;
    for(size_t offset=0;offset<bytes;) {
        const auto* info=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(storage.data()+offset);
        if(bytes-offset<sizeof(*info) || info->Size<sizeof(*info) || info->Size>bytes-offset)throw std::runtime_error("invalid CPU topology record");
        if(info->Type==CpuSetInformation) {
            const auto& cpu=info->CpuSet;
            if(cpu.Group!=0 || cpu.LogicalProcessorIndex>=64)throw std::runtime_error("unsupported CPU group");
            const uint64_t bit=uint64_t(1)<<cpu.LogicalProcessorIndex;
            identity<<'/'<<unsigned(cpu.LogicalProcessorIndex)<<':'<<unsigned(cpu.CoreIndex)<<':'<<unsigned(cpu.LastLevelCacheIndex)<<':'<<unsigned(cpu.EfficiencyClass)<<':'<<unsigned(cpu.SchedulingClass);
            const bool selectable=(allowed&bit) && (!cpu.Allocated || cpu.AllocatedToTargetProcess)
                && (defaults.empty() || std::find(defaults.begin(),defaults.end(),cpu.Id)!=defaults.end());
            identity<<':'<<selectable;
            auto [it,inserted]=cores.try_emplace(cpu.CoreIndex,CpuCore{-1,0,cpu.EfficiencyClass});
            it->second.mask|=bit;
            if(selectable && cpu.LogicalProcessorIndex<63 && (it->second.cpu<0 || cpu.LogicalProcessorIndex<it->second.cpu)) {
                it->second.cpu=cpu.LogicalProcessorIndex;
                it->second.scheduling_class=cpu.SchedulingClass;
            }
        }
        offset+=info->Size;
    }
    CpuTopology result{identity.str(),{}};
    for(const auto& [index,core]:cores)if(core.cpu>=0)result.cores.push_back(core);
    return result;
}
uint64_t cpu_worker_mask(const std::vector<CpuCore>& cores,int cpu,uint64_t allowed) {
    if(cpu<0 || cpu>=63 || !(allowed&(uint64_t(1)<<cpu)))return allowed;
    for(const auto& core:cores)if(core.mask&(uint64_t(1)<<cpu))return allowed&~(core.mask&~(uint64_t(1)<<cpu));
    return allowed;
}
}
