#include "bloom_pass.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("bloom: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/bloom.slang Params (push constants)
struct Params { uint32_t src[2], dst[2], region[2]; float weight, weight_a, scale, headroom; uint32_t reserved[2]; };
static_assert(sizeof(Params)==48);
constexpr uint32_t kSetsShared=10;   // down first, down 1..5, up 4..1
constexpr VkFormat kFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
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
uint32_t BloomPass::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((mask&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&properties)==properties) return i;
    throw std::runtime_error("bloom: required memory type unavailable");
}
BloomPass::Image BloomPass::make_image(VkExtent2D extent) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=kFormat;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&result.image),"image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,result.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_,&allocate,nullptr,&result.memory),"image memory");
    check(vkBindImageMemory(device_,result.image,result.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=kFormat; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&result.view),"image view");
    return result;
}
void BloomPass::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void BloomPass::write(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set; write.dstBinding=binding; write.descriptorCount=1; write.descriptorType=type; write.pImageInfo=&info;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
}
void BloomPass::dispatch(VkCommandBuffer command, VkPipeline pipeline, VkDescriptorSet set, const void* params, VkExtent2D size) {
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set,0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params),params);
    vkCmdDispatch(command,(size.width+7)/8,(size.height+7)/8,1);
    barrier_all(command);
}
void BloomPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle) {
    if (device_) return;
    device_=device; memory_=memory; slots_=slots;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        const char* names[4]={"bloom.down_first.spv","bloom.down.spv","bloom.up.spv","bloom.composite.spv"};
        const char* entries[4]={"downFirstMain","downMain","upMain","compositeMain"};
        for (size_t i=0;i<4;++i) {
            std::ifstream file(shaders/names[i],std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
            if (code.empty() || code.size()%4) throw std::runtime_error(std::string("bloom: shader ")+names[i]+" missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_,&module,nullptr,&modules_[i]),"shader module");
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR; sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"sampler");
        VkDescriptorSetLayoutBinding bindings[5]{};   // 0 sampler, 1 image A, 2 image B (sampled), 3 level output (rgba16f), 4 colour (rgba8) (storage)
        bindings[0]={0,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[1]={1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[2]={2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[3]={3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[4]={4,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=5; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        for (size_t i=0;i<4;++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules_[i],entries[i],nullptr};
            pipeline.layout=layout_;
            check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipelines_[i]),"pipeline");
        }
        const uint32_t count=kSetsShared+2*slots_;
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLER,count},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2*count},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*count}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=count; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        sets_.resize(count);
        std::vector<VkDescriptorSetLayout> layouts(count,set_layout_);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=count; allocate.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device_,&allocate,sets_.data()),"descriptor sets");
        for (auto set_handle:sets_) {
            const VkDescriptorImageInfo info{sampler_,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set_handle; write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; write.pImageInfo=&info;
            vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
        }
        glow_.resize(slots_); valid_.assign(slots_,false);
    } catch (...) { shutdown(); throw; }
}
void BloomPass::record(const BloomInputs& in) {
    if (!in.settings.enabled || in.settings.strength==0 || !in.region.width || !in.region.height || in.slot>=slots_) { invalidate(in.slot); return; }
    const auto command=in.command;
    if (in.region.width!=region_.width || in.region.height!=region_.height) {   // first use, or the scene targets changed size
        idle_();   // the sets and the images may be used by frames in flight
        for (auto& image:level_) destroy_image(image);
        for (auto& image:glow_) destroy_image(image);
        for (uint32_t i=0;i<kLevels;++i) {
            const auto size=bloom_level_extent(in.region.width,in.region.height,i);
            level_extent_[i]={size.width,size.height};
            level_[i]=make_image(level_extent_[i]);
        }
        for (auto& image:glow_) image=make_image(level_extent_[0]);
        const auto sampled=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, storage=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write(sets_[0],3,storage,level_[0].view);
        for (uint32_t i=1;i<kLevels;++i) { write(sets_[i],1,sampled,level_[i-1].view); write(sets_[i],3,storage,level_[i].view); }
        for (uint32_t i=1;i<kLevels-1;++i) {   // up passes 4..1 read the coarser level and accumulate into their own
            const auto set=sets_[10-i];
            write(set,1,sampled,level_[i+1].view); write(set,2,sampled,level_[i].view); write(set,3,storage,level_[i].view);
        }
        for (uint32_t s=0;s<slots_;++s) {
            const auto up=sets_[kSetsShared+s], composite=sets_[kSetsShared+slots_+s];
            write(up,1,sampled,level_[1].view); write(up,2,sampled,level_[0].view); write(up,3,storage,glow_[s].view);
            write(composite,1,sampled,glow_[s].view);
            valid_[s]=false;
        }
        region_=in.region; bound_color_=VK_NULL_HANDLE; ++generation_;
    }
    if (bound_color_!=in.color.view || bound_generation_!=in.targets_generation) {   // the generation catches a recreated view that got a destroyed one's handle
        if (bound_color_) idle_();
        write(sets_[0],1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,in.color.view);
        for (uint32_t s=0;s<slots_;++s) write(sets_[kSetsShared+slots_+s],4,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,in.color.view);
        bound_color_=in.color.view; bound_generation_=in.targets_generation;
    }
    for (auto* image:[&] { std::vector<Image*> all; for (auto& level:level_) all.push_back(&level); for (auto& glow:glow_) all.push_back(&glow); return all; }())
        if (!image->initialized) { image_barrier(command,image->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); image->initialized=true; }
    const float mix=bloom_mix(in.settings,in.hdr);
    Params params{};
    params.region[0]=in.region.width; params.region[1]=in.region.height;
    params.headroom=in.hdr ? std::max(1.f,in.hdr_headroom) : kBloomSdrHeadroom;
    params.scale=1; params.weight_a=1;
    barrier_all(command);   // the upscaler's, the blur's and the previous frame's accesses
    params.dst[0]=level_extent_[0].width; params.dst[1]=level_extent_[0].height;
    dispatch(command,pipelines_[0],sets_[0],&params,level_extent_[0]);
    for (uint32_t i=1;i<kLevels;++i) {
        params.src[0]=level_extent_[i-1].width; params.src[1]=level_extent_[i-1].height;
        params.dst[0]=level_extent_[i].width; params.dst[1]=level_extent_[i].height;
        dispatch(command,pipelines_[1],sets_[i],&params,level_extent_[i]);
    }
    for (uint32_t i=kLevels-2;i>=1;--i) {   // level 4 takes the coarsest level's own weight, the others the accumulated sum
        params.src[0]=level_extent_[i+1].width; params.src[1]=level_extent_[i+1].height;
        params.dst[0]=level_extent_[i].width; params.dst[1]=level_extent_[i].height;
        params.weight=kBloomWeights[i]; params.weight_a=i==kLevels-2 ? kBloomWeights[kLevels-1] : 1.f;
        dispatch(command,pipelines_[2],sets_[10-i],&params,level_extent_[i]);
    }
    params.src[0]=level_extent_[1].width; params.src[1]=level_extent_[1].height;
    params.dst[0]=level_extent_[0].width; params.dst[1]=level_extent_[0].height;
    params.weight=kBloomWeights[0]; params.weight_a=1; params.scale=mix;
    dispatch(command,pipelines_[2],sets_[kSetsShared+in.slot],&params,level_extent_[0]);
    if (!in.hdr) dispatch(command,pipelines_[3],sets_[kSetsShared+slots_+in.slot],&params,in.region);
    valid_[in.slot]=in.hdr;
}
void BloomPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (auto& image:level_) destroy_image(image);
    for (auto& image:glow_) destroy_image(image);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline:pipelines_) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    if (sampler_) vkDestroySampler(device_,sampler_,nullptr);
    for (auto module:modules_) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=BloomPass{};
}
}
