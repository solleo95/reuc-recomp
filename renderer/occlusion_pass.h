#pragma once
#include "graphics_effects.h"
#include "raster_effects_math.h"
#include "scene_lights.h"
#include "shadow_math.h"
#include "upscaler.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace uc {
// The shadow map of this frame (renderer/shadow_map.h, in VK_IMAGE_LAYOUT_GENERAL) and what the composite needs to read it.
struct ShadowInputs { VkImageView map=VK_NULL_HANDLE; uint64_t generation=0; ShadowFit fit{}; ShadowPreset preset{}; std::array<float,3> to_light{}; };
// What the screen-space occlusion needs of a scene: the internal scene targets in VK_IMAGE_LAYOUT_GENERAL (left there), the scene viewport in pixels of
// those targets and the projection the scene was drawn with.
struct OcclusionInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color, depth, gbuffer;   // color: RGBA8 with storage usage (modified in place); depth: D32 sampled; gbuffer: gbuffer.h texels, sampled
    VkExtent2D region{};                  // the top-left part of the targets that holds the scene
    std::array<float,6> viewport{};       // x, y, width, height in target pixels, then the viewport's near and far depth
    ViewParams view{};
    EffectLevel ao=EffectLevel::off;
    bool contact=false;                   // contact shadows toward `light` (nothing happens without one)
    std::optional<gx::CollectedLight> light;  // the scene's dominant directional or spot light
    std::optional<ShadowInputs> shadow;   // shadow maps: multiplied into the colour together with the occlusion
    uint32_t slot=0;                      // the frame slot: picks this frame's shadow parameter block (frames overlap on the GPU)
    uint64_t targets_generation=0;       // VulkanRenderer::targets_generation(): changes when the scene targets were recreated
    uint32_t noise_frame=0;             // frame index for the noise pattern; 0 keeps it fixed (a temporal upscaler is what averages it)
};
// Ground-truth ambient occlusion on the scene colour (shaders/gtao.slang): a horizon search at the preset's resolution, a depth-aware blur, and a
// composite that multiplies the colour of every pixel a depth-writing draw covered, faded by its GX fog. Compute only.
class OcclusionPass {
public:
    // idle: how to wait for the GPU before images or bindings change (the renderer's presentation backend; vkDeviceWaitIdle when empty).
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle={});
    void record(const OcclusionInputs& inputs);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    void create_shadow_block();
    Image make_image(VkFormat format, VkExtent2D extent);
    void destroy_image(Image& image);
    void write_image(uint32_t binding, VkImageView view, VkDescriptorType type);
    void resize(VkExtent2D ao_size, VkExtent2D contact_size);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[4]{};   // ao, denoise, composite, contact
    VkPipeline pipelines_[4]{};
    VkSampler sampler_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    Image raw_, blurred_, contact_;
    VkExtent2D ao_size_{}, contact_size_{};
    VkImageView bound_depth_=VK_NULL_HANDLE, bound_gbuffer_=VK_NULL_HANDLE, bound_color_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0, bound_shadow_generation_=0;
    VkImageView bound_shadow_=VK_NULL_HANDLE;
    VkBuffer shadow_block_=VK_NULL_HANDLE; VkDeviceMemory shadow_block_memory_=VK_NULL_HANDLE; void* shadow_block_mapped_=nullptr;   // one 256-byte block per frame slot
    std::function<void()> idle_;
};
}
