#include "rcas_pass.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("RCAS: ") + what + " failed: VkResult " + std::to_string(result)); }
void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = before; barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &info);
}
struct Constants { int32_t width, height; float sharpness, unused; };
}
void RcasPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders) {
    if (pipeline_) return;
    device_ = device; memory_ = memory;
    std::ifstream file(shaders / "rcas.comp.spv", std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    if (code.empty() || code.size() % 4) throw std::runtime_error("RCAS: shader rcas.comp.spv missing or invalid");
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = code.size(); module.pCode = reinterpret_cast<const uint32_t*>(code.data());
    check(vkCreateShaderModule(device_, &module, nullptr, &module_), "shader module");
    const VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = 2; set.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_), "descriptor layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "pipeline layout");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module_, "main", nullptr};
    pipeline.layout = layout_;
    check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "pipeline");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "descriptor pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &set_layout_;
    check(vkAllocateDescriptorSets(device_, &allocate, &set_), "descriptor set");
}
void RcasPass::resize(VkExtent2D extent) {
    check(vkDeviceWaitIdle(device_), "idle before resize");   // the set and the copy may be used by frames in flight (render-scale change only)
    if (copy_view_) vkDestroyImageView(device_, copy_view_, nullptr);
    if (copy_) vkDestroyImage(device_, copy_, nullptr);
    if (copy_memory_) vkFreeMemory(device_, copy_memory_, nullptr);
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {extent.width, extent.height, 1}; image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(device_, &image, nullptr, &copy_), "copy image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_, copy_, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) throw std::runtime_error("RCAS: no device-local memory");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
    check(vkAllocateMemory(device_, &allocate, nullptr, &copy_memory_), "copy memory");
    check(vkBindImageMemory(device_, copy_, copy_memory_, 0), "bind copy");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = copy_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &view, nullptr, &copy_view_), "copy view");
    copy_extent_ = extent; bound_target_ = VK_NULL_HANDLE;
}
void RcasPass::record(VkCommandBuffer command, VkImage image, VkImageView view, VkExtent2D region, float constant) {
    if (region.width != copy_extent_.width || region.height != copy_extent_.height) resize(region);
    if (bound_target_ != view) {
        const VkDescriptorImageInfo source{VK_NULL_HANDLE, copy_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, target{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = writes[1].dstSet = set_; writes[0].descriptorCount = writes[1].descriptorCount = 1;
        writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &source;
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &target;
        if (bound_target_) check(vkDeviceWaitIdle(device_), "idle before rebinding");   // a new EFB after a rebuild
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        bound_target_ = view;
    }
    image_barrier(command, copy_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.extent = {region.width, region.height, 1};
    vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_GENERAL, copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(command, copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);   // the copy read finishes before the shader writes
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    const Constants constants{int32_t(region.width), int32_t(region.height), constant, 0};
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof constants, &constants);
    vkCmdDispatch(command, (region.width + 7) / 8, (region.height + 7) / 8, 1);
    image_barrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}
void RcasPass::shutdown() {
    if (!device_) return;
    if (copy_view_) vkDestroyImageView(device_, copy_view_, nullptr);
    if (copy_) vkDestroyImage(device_, copy_, nullptr);
    if (copy_memory_) vkFreeMemory(device_, copy_memory_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (module_) vkDestroyShaderModule(device_, module_, nullptr);
    *this = RcasPass{};
}
}
