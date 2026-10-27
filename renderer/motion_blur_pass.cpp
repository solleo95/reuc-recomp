#include "motion_blur_pass.h"
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("motion blur: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/motion_blur.slang Params (push constants)
struct Params { uint32_t output_size[2], render_size[2]; float shutter; uint32_t samples, tile_size, reserved; };
static_assert(sizeof(Params)==32);
constexpr uint32_t kTileSize=16;
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
uint32_t MotionBlurPass::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((mask&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&properties)==properties) return i;
    throw std::runtime_error("motion blur: required memory type unavailable");
}
MotionBlurPass::Image MotionBlurPass::make_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=usage; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
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
void MotionBlurPass::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void MotionBlurPass::write_image(uint32_t binding, VkImageView view, VkDescriptorType type) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set_; write.dstBinding=binding; write.descriptorCount=1; write.descriptorType=type; write.pImageInfo=&info;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
}
void MotionBlurPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle) {
    if (device_) return;
    device_=device; memory_=memory;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        const char* names[2]={"motion_blur.tile.spv","motion_blur.blur.spv"};
        for (size_t i=0;i<2;++i) {
            std::ifstream file(shaders/names[i],std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
            if (code.empty() || code.size()%4) throw std::runtime_error(std::string("motion blur: shader ")+names[i]+" missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_,&module,nullptr,&modules_[i]),"shader module");
        }
        VkDescriptorSetLayoutBinding bindings[5]{};   // 1 colour copy, 2 motion, 4 tile-max (sampled); 3 colour, 5 tile-max output (storage)
        for (uint32_t i=0;i<5;++i) bindings[i]={i+1,(i==2 || i==4) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=5; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<2;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],i==0 ? "tileMain" : "blurMain",nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=2; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout_;
        check(vkAllocateDescriptorSets(device_,&allocate,&set_),"descriptor set");
    } catch (...) { shutdown(); throw; }
}
void MotionBlurPass::record(const MotionBlurInputs& in) {
    const auto preset=blur_preset(in.level);
    if (!preset.samples || !in.output_size.width || !in.output_size.height || !in.render_size.width || !in.render_size.height) return;
    const VkExtent2D tiles{(in.render_size.width+kTileSize-1)/kTileSize,(in.render_size.height+kTileSize-1)/kTileSize};
    const VkExtent2D full{in.color.extent};
    if (full.width!=source_size_.width || full.height!=source_size_.height || tiles.width!=tiles_size_.width || tiles.height!=tiles_size_.height) {
        idle_();   // the set and the images may be used by frames in flight (render scale change)
        destroy_image(source_); destroy_image(tiles_);
        source_=make_image(VK_FORMAT_R8G8B8A8_UNORM,full,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        tiles_=make_image(VK_FORMAT_R16G16_SFLOAT,tiles,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT);
        write_image(1,source_.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(4,tiles_.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(5,tiles_.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        source_size_=full; tiles_size_=tiles;
    }
    if (bound_color_!=in.color.view || bound_motion_!=in.motion.view || bound_generation_!=in.targets_generation) {   // the generation catches a recreated view that got a destroyed one's handle
        if (bound_color_) idle_();
        write_image(2,in.motion.view,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(3,in.color.view,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        bound_color_=in.color.view; bound_motion_=in.motion.view; bound_generation_=in.targets_generation;
    }
    const auto command=in.command;
    if (!source_.initialized) { image_barrier(command,source_.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); source_.initialized=true; }
    if (!tiles_.initialized) { image_barrier(command,tiles_.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); tiles_.initialized=true; }
    barrier_all(command);   // the upscaler's writes
    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.extent={full.width,full.height,1};
    vkCmdCopyImage(command,in.color.image,VK_IMAGE_LAYOUT_GENERAL,source_.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    barrier_all(command);
    Params params{};
    params.output_size[0]=in.output_size.width; params.output_size[1]=in.output_size.height;
    params.render_size[0]=in.render_size.width; params.render_size[1]=in.render_size.height;
    params.shutter=preset.shutter; params.samples=preset.samples; params.tile_size=kTileSize;
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set_,0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdDispatch(command,(tiles.width+7)/8,(tiles.height+7)/8,1);
    barrier_all(command);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    vkCmdDispatch(command,(in.output_size.width+7)/8,(in.output_size.height+7)/8,1);
    barrier_all(command);
}
void MotionBlurPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroy_image(source_); destroy_image(tiles_);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=MotionBlurPass{};
}
}
