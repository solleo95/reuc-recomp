#pragma once
#include "graphics_effects.h"
#include "rt_shadow_math.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace uc {
// What the ray-traced shadows need of a scene: the frame's TLAS (RtScene::tlas(), built before this pass) and the internal scene targets in
// VK_IMAGE_LAYOUT_GENERAL: D32 depth, the G-buffer, the R32_UINT material-record index per pixel and the RGBA8 colour (storage usage, modified by composite()),
// plus the frame's material records and lights (RtMaterialRecord at `index`, the draw's 8 GX lights at index*8).
struct RtShadowInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas=VK_NULL_HANDLE;
    VkImageView depth=VK_NULL_HANDLE, gbuffer=VK_NULL_HANDLE, material=VK_NULL_HANDLE, color=VK_NULL_HANDLE;
    VkBuffer records=VK_NULL_HANDLE, lights=VK_NULL_HANDLE;
    VkExtent2D region{};                  // the top-left part of the targets that holds the scene
    std::array<float,6> viewport{};       // x, y, width, height in target pixels, then the viewport's near and far depth
    ViewParams view{};
    EffectLevel sun=EffectLevel::off, local=EffectLevel::off;   // the rt_shadows and rt_local_shadows levels
    bool level_light=false;               // trace the synthetic overhead light and skip the headlamp (rt_shadow_math.h): the level's shadows; renderer: rt_local_shadows is on
    uint32_t frame=0;                     // noise frame for the soft-shadow jitter (0 keeps it fixed)
    uint32_t slot=0;                      // the frame slot: its descriptor sets are rewritten here (its previous frame has completed)
};
// The visibility pass of the ray-traced shadows (shaders/rt_shadow.slang): trace() writes one factor per signal pixel (1/divisor of the scene resolution), the
// caller denoises it (RtDenoiser, RtSignal::visibility) and composite() multiplies the scene colour by the filtered factor. Nothing is allocated until the
// first trace(); descriptor sets are per frame slot, the signal image is shared (resized behind idle).
class RtShadowPass {
public:
    // idle: how to wait for the GPU before the signal image changes (the renderer's presentation backend; vkDeviceWaitIdle when empty).
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle={});
    // Returns the noisy factor (R16F, VK_IMAGE_LAYOUT_GENERAL, written with a barrier after it); the divisor comes from the active presets.
    VkImageView trace(const RtShadowInputs& inputs);
    VkExtent2D signal_extent() const { return signal_extent_; }
    VkImage signal_image() const { return signal_.image; }
    uint32_t signal_images_created() const { return images_created_; }   // tests: how often the signal image was (re)created
    // After trace(): `filtered` is the denoiser's output (sampled, GENERAL). Same inputs as the trace.
    void composite(const RtShadowInputs& inputs, VkImageView filtered);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    void write_set(VkDescriptorSet set, const RtShadowInputs& inputs, VkImageView signal_in);
    void ensure_signal(VkExtent2D extent);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[2]{};   // trace, composite
    VkPipeline pipelines_[2]{};
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,16> trace_sets_{}, composite_sets_{};
    uint32_t slots_=0;
    Image signal_;
    VkExtent2D signal_extent_{};
    uint32_t divisor_=1, images_created_=0;
    bool signal_initialized_=false;
    std::function<void()> idle_;
};
}
