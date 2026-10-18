#pragma once
#include <vulkan/vulkan.h>
#include <filesystem>

namespace uc {
// RCAS sharpening of the upscaled scene region of an RGBA8 image in VK_IMAGE_LAYOUT_GENERAL (left there). The DLSS path and the test blit
// upscaler use it; FSR applies the same filter inside its own dispatch.
class RcasPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders);
    // `constant` = rcas_constant(sharpness); region = top-left width x height of `image`.
    void record(VkCommandBuffer command, VkImage image, VkImageView view, VkExtent2D region, float constant);
    void shutdown();
private:
    void resize(VkExtent2D extent);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule module_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    VkImage copy_=VK_NULL_HANDLE; VkDeviceMemory copy_memory_=VK_NULL_HANDLE; VkImageView copy_view_=VK_NULL_HANDLE;
    VkExtent2D copy_extent_{};
    VkImageView bound_target_=VK_NULL_HANDLE;
};
}
