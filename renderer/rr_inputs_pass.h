#pragma once
#include "upscaler.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <filesystem>
#include <functional>

namespace uc {
// The guides Ray Reconstruction takes besides what the renderer has (shaders/rr_inputs.slang): normal + roughness (RGBA16F) and specular albedo (RGBA8) at the scene extent.
struct RrInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkImageView depth=VK_NULL_HANDLE, gbuffer=VK_NULL_HANDLE, material=VK_NULL_HANDLE;   // sampled, VK_IMAGE_LAYOUT_GENERAL
    VkBuffer records=VK_NULL_HANDLE;
    VkExtent2D region{}, extent{};   // the scene region and the scene targets' extent
    std::array<float,6> viewport{};
    ViewParams view{};
    uint32_t slot=0;
};
class RrInputsPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle={});
    // Writes both images (left in VK_IMAGE_LAYOUT_GENERAL) and returns them for the upscaler's tags.
    void record(const RrInputs& inputs);
    UpscaleImage normal_roughness() const { return {normal_.image,normal_.view,VK_FORMAT_R16G16B16A16_SFLOAT,extent_,kUsage}; }
    UpscaleImage specular_albedo() const { return {specular_.image,specular_.view,VK_FORMAT_R8G8B8A8_UNORM,extent_,kUsage}; }
    void shutdown();
private:
    static constexpr VkImageUsageFlags kUsage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    Image make_image(VkExtent2D extent, VkFormat format);
    void destroy_image(Image& image);
    void ensure(VkExtent2D extent);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule module_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,64> sets_{};
    uint32_t slots_=0;
    Image normal_, specular_;
    VkExtent2D extent_{};
    bool initialized_=false;
    std::function<void()> idle_;
};
}
