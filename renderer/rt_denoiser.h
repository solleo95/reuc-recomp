#pragma once
#include "rt_denoise_math.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace uc {
// visibility, ambient_occlusion: one channel (red), filtered on depth and normal; radiance: RGB with variance-guided luminance weights as well.
enum class RtSignal : uint32_t { visibility, ambient_occlusion, radiance };
// The scene targets the denoiser is guided by, in VK_IMAGE_LAYOUT_GENERAL (left there): D32 depth, RG16F motion (previous minus current position in
// scene-target pixels) and the normal G-buffer (gbuffer.h), all sampled. The signal covers `region`, the top-left part of the targets that holds the scene.
struct DenoiseInputs {
    VkImageView depth=VK_NULL_HANDLE, motion=VK_NULL_HANDLE, gbuffer=VK_NULL_HANDLE;
    VkExtent2D region{};
    ViewParams view{};                          // the projection the scene was drawn with
    std::array<float,2> depth_range{0.f,1.f};   // the viewport's near and far depth
    uint64_t targets_generation=0;              // VulkanRenderer::targets_generation(): changes when the scene targets were recreated
};
// The own denoiser of a ray-traced signal (docs/superpowers/specs/2026-10-19-ray-tracing-design.md, "Denoising"): temporal accumulation with
// reprojection by the motion vectors and depth/normal rejection (rt_denoise_math.h), then `atrous_iterations` passes of a 5x5 edge-aware à-trous
// filter (shaders/rt_denoise.slang). One instance per signal; it owns that signal's history. Nothing is allocated or recorded until a caller asks.
class RtDenoiser {
public:
    // idle: how to wait for the GPU before images or bindings change (the renderer's presentation backend; vkDeviceWaitIdle when empty).
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle={});
    void resize(VkExtent2D signal_extent);   // drops the history
    // noisy: the signal at the resize() extent, sampled, in VK_IMAGE_LAYOUT_GENERAL, written before this call with its own barrier. Pass the same view
    // every frame: a new one rebinds (and waits for the GPU). Returns the filtered signal (RGBA16F, VK_IMAGE_LAYOUT_GENERAL), valid until the next call.
    VkImageView denoise(VkCommandBuffer command, RtSignal signal, VkImageView noisy, const DenoiseInputs& inputs, uint32_t atrous_iterations);
    void reset() { reset_=true; }            // camera cut: the next frame starts a new history
    VkImage output_image() const { return output_; }   // tests/tools: the image behind the view denoise() returned last
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    Image make_image(VkExtent2D extent);
    void destroy_image(Image& image);
    void write_sets();
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[2]{};   // temporal, à-trous
    VkPipeline pipelines_[2]{};
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    // [parity][step]: the frame parity picks the history pair; step 0 = temporal and the first filter pass (history -> filtered[0]), 1 = filtered[0] ->
    // filtered[1], 2 = filtered[1] -> filtered[0].
    VkDescriptorSet sets_[2][3]{};
    Image history_[2], moments_[2], guide_[2], filtered_[2];   // ping-pong by frame parity (history, moments, guide) or by filter pass
    VkExtent2D extent_{};
    uint32_t parity_=0;
    bool reset_=true, initialized_=false;   // initialized_: the images were moved to VK_IMAGE_LAYOUT_GENERAL
    VkImageView bound_noisy_=VK_NULL_HANDLE, bound_depth_=VK_NULL_HANDLE, bound_motion_=VK_NULL_HANDLE, bound_gbuffer_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0;
    VkImage output_=VK_NULL_HANDLE;
    std::function<void()> idle_;
};
}
