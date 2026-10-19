#include "volumetric_pass.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("volumetric: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/volumetric_shafts.slang Params (push constants)
struct Params { float light[2]; uint32_t region[2], low_size[2]; uint32_t samples; float density, decay, intensity; uint32_t pad[2]; float tint[4]; };
static_assert(sizeof(Params)==64);
constexpr float kDensity=0.9f, kDecay=0.96f;
void barrier_all(VkCommandBuffer command) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
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
ShaftPreset shaft_preset(EffectLevel level) {
    switch(level) {
    case EffectLevel::low: return {4,24,0.35f};
    case EffectLevel::medium: case EffectLevel::high: case EffectLevel::ultra: return {2,40,0.35f};   // high and ultra get the froxel volume once it exists
    case EffectLevel::off: break;
    }
    return {1,0,0};
}
uint32_t VolumetricPass::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((mask&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&properties)==properties) return i;
    throw std::runtime_error("volumetric: required memory type unavailable");
}
VolumetricPass::Image VolumetricPass::make_image(VkExtent2D extent) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_R8G8B8A8_UNORM;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&result.image),"image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,result.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_,&allocate,nullptr,&result.memory),"image memory");
    check(vkBindImageMemory(device_,result.image,result.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_R8G8B8A8_UNORM; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&result.view),"image view");
    return result;
}
void VolumetricPass::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void VolumetricPass::write_image(uint32_t binding, VkImageView view, VkDescriptorType type) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set_; write.dstBinding=binding; write.descriptorCount=1; write.descriptorType=type; write.pImageInfo=&info;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
}
void VolumetricPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle) {
    if (device_) return;
    device_=device; memory_=memory;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        const char* names[2]={"volumetric_shafts.shaft.spv","volumetric_shafts.composite.spv"};
        for (size_t i=0;i<2;++i) {
            std::ifstream file(shaders/names[i],std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
            if (code.empty() || code.size()%4) throw std::runtime_error(std::string("volumetric: shader ")+names[i]+" missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_,&module,nullptr,&modules_[i]),"shader module");
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        check(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"sampler");
        VkDescriptorSetLayoutBinding bindings[4]{};   // 0 sampler; 1 colour (storage); 2 shafts (sampled); 3 shafts (storage)
        bindings[0]={0,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&sampler_};
        bindings[1]={1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[2]={2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[3]={3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=4; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<2;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],i==0 ? "shaftMain" : "compositeMain",nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout_;
        check(vkAllocateDescriptorSets(device_,&allocate,&set_),"descriptor set");
    } catch (...) { shutdown(); throw; }
}
bool VolumetricPass::record(const VolumetricInputs& in) {
    const auto preset=shaft_preset(in.level);
    if (!preset.samples || !in.light || !in.region.width || !in.region.height) return false;
    const auto light=light_screen_position(*in.light,in.view,in.viewport);
    if (!light) return false;
    const VkExtent2D size{(in.region.width+preset.divisor-1)/preset.divisor,(in.region.height+preset.divisor-1)/preset.divisor};
    if (size.width!=shafts_size_.width || size.height!=shafts_size_.height) {
        idle_();   // the set and the image may be used by frames in flight (render scale or preset change)
        destroy_image(shafts_);
        shafts_=make_image(size);
        write_image(2,shafts_.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(3,shafts_.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        shafts_size_=size;
    }
    if (bound_color_!=in.color.view || bound_generation_!=in.targets_generation) {   // the generation catches a recreated view that got a destroyed one's handle
        if (bound_color_) idle_();
        write_image(1,in.color.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        bound_color_=in.color.view; bound_generation_=in.targets_generation;
    }
    const auto command=in.command;
    if (!shafts_.initialized) { image_barrier(command,shafts_.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); shafts_.initialized=true; }
    Params params{};
    params.light[0]=(*light)[0]; params.light[1]=(*light)[1];
    params.region[0]=in.region.width; params.region[1]=in.region.height; params.low_size[0]=size.width; params.low_size[1]=size.height;
    params.samples=preset.samples; params.density=kDensity; params.decay=kDecay; params.intensity=preset.intensity;
    for (int c=0;c<3;++c) params.tint[c]=std::clamp(in.light->color[c],0.f,1.f);
    barrier_all(command);   // the occlusion pass's writes
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set_,0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdDispatch(command,(size.width+7)/8,(size.height+7)/8,1);
    barrier_all(command);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    vkCmdDispatch(command,(in.region.width+7)/8,(in.region.height+7)/8,1);
    barrier_all(command);
    return true;
}
void VolumetricPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroy_image(shafts_);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    if (sampler_) vkDestroySampler(device_,sampler_,nullptr);
    *this=VolumetricPass{};
}
}
