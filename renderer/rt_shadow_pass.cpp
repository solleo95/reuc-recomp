#include "rt_shadow_pass.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("rt shadows: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/rt_shadow.slang Params (push constants)
struct PushParams {
    float viewport[4], view_a[4], view_b[2], depth_range[2];
    uint32_t region[2], signal_size[2];
    uint32_t divisor, frame, sun_rays, local_lights;
    float sun_cone, light_radius;
    float lamp_clearance;
    uint32_t reserved;
};
static_assert(sizeof(PushParams)==96 && offsetof(PushParams,region)==48 && offsetof(PushParams,divisor)==64 && offsetof(PushParams,sun_cone)==80);
constexpr VkFormat kSignalFormat=VK_FORMAT_R16_SFLOAT;
constexpr uint32_t kBindings=9;
VkShaderModule load_module(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
    if (code.empty() || code.size()%4) throw std::runtime_error("rt shadows: shader "+path.string()+" missing or invalid");
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
}
void RtShadowPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle) {
    if (device_) return;
    if (!slots || slots>trace_sets_.size()) throw std::invalid_argument("rt shadows: unsupported frame slot count");
    device_=device; memory_=memory; slots_=slots;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        modules_[0]=load_module(device_,shaders/"rt_shadow.trace.spv"); modules_[1]=load_module(device_,shaders/"rt_shadow.composite.spv");
        const VkDescriptorType types[kBindings]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (uint32_t i=0;i<kBindings;++i) bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=kBindings; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushParams)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<2;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],i==0 ? "traceMain" : "compositeMain",nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const uint32_t sets=2*slots;
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,sets},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4*sets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2*sets},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*sets}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=sets; pool.poolSizeCount=4; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        const std::vector<VkDescriptorSetLayout> layouts(sets,set_layout_);
        std::vector<VkDescriptorSet> allocated(sets);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=sets; allocate.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device_,&allocate,allocated.data()),"descriptor sets");
        for (uint32_t i=0;i<slots;++i) { trace_sets_[i]=allocated[2*i]; composite_sets_[i]=allocated[2*i+1]; }
    } catch (...) { shutdown(); throw; }
}
void RtShadowPass::ensure_signal(VkExtent2D extent) {
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
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=UINT32_MAX;
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((requirements.memoryTypeBits&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { allocate.memoryTypeIndex=i; break; }
    if (allocate.memoryTypeIndex==UINT32_MAX) throw std::runtime_error("rt shadows: required memory type unavailable");
    check(vkAllocateMemory(device_,&allocate,nullptr,&signal_.memory),"signal memory");
    check(vkBindImageMemory(device_,signal_.image,signal_.memory,0),"bind signal image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=signal_.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=kSignalFormat; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&signal_.view),"signal view");
    signal_extent_=extent; signal_initialized_=false; ++images_created_;
}
void RtShadowPass::write_set(VkDescriptorSet set, const RtShadowInputs& in, VkImageView signal_in) {
    VkWriteDescriptorSetAccelerationStructureKHR tlas{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; tlas.accelerationStructureCount=1; tlas.pAccelerationStructures=&in.tlas;
    const VkDescriptorImageInfo images[6]={{VK_NULL_HANDLE,in.depth,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.gbuffer,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.material,VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE,signal_.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,signal_in,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.color,VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo buffers[2]={{in.records,0,VK_WHOLE_SIZE},{in.lights,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[0].pNext=&tlas;
    for (uint32_t i=1;i<=3;++i) { writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo=&images[i-1]; }
    for (uint32_t i=4;i<=5;++i) { writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&buffers[i-4]; }
    writes[6].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[6].pImageInfo=&images[3];
    writes[7].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[7].pImageInfo=&images[4];
    writes[8].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[8].pImageInfo=&images[5];
    vkUpdateDescriptorSets(device_,kBindings,writes,0,nullptr);
}
VkImageView RtShadowPass::trace(const RtShadowInputs& in) {
    if (in.slot>=slots_) throw std::invalid_argument("rt shadows: frame slot out of range");
    const RtShadowPreset sun=rt_shadow_preset(in.sun);
    const RtLocalPreset local=rt_local_preset(in.local);
    divisor_=std::min(sun.divisor ? sun.divisor : UINT32_MAX,local.divisor ? local.divisor : UINT32_MAX);
    if (divisor_==UINT32_MAX || !in.region.width || !in.region.height) throw std::logic_error("rt shadows: trace() with both effects off");
    ensure_signal({(in.region.width+divisor_-1)/divisor_,(in.region.height+divisor_-1)/divisor_});
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
    PushParams params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    params.region[0]=in.region.width; params.region[1]=in.region.height; params.signal_size[0]=signal_extent_.width; params.signal_size[1]=signal_extent_.height;
    params.divisor=divisor_; params.frame=in.frame; params.sun_rays=sun.divisor ? sun.rays : 0; params.local_lights=local.divisor ? local.lights : 0;
    params.sun_cone=sun.soft ? kRtSunConeRadians : 0.f; params.light_radius=local.soft ? kRtLightRadiusFraction : 0.f; params.lamp_clearance=kRtLampClearance;
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdBindDescriptorSets(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&trace_sets_[in.slot],0,nullptr);
    vkCmdPushConstants(in.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(in.command,(signal_extent_.width+7)/8,(signal_extent_.height+7)/8,1);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    return signal_.view;
}
void RtShadowPass::composite(const RtShadowInputs& in, VkImageView filtered) {
    if (in.slot>=slots_ || !signal_.image) throw std::logic_error("rt shadows: composite() before trace()");
    write_set(composite_sets_[in.slot],in,filtered);
    PushParams params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    params.region[0]=in.region.width; params.region[1]=in.region.height; params.signal_size[0]=signal_extent_.width; params.signal_size[1]=signal_extent_.height;
    params.divisor=divisor_;
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    vkCmdBindDescriptorSets(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&composite_sets_[in.slot],0,nullptr);
    vkCmdPushConstants(in.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(in.command,(in.region.width+7)/8,(in.region.height+7)/8,1);
    memory_barrier(in.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
}
void RtShadowPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    if (signal_.view) vkDestroyImageView(device_,signal_.view,nullptr);
    if (signal_.image) vkDestroyImage(device_,signal_.image,nullptr);
    if (signal_.memory) vkFreeMemory(device_,signal_.memory,nullptr);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=RtShadowPass{};
}
}
