#include "pass_timer.h"
#include <stdexcept>

namespace uc {
void PassTimer::initialize(VkDevice device, float timestamp_period_ns, uint32_t slots) {
    shutdown();
    device_=device; period_=timestamp_period_ns; slots_=slots; recorded_.assign(slots,{});
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; info.queryType=VK_QUERY_TYPE_TIMESTAMP; info.queryCount=slots*uint32_t(kGpuPassCount)*2;
    if (vkCreateQueryPool(device_,&info,nullptr,&pool_)!=VK_SUCCESS) throw std::runtime_error("pass timer query pool");
}
void PassTimer::reset(VkCommandBuffer command, uint32_t slot) {
    vkCmdResetQueryPool(command,pool_,query(slot,GpuPass(0)),uint32_t(kGpuPassCount)*2);
    recorded_[slot]={};
}
void PassTimer::begin(VkCommandBuffer command, uint32_t slot, GpuPass pass) {
    vkCmdWriteTimestamp2(command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,pool_,query(slot,pass));
}
void PassTimer::end(VkCommandBuffer command, uint32_t slot, GpuPass pass) {
    vkCmdWriteTimestamp2(command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,pool_,query(slot,pass)+1);
    recorded_[slot][size_t(pass)]=true; ++runs_[size_t(pass)];
}
void PassTimer::collect(uint32_t slot) {
    for (size_t pass=0;pass<kGpuPassCount;++pass) {
        if (!recorded_[slot][pass]) continue;
        uint64_t stamps[2];
        if (vkGetQueryPoolResults(device_,pool_,query(slot,GpuPass(pass)),2,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && stamps[1]>=stamps[0])
            last_[pass]=double(stamps[1]-stamps[0])*period_/1e6;
        recorded_[slot][pass]=false;
    }
}
void PassTimer::shutdown() {
    if (pool_) vkDestroyQueryPool(device_,pool_,nullptr);
    pool_=VK_NULL_HANDLE;
}
}
