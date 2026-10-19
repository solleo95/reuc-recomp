// SMAA 1x as an Upscaler at ratio 1: a region-sized copy of the scene goes through luma edge detection, blending weight calculation and
// neighbourhood blending (renderer/shaders/smaa.slang, third_party/smaa) into the output. No jitter, no motion vectors.
#include "upscaler.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("SMAA: ") + what + " failed: VkResult " + std::to_string(result)); }
struct Metrics { float rt[4]; };   // 1/width, 1/height, width, height of the region (smaa.slang Metrics)
struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };

class SmaaUpscaler final : public Upscaler {
public:
    SmaaUpscaler(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders) : device_(device), memory_(memory) {
        try { create(shaders); } catch (...) { destroy(); throw; }
    }
    ~SmaaUpscaler() override { destroy(); }
    bool sharpens() const override { return false; }
    RenderExtent minimum_internal(RenderExtent output) const override { return output; }   // ratio 1: the scene renders at the output size
    void evaluate(const UpscaleInputs& in) override {
        const VkExtent2D region = in.render_size;
        if (region.width != extent_.width || region.height != extent_.height) resize(region);
        if (bound_output_ != in.output.view) bind_output(in.output.view);
        const auto command = in.command;
        if (!lookup_uploaded_) upload_lookups(command);
        for (Image* image : {&source_, &edges_, &weights_}) if (!image->initialized) { barrier(command, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL); image->initialized = true; }
        // The region of the scene, as a sampled copy with its own clamped border.
        barrier(command, source_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.extent = {region.width, region.height, 1};
        vkCmdCopyImage(command, in.color.image, VK_IMAGE_LAYOUT_GENERAL, source_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(command, source_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        const Metrics metrics{{1.f / region.width, 1.f / region.height, float(region.width), float(region.height)}};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof metrics, &metrics);
        for (size_t pass = 0; pass < 3; ++pass) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[pass]);
            vkCmdDispatch(command, (region.width + 7) / 8, (region.height + 7) / 8, 1);
            memory_barrier(command);   // the next pass reads what this one wrote
        }
    }

private:
    void create(const std::filesystem::path& shaders) {
        const char* names[3] = {"smaa.edges.spv", "smaa.weights.spv", "smaa.blend.spv"};
        for (size_t i = 0; i < 3; ++i) {
            std::ifstream file(shaders / names[i], std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
            if (code.empty() || code.size() % 4) throw std::runtime_error(std::string("SMAA: shader ") + names[i] + " missing or invalid");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = code.size(); module.pCode = reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(device_, &module, nullptr, &modules_[i]), "shader module");
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        check(vkCreateSampler(device_, &sampler, nullptr, &linear_), "linear sampler");
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        check(vkCreateSampler(device_, &sampler, nullptr, &point_), "point sampler");
        const VkSampler samplers[2] = {linear_, point_};
        VkDescriptorSetLayoutBinding bindings[10]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &samplers[0]};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &samplers[1]};
        for (uint32_t i = 2; i < 7; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 7; i < 10; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = 10; set.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_), "descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Metrics)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "pipeline layout");
        for (size_t i = 0; i < 3; ++i) {
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, modules_[i], i == 0 ? "edgesMain" : i == 1 ? "weightsMain" : "blendMain", nullptr};
            pipeline.layout = layout_;
            check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipelines_[i]), "pipeline");
        }
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1; pool.poolSizeCount = 3; pool.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(device_, &allocate, &set_), "descriptor set");
        // The lookup tables: uploaded by the first evaluate from a staging buffer that lives as long as the upscaler (about 180 KB).
        area_ = make_image(VK_FORMAT_R8G8_UNORM, {AREATEX_WIDTH, AREATEX_HEIGHT}, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        search_ = make_image(VK_FORMAT_R8_UNORM, {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = AREATEX_SIZE + SEARCHTEX_SIZE; buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device_, &buffer, nullptr, &staging_), "staging buffer");
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device_, staging_, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &allocation, nullptr, &staging_memory_), "staging memory");
        check(vkBindBufferMemory(device_, staging_, staging_memory_, 0), "bind staging");
        void* mapped = nullptr; check(vkMapMemory(device_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped), "map staging");
        std::memcpy(mapped, areaTexBytes, AREATEX_SIZE); std::memcpy(static_cast<char*>(mapped) + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
        vkUnmapMemory(device_, staging_memory_);
        write_image(5, area_.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(6, search_.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
        for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & properties) == properties) return i;
        throw std::runtime_error("SMAA: required memory type unavailable");
    }
    Image make_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
        Image result;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
        info.extent = {extent.width, extent.height, 1}; info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &info, nullptr, &result.image), "image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_, result.image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &allocate, nullptr, &result.memory), "image memory");
        check(vkBindImageMemory(device_, result.image, result.memory, 0), "bind image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &view, nullptr, &result.view), "image view");
        return result;
    }
    void destroy_image(Image& image) {
        if (image.view) vkDestroyImageView(device_, image.view, nullptr);
        if (image.image) vkDestroyImage(device_, image.image, nullptr);
        if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
        image = {};
    }
    void write_image(uint32_t binding, VkImageView view, VkDescriptorType type) {
        const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set_; write.dstBinding = binding; write.descriptorCount = 1; write.descriptorType = type; write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    void resize(VkExtent2D region) {
        check(vkDeviceWaitIdle(device_), "idle before resize");   // the set and the images may be used by frames in flight (render-scale change only)
        destroy_image(source_); destroy_image(edges_); destroy_image(weights_);
        source_ = make_image(VK_FORMAT_R8G8B8A8_UNORM, region, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        edges_ = make_image(VK_FORMAT_R8G8_UNORM, region, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
        weights_ = make_image(VK_FORMAT_R8G8B8A8_UNORM, region, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
        write_image(2, source_.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); write_image(3, edges_.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(4, weights_.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(7, edges_.view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); write_image(8, weights_.view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        extent_ = region;
    }
    void bind_output(VkImageView view) {
        if (bound_output_) check(vkDeviceWaitIdle(device_), "idle before rebinding");   // a new EFB after a rebuild
        write_image(9, view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        bound_output_ = view;
    }
    void upload_lookups(VkCommandBuffer command) {
        barrier(command, area_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier(command, search_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copies[2]{};
        copies[0].bufferOffset = 0; copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copies[0].imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};
        copies[1].bufferOffset = AREATEX_SIZE; copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copies[1].imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
        vkCmdCopyBufferToImage(command, staging_, area_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[0]);
        vkCmdCopyBufferToImage(command, staging_, search_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[1]);
        barrier(command, area_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        barrier(command, search_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        lookup_uploaded_ = true;
    }
    static void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &info);
    }
    static void memory_barrier(VkCommandBuffer command) {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount = 1; info.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &info);
    }
    void destroy() {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        destroy_image(source_); destroy_image(edges_); destroy_image(weights_); destroy_image(area_); destroy_image(search_);
        if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
        if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        for (auto pipeline : pipelines_) if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        for (auto module : modules_) if (module) vkDestroyShaderModule(device_, module, nullptr);
        if (linear_) vkDestroySampler(device_, linear_, nullptr);
        if (point_) vkDestroySampler(device_, point_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    VkDevice device_;
    VkPhysicalDeviceMemoryProperties memory_;
    VkShaderModule modules_[3]{};
    VkPipeline pipelines_[3]{};
    VkSampler linear_=VK_NULL_HANDLE, point_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    Image source_, edges_, weights_, area_, search_;
    VkBuffer staging_=VK_NULL_HANDLE; VkDeviceMemory staging_memory_=VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkImageView bound_output_=VK_NULL_HANDLE;
    bool lookup_uploaded_=false;
};
}
std::unique_ptr<Upscaler> make_smaa_upscaler(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders) {
    return std::make_unique<SmaaUpscaler>(device, memory, shaders);
}
}
