#include "cpu_topology.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <filesystem>

// Linux counterpart of cpu_topology_win.cpp: cores from /sys/devices/system/cpu, the allowed set from sched_getaffinity.
// shortcut: every core gets efficiency class 0 (the Steam Deck's CPU is uniform); read cpu_capacity to tell big and little cores apart on hybrid machines.
namespace uc {
namespace {
std::string text_of(const std::filesystem::path& path) {
    std::ifstream in(path);std::string line;
    std::getline(in,line);
    return line;
}
long number_of(const std::filesystem::path& path,long fallback) {
    const auto text=text_of(path);
    return text.empty() ? fallback : std::atol(text.c_str());
}
std::string model_name() {
    std::ifstream in("/proc/cpuinfo");std::string line;
    while(std::getline(in,line))
        if(line.compare(0,10,"model name")==0) {const auto colon=line.find(':');return colon==std::string::npos ? line : line.substr(colon+2);}
    return {};
}
int on_mains_power() {   // 1 on AC, 0 on battery, -1 unknown
    std::error_code ignored;
    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/power_supply",ignored))
        if(text_of(entry.path()/"type")=="Mains")return number_of(entry.path()/"online",-1);
    return -1;
}
}
CpuTopology cpu_topology() {
    cpu_set_t set;
    if(sched_getaffinity(0,sizeof set,&set))throw std::runtime_error("cannot read CPU affinity");
    uint64_t allowed=0;
    for(int cpu=0;cpu<64;++cpu)if(CPU_ISSET(cpu,&set))allowed|=uint64_t(1)<<cpu;
    const long count=sysconf(_SC_NPROCESSORS_CONF);
    if(count<1)throw std::runtime_error("CPU topology unavailable");
    char host[256]{};gethostname(host,sizeof host-1);
    std::ostringstream identity;
    identity<<model_name()<<','<<host<<'/'<<on_mains_power()<<'/'<<allowed;
    std::map<long,CpuCore> cores;   // key: package and core id
    for(int cpu=0;cpu<count && cpu<64;++cpu) {
        const std::filesystem::path topology="/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/topology";
        const long core=number_of(topology/"core_id",cpu),package=number_of(topology/"physical_package_id",0);
        const uint64_t bit=uint64_t(1)<<cpu;
        const bool selectable=(allowed&bit)!=0;
        identity<<'/'<<cpu<<':'<<core<<':'<<package<<':'<<selectable;
        auto [it,inserted]=cores.try_emplace(package*65536+core,CpuCore{-1,0,0});
        it->second.mask|=bit;
        if(selectable && cpu<63 && (it->second.cpu<0 || cpu<it->second.cpu))it->second.cpu=cpu;
    }
    CpuTopology result{identity.str(),{}};
    for(const auto& [index,core]:cores)if(core.cpu>=0)result.cores.push_back(core);
    return result;
}
}
