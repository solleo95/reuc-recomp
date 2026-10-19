#pragma once
#include "graphics_effects.h"
#include "upscaler.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace uc {
// What the blur needs of a frame: the finished (upscaled) colour in VK_IMAGE_LAYOUT_GENERAL with storage and transfer usage (modified in place), the
// scene's motion vectors in GENERAL (previous minus current, scene-target pixels), and both region sizes.
struct MotionBlurInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color, motion;
    VkExtent2D output_size{}, render_size{};   // the scene region in output pixels and in scene-target pixels
    EffectLevel level=EffectLevel::off;
    uint64_t targets_generation=0;            // VulkanRenderer::targets_generation()
};
// The shutter fraction and the sample count of a preset.
struct BlurPreset { float shutter; uint32_t samples; };
BlurPreset blur_preset(EffectLevel level);
// Per-object motion blur (shaders/motion_blur.slang): a tile-max of the vectors, then a gather along each pixel's (or its neighbourhood's) vector from a
// copy of the colour. Compute only.
class MotionBlurPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle={});
    void record(const MotionBlurInputs& inputs);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Image make_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage);
    void destroy_image(Image& image);
    void write_image(uint32_t binding, VkImageView view, VkDescriptorType type);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[2]{};   // tile, blur
    VkPipeline pipelines_[2]{};
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    Image source_, tiles_;
    VkExtent2D source_size_{}, tiles_size_{};
    VkImageView bound_color_=VK_NULL_HANDLE, bound_motion_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0;
    std::function<void()> idle_;
};
}
