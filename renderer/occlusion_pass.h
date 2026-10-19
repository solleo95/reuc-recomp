#pragma once
#include "graphics_effects.h"
#include "upscaler.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>

namespace uc {
// What the screen-space occlusion needs of a scene: the internal scene targets in VK_IMAGE_LAYOUT_GENERAL (left there), the scene viewport in pixels of
// those targets and the projection the scene was drawn with.
struct OcclusionInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color, depth, gbuffer;   // color: RGBA8 with storage usage (modified in place); depth: D32 sampled; gbuffer: gbuffer.h texels, sampled
    VkExtent2D region{};                  // the top-left part of the targets that holds the scene
    std::array<float,6> viewport{};       // x, y, width, height in target pixels, then the viewport's near and far depth
    ViewParams view{};
    EffectLevel ao=EffectLevel::off;
    uint32_t noise_frame=0;               // frame index for the noise pattern; 0 keeps it fixed (a temporal upscaler is what averages it)
};
// The slices, steps and resolution of a preset.
struct AoPreset { uint32_t slices, steps; bool full_resolution; };
AoPreset ao_preset(EffectLevel level);
// Ground-truth ambient occlusion on the scene colour (shaders/gtao.slang): a horizon search at the preset's resolution, a depth-aware blur, and a
// composite that multiplies the colour of every pixel a depth-writing draw covered, faded by its GX fog. Compute only.
class OcclusionPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders);
    void record(const OcclusionInputs& inputs);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Image make_image(VkFormat format, VkExtent2D extent);
    void destroy_image(Image& image);
    void write_image(uint32_t binding, VkImageView view, VkDescriptorType type);
    void resize(VkExtent2D ao_size);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[3]{};
    VkPipeline pipelines_[3]{};
    VkSampler sampler_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    Image raw_, blurred_;
    VkExtent2D ao_size_{};
    VkImageView bound_depth_=VK_NULL_HANDLE, bound_gbuffer_=VK_NULL_HANDLE, bound_color_=VK_NULL_HANDLE;
};
}
