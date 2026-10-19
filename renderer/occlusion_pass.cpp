#include "occlusion_pass.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("occlusion: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/gtao.slang Params (push constants)
struct Params {
    float viewport[4]; float view_a[4]; float view_b[2]; uint32_t region[2]; uint32_t ao_size[2];
    float radius, falloff; uint32_t slices, steps, noise, flags; float strength, reserved; float depth_range[2];
};
static_assert(sizeof(Params)==96 && offsetof(Params,region)==40 && offsetof(Params,radius)==56 && offsetof(Params,strength)==80 && offsetof(Params,depth_range)==88);
constexpr float kRadius=0.5f;      // view-space units: the game's units are metres (projection near 0.01, far 1000)
constexpr float kFalloff=0.6f;
constexpr float kStrength=1.0f;
// Between passes: compute to compute. After the last one (`last`) everything that follows (the upscaler reads the colour) waits for it.
void memory_barrier(VkCommandBuffer command, bool last) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.dstStageMask=last ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstAccessMask=last ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout=before; barrier.newLayout=after;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount=1; info.pImageMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
}
AoPreset ao_preset(EffectLevel level) {
    switch(level) {
    case EffectLevel::low: return {1,2,false};
    case EffectLevel::medium: return {2,2,false};
    case EffectLevel::high: return {3,3,true};
    case EffectLevel::ultra: return {9,3,true};
    case EffectLevel::off: break;
    }
    return {0,0,false};
}
uint32_t OcclusionPass::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((mask&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&properties)==properties) return i;
    throw std::runtime_error("occlusion: required memory type unavailable");
}
OcclusionPass::Image OcclusionPass::make_image(VkFormat format, VkExtent2D extent) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&result.image),"image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,result.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_,&allocate,nullptr,&result.memory),"image memory");
    check(vkBindImageMemory(device_,result.image,result.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&result.view),"image view");
    return result;
}
void OcclusionPass::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void OcclusionPass::write_image(uint32_t binding, VkImageView view, VkDescriptorType type) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set_; write.dstBinding=binding; write.descriptorCount=1; write.descriptorType=type; write.pImageInfo=&info;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
}
void OcclusionPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders) {
    if (device_) return;
    device_=device; memory_=memory;
    try {
        const char* names[3]={"gtao.ao.spv","gtao.denoise.spv","gtao.composite.spv"};
        for (size_t i=0;i<3;++i) {
            std::ifstream file(shaders/names[i],std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
            if (code.empty() || code.size()%4) throw std::runtime_error(std::string("occlusion: shader ")+names[i]+" missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_,&module,nullptr,&modules_[i]),"shader module");
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        check(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"sampler");
        VkDescriptorSetLayoutBinding bindings[8]{};
        bindings[0]={0,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&sampler_};
        for (uint32_t i=1;i<8;++i) bindings[i]={i,(i==3 || i>=6) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=8; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<3;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],i==0 ? "aoMain" : i==1 ? "denoiseMain" : "compositeMain",nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout_;
        check(vkAllocateDescriptorSets(device_,&allocate,&set_),"descriptor set");
    } catch (...) { shutdown(); throw; }
}
void OcclusionPass::resize(VkExtent2D ao_size) {
    check(vkDeviceWaitIdle(device_),"idle before resize");   // the set and the images may be used by frames in flight (render scale or preset change)
    destroy_image(raw_); destroy_image(blurred_);
    raw_=make_image(VK_FORMAT_R8_UNORM,ao_size); blurred_=make_image(VK_FORMAT_R8_UNORM,ao_size);
    write_image(4,raw_.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(5,blurred_.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    write_image(6,raw_.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); write_image(7,blurred_.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    ao_size_=ao_size;
}
void OcclusionPass::record(const OcclusionInputs& in) {
    const auto preset=ao_preset(in.ao);
    if (!preset.slices || !in.region.width || !in.region.height) return;
    const VkExtent2D ao_size=preset.full_resolution ? in.region : VkExtent2D{(in.region.width+1)/2,(in.region.height+1)/2};
    if (ao_size.width!=ao_size_.width || ao_size.height!=ao_size_.height) resize(ao_size);
    if (bound_depth_!=in.depth.view || bound_gbuffer_!=in.gbuffer.view || bound_color_!=in.color.view) {
        if (bound_depth_) check(vkDeviceWaitIdle(device_),"idle before rebinding");   // new scene targets after a rebuild
        write_image(1,in.depth.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(2,in.gbuffer.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(3,in.color.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        bound_depth_=in.depth.view; bound_gbuffer_=in.gbuffer.view; bound_color_=in.color.view;
    }
    const auto command=in.command;
    for (Image* image : {&raw_,&blurred_}) if (!image->initialized) { image_barrier(command,image->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); image->initialized=true; }
    Params params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.region[0]=in.region.width; params.region[1]=in.region.height; params.ao_size[0]=ao_size.width; params.ao_size[1]=ao_size.height;
    params.radius=kRadius; params.falloff=kFalloff; params.slices=preset.slices; params.steps=preset.steps; params.noise=in.noise_frame; params.strength=kStrength;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set_,0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    const VkExtent2D sizes[3]={ao_size,ao_size,in.region};
    for (size_t pass=0;pass<3;++pass) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[pass]);
        vkCmdDispatch(command,(sizes[pass].width+7)/8,(sizes[pass].height+7)/8,1);
        memory_barrier(command,pass==2);
    }
}
void OcclusionPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroy_image(raw_); destroy_image(blurred_);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    if (sampler_) vkDestroySampler(device_,sampler_,nullptr);
    *this=OcclusionPass{};
}
}
