#include "rt_denoiser.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("rt denoiser: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/rt_denoise.slang Params (push constants)
struct Params { uint32_t size[2], region[2]; float depth[2], depth_range[2]; uint32_t signal, flags, step, first; };
static_assert(sizeof(Params)==48 && offsetof(Params,depth)==16 && offsetof(Params,signal)==32);
constexpr uint32_t kFlagReset=1;
constexpr VkFormat kFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBindings=12;   // 0-3 noisy, depth, motion, G-buffer (sampled); 4-6 previous history, moments, guide; 7-9 current ones; 10, 11 filter in, out
// Between passes: compute to compute. After the last one (`last`) everything that follows (the caller reads the result) waits for it.
void memory_barrier(VkCommandBuffer command, bool last) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.dstStageMask=last ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.dstAccessMask=last ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
void image_barrier(VkCommandBuffer command, VkImage image) {   // a new image to VK_IMAGE_LAYOUT_GENERAL
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount=1; info.pImageMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
}
RtDenoiser::Image RtDenoiser::make_image(VkExtent2D extent) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=kFormat;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&result.image),"image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,result.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=UINT32_MAX;
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((requirements.memoryTypeBits&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { allocate.memoryTypeIndex=i; break; }
    if (allocate.memoryTypeIndex==UINT32_MAX) { vkDestroyImage(device_,result.image,nullptr); throw std::runtime_error("rt denoiser: required memory type unavailable"); }
    check(vkAllocateMemory(device_,&allocate,nullptr,&result.memory),"image memory");
    check(vkBindImageMemory(device_,result.image,result.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=kFormat; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&result.view),"image view");
    return result;
}
void RtDenoiser::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void RtDenoiser::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle) {
    if (device_) return;
    device_=device; memory_=memory;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        const char* names[2]={"rt_denoise.temporal.spv","rt_denoise.atrous.spv"};
        for (size_t i=0;i<2;++i) {
            std::ifstream file(shaders/names[i],std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
            if (code.empty() || code.size()%4) throw std::runtime_error(std::string("rt denoiser: shader ")+names[i]+" missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_,&module,nullptr,&modules_[i]),"shader module");
        }
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (uint32_t i=0;i<kBindings;++i) bindings[i]={i,i<4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=kBindings; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<2;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],i==0 ? "temporalMain" : "atrousMain",nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        constexpr uint32_t sets=6;
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4*sets},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,(kBindings-4)*sets}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=sets; pool.poolSizeCount=2; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        const VkDescriptorSetLayout layouts[sets]={set_layout_,set_layout_,set_layout_,set_layout_,set_layout_,set_layout_};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=sets; allocate.pSetLayouts=layouts;
        check(vkAllocateDescriptorSets(device_,&allocate,&sets_[0][0]),"descriptor sets");
    } catch (...) { shutdown(); throw; }
}
void RtDenoiser::resize(VkExtent2D signal_extent) {
    if (signal_extent.width==extent_.width && signal_extent.height==extent_.height) return;
    if (extent_.width) idle_();   // the sets and the images may be used by frames in flight
    for (int i=0;i<2;++i) { destroy_image(history_[i]); destroy_image(moments_[i]); destroy_image(guide_[i]); destroy_image(filtered_[i]); }
    extent_=signal_extent; initialized_=false; reset_=true; output_=VK_NULL_HANDLE;
    for (int i=0;i<2;++i) { history_[i]=make_image(extent_); moments_[i]=make_image(extent_); guide_[i]=make_image(extent_); filtered_[i]=make_image(extent_); }
    bound_noisy_=VK_NULL_HANDLE;   // the sets name the old images: write_sets() at the next denoise()
}
void RtDenoiser::write_sets() {
    const VkImageView sampled[4]={bound_noisy_,bound_depth_,bound_motion_,bound_gbuffer_};
    for (uint32_t parity=0;parity<2;++parity) for (uint32_t step=0;step<3;++step) {
        const uint32_t previous=parity^1;
        const VkImageView storage[kBindings-4]={history_[previous].view,moments_[previous].view,guide_[previous].view,history_[parity].view,moments_[parity].view,guide_[parity].view,
            step==0 ? history_[parity].view : filtered_[step-1].view, filtered_[step==1 ? 1 : 0].view};
        VkDescriptorImageInfo infos[kBindings]; VkWriteDescriptorSet writes[kBindings];
        for (uint32_t i=0;i<kBindings;++i) {
            infos[i]={VK_NULL_HANDLE,i<4 ? sampled[i] : storage[i-4],VK_IMAGE_LAYOUT_GENERAL};
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=sets_[parity][step]; writes[i].dstBinding=i; writes[i].descriptorCount=1;
            writes[i].descriptorType=i<4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[i].pImageInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device_,kBindings,writes,0,nullptr);
    }
}
VkImageView RtDenoiser::denoise(VkCommandBuffer command, RtSignal signal, VkImageView noisy, const DenoiseInputs& in, uint32_t atrous_iterations) {
    if (!extent_.width || !extent_.height) throw std::logic_error("rt denoiser: denoise() before resize()");
    if (bound_noisy_!=noisy || bound_depth_!=in.depth || bound_motion_!=in.motion || bound_gbuffer_!=in.gbuffer || bound_generation_!=in.targets_generation) {   // the generation catches a recreated view that got a destroyed one's handle
        if (bound_noisy_) idle_();   // new inputs while frames may still use the sets
        bound_noisy_=noisy; bound_depth_=in.depth; bound_motion_=in.motion; bound_gbuffer_=in.gbuffer; bound_generation_=in.targets_generation;
        write_sets();
    }
    if (!initialized_) {
        for (int i=0;i<2;++i) for (Image* image:{&history_[i],&moments_[i],&guide_[i],&filtered_[i]}) image_barrier(command,image->image);
        initialized_=true;
    }
    parity_^=1;
    Params params{{extent_.width,extent_.height},{in.region.width,in.region.height},{in.view.depth_a,in.view.depth_b},{in.depth_range[0],in.depth_range[1]},
        uint32_t(signal),reset_ ? kFlagReset : 0u,1,1};
    reset_=false;
    const uint32_t groups_x=(extent_.width+7)/8, groups_y=(extent_.height+7)/8;
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[parity_][0],0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(command,groups_x,groups_y,1);
    memory_barrier(command,atrous_iterations==0);
    if (atrous_iterations) vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    for (uint32_t i=0;i<atrous_iterations;++i) {   // steps 1, 2, 4, ...: history -> filtered[0] -> filtered[1] -> filtered[0] ...
        const uint32_t set=i==0 ? 0 : (i%2 ? 1 : 2);
        params.step=1u<<std::min(i,15u); params.first=i==0 ? 1 : 0;
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[parity_][set],0,nullptr);
        vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
        vkCmdDispatch(command,groups_x,groups_y,1);
        memory_barrier(command,i+1==atrous_iterations);
    }
    const Image& result=atrous_iterations ? filtered_[(atrous_iterations-1)%2] : history_[parity_];
    output_=result.image;
    return result.view;
}
void RtDenoiser::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (int i=0;i<2;++i) { destroy_image(history_[i]); destroy_image(moments_[i]); destroy_image(guide_[i]); destroy_image(filtered_[i]); }
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=RtDenoiser{};
}
}
