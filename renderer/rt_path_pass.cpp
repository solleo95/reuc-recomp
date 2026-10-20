#include "rt_path_pass.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("rt path: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/rt_path.slang Params (push constants)
struct PushParams {
    float viewport[4], view_a[4], view_b[2], depth_range[2];
    uint32_t region[2], signal_size[2];
    uint32_t divisor_frame, counts; float ao_radius, ao_strength;   // rt_path_math.h rt_path_pack
    float reach, emission, ambient, key;
    float overhead[4];
    float mix, threshold;
    uint32_t record_count; float clamp_radiance;
};
static_assert(sizeof(PushParams)==128 && offsetof(PushParams,region)==48 && offsetof(PushParams,divisor_frame)==64 && offsetof(PushParams,reach)==80 && offsetof(PushParams,overhead)==96 && offsetof(PushParams,mix)==112);
constexpr VkFormat kSignalFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBindings=12, kPasses=3;
VkShaderModule load_module(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
    if (code.empty() || code.size()%4) throw std::runtime_error("rt path: shader "+path.string()+" missing or invalid");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size(); info.pCode=reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module=VK_NULL_HANDLE; check(vkCreateShaderModule(device,&info,nullptr,&module),"shader module");
    return module;
}
void memory_barrier(VkCommandBuffer command, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=src_stage; barrier.srcAccessMask=src_access; barrier.dstStageMask=dst_stage; barrier.dstAccessMask=dst_access;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits) {
    for (uint32_t i=0;i<memory.memoryTypeCount;++i) if ((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    throw std::runtime_error("rt path: required memory type unavailable");
}
}
void RtPathPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle) {
    if (device_) return;
    if (!slots || slots>trace_sets_.size()) throw std::invalid_argument("rt path: unsupported frame slot count");
    device_=device; memory_=memory; slots_=slots;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        const char* names[kPasses]={"accumulate","trace","composite"};
        for (uint32_t i=0;i<kPasses;++i) modules_[i]=load_module(device_,shaders/(std::string("rt_path.")+names[i]+".spv"));
        const VkDescriptorType types[kBindings]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (uint32_t i=0;i<kBindings;++i) bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=kBindings; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushParams)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (uint32_t i=0;i<kPasses;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            const std::string entry=std::string(names[i])+"Main";
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],entry.c_str(),nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const uint32_t sets=2*slots;
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,sets},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,5*sets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*sets},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*sets}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=sets; pool.poolSizeCount=4; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        const std::vector<VkDescriptorSetLayout> layouts(sets,set_layout_);
        std::vector<VkDescriptorSet> allocated(sets);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=sets; allocate.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device_,&allocate,allocated.data()),"descriptor sets");
        for (uint32_t i=0;i<slots;++i) { trace_sets_[i]=allocated[2*i]; composite_sets_[i]=allocated[2*i+1]; }
    } catch (...) { shutdown(); throw; }
}
void RtPathPass::ensure_signal(VkExtent2D extent) {
    if (signal_.image && extent.width==signal_extent_.width && extent.height==signal_extent_.height) return;
    if (signal_.image) {
        idle_();   // frames in flight may still read or write the image and name its view in their sets
        vkDestroyImageView(device_,signal_.view,nullptr); vkDestroyImage(device_,signal_.image,nullptr); vkFreeMemory(device_,signal_.memory,nullptr);
        signal_={};
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=kSignalFormat;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&signal_.image),"signal image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,signal_.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=memory_type(memory_,requirements.memoryTypeBits);
    check(vkAllocateMemory(device_,&allocate,nullptr,&signal_.memory),"signal memory");
    check(vkBindImageMemory(device_,signal_.image,signal_.memory,0),"bind signal image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=signal_.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=kSignalFormat; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&signal_.view),"signal view");
    signal_extent_=extent; signal_initialized_=false;
}
void RtPathPass::ensure_sums(uint32_t records) {
    if (sums_ && records<=sums_capacity_) return;
    if (sums_) { idle_(); vkDestroyBuffer(device_,sums_,nullptr); vkFreeMemory(device_,sums_memory_,nullptr); sums_=VK_NULL_HANDLE; sums_memory_=VK_NULL_HANDLE; }
    sums_capacity_=std::max<uint32_t>(4096,records*2);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=VkDeviceSize(sums_capacity_)*32; info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT; info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_,&info,nullptr,&sums_),"sums buffer");
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device_,sums_,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=memory_type(memory_,requirements.memoryTypeBits);
    check(vkAllocateMemory(device_,&allocate,nullptr,&sums_memory_),"sums memory");
    check(vkBindBufferMemory(device_,sums_,sums_memory_,0),"bind sums buffer");
}
void RtPathPass::write_set(VkDescriptorSet set, const RtPathInputs& in, VkImageView signal_in) {
    VkWriteDescriptorSetAccelerationStructureKHR tlas{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; tlas.accelerationStructureCount=1; tlas.pAccelerationStructures=&in.tlas;
    const VkDescriptorImageInfo images[7]={{VK_NULL_HANDLE,in.depth,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.gbuffer,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.material,VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE,signal_.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,signal_in,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.color,VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE,in.albedo,VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo buffers[4]={{in.records,0,VK_WHOLE_SIZE},{in.hit_table,0,VK_WHOLE_SIZE},{in.hit_bases,0,VK_WHOLE_SIZE},{sums_,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[0].pNext=&tlas;
    for (uint32_t i=1;i<=3;++i) { writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo=&images[i-1]; }
    writes[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo=&buffers[0];
    writes[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[5].pImageInfo=&images[3];
    writes[6].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[6].pImageInfo=&images[4];
    writes[7].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[7].pImageInfo=&images[5];
    writes[8].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[8].pImageInfo=&images[6];
    for (uint32_t i=0;i<3;++i) { writes[9+i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[9+i].pBufferInfo=&buffers[1+i]; }
    vkUpdateDescriptorSets(device_,kBindings,writes,0,nullptr);
}
namespace {
PushParams make_params(const RtPathInputs& in, VkExtent2D signal) {
    PushParams params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    params.region[0]=in.region.width; params.region[1]=in.region.height; params.signal_size[0]=signal.width; params.signal_size[1]=signal.height;
    const auto& s=in.settings;
    const auto packed=rt_path_pack(s,in.frame);
    params.divisor_frame=packed.divisor_frame; params.counts=packed.counts; params.ao_radius=s.ao_radius; params.ao_strength=s.ao_strength;
    params.reach=s.reach; params.emission=s.emission; params.ambient=s.ambient; params.key=s.key;
    params.overhead[0]=in.overhead[0]; params.overhead[1]=in.overhead[1]; params.overhead[2]=in.overhead[2]; params.overhead[3]=s.key_reach;
    params.mix=s.mix; params.threshold=s.threshold; params.record_count=in.record_count; params.clamp_radiance=s.max_radiance;
    return params;
}
}
VkImageView RtPathPass::trace(const RtPathInputs& in) {
    if (in.slot>=slots_) throw std::invalid_argument("rt path: frame slot out of range");
    if (!in.albedo || !in.hit_table || !in.hit_bases) throw std::invalid_argument("rt path: albedo view or hit table missing");
    if (!in.settings.divisor || !in.settings.paths || !in.settings.bounces || !in.region.width || !in.region.height) throw std::logic_error("rt path: trace() with empty settings");
    const uint32_t divisor=in.settings.divisor;
    ensure_signal({(in.region.width+divisor-1)/divisor,(in.region.height+divisor-1)/divisor});
    ensure_sums(in.record_count);
    if (!signal_initialized_) {   // a new image to VK_IMAGE_LAYOUT_GENERAL
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=signal_.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&barrier;
        vkCmdPipelineBarrier2(in.command,&dependency);
        signal_initialized_=true;
    }
    write_set(trace_sets_[in.slot],in,signal_.view);   // the slot's previous frame has completed, so its sets can be rewritten
    const PushParams params=make_params(in,signal_extent_);
    // the mean albedo of the records: cleared, then accumulated on a grid with a step of 4 pixels
    vkCmdFillBuffer(in.command,sums_,0,VkDeviceSize(in.record_count)*32,0);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindDescriptorSets(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&trace_sets_[in.slot],0,nullptr);
    vkCmdPushConstants(in.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdDispatch(in.command,((in.region.width+3)/4+7)/8,((in.region.height+3)/4+7)/8,1);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    vkCmdDispatch(in.command,(signal_extent_.width+7)/8,(signal_extent_.height+7)/8,1);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    return signal_.view;
}
void RtPathPass::composite(const RtPathInputs& in, VkImageView filtered) {
    if (in.slot>=slots_ || !signal_.image) throw std::logic_error("rt path: composite() before trace()");
    write_set(composite_sets_[in.slot],in,filtered);
    const PushParams params=make_params(in,signal_extent_);
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[2]);
    vkCmdBindDescriptorSets(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&composite_sets_[in.slot],0,nullptr);
    vkCmdPushConstants(in.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(in.command,(in.region.width+7)/8,(in.region.height+7)/8,1);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
}
void RtPathPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    if (signal_.view) vkDestroyImageView(device_,signal_.view,nullptr);
    if (signal_.image) vkDestroyImage(device_,signal_.image,nullptr);
    if (signal_.memory) vkFreeMemory(device_,signal_.memory,nullptr);
    if (sums_) vkDestroyBuffer(device_,sums_,nullptr);
    if (sums_memory_) vkFreeMemory(device_,sums_memory_,nullptr);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=RtPathPass{};
}
}
