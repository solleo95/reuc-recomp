#pragma once
#include "bloom_math.h"
#include "upscaler.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace uc {
// What the bloom needs of a scene: the finished (upscaled) scene colour in VK_IMAGE_LAYOUT_GENERAL with sampled and storage usage (modified in place in
// SDR), and the scene part of it in pixels.
struct BloomInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color;
    VkExtent2D region{};
    BloomSettings settings;
    bool hdr=false;             // HDR output: the glow stays in the slot's float image for the HDR encode; SDR: it is added to `color`
    float hdr_headroom=1;       // HDR: peak / paper white, what 1.0 in the 8-bit scene counts as at most
    uint32_t slot=0;            // which of the pass's `slots` glow images to write (the renderer uses one: the frame's present reads it before the next frame's bloom)
    uint64_t targets_generation=0;   // VulkanRenderer::targets_generation(): changes when the scene targets were recreated
};
// Bloom (shaders/bloom.slang): a six-level pyramid of the scene (Karis-weighted, inverse tone expanded first level, 13-tap downsamples, tent upsamples), summed
// into a glow at half the region's size. Compute only, no temporal state.
class BloomPass {
public:
    static constexpr uint32_t kLevels=6;
    // slots: the renderer's frame slots. idle: how to wait for the GPU before images or bindings change (vkDeviceWaitIdle when empty).
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle={});
    // Records the pass, or nothing for a switched-off bloom or strength 0 (the slot's glow is then invalid).
    void record(const BloomInputs& inputs);
    // A glow image (by slot): scene-referred linear light (1.0 = paper white) already scaled by the mix, covering the region at glow_extent(). Valid only after an HDR
    // record of that slot; SDR records and invalidate() clear it.
    VkImageView glow_view(uint32_t slot) const { return slot<glow_.size() ? glow_[slot].view : VK_NULL_HANDLE; }
    VkExtent2D glow_extent() const { return level_extent_[0]; }
    uint64_t generation() const { return generation_; }   // changes when the glow images were recreated: descriptor sets that name glow_view() are stale
    bool glow_valid(uint32_t slot) const { return slot<valid_.size() && valid_[slot]; }
    void invalidate(uint32_t slot) { if(slot<valid_.size()) valid_[slot]=false; }
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Image make_image(VkExtent2D extent);
    void destroy_image(Image& image);
    void write(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view);
    void dispatch(VkCommandBuffer command, VkPipeline pipeline, VkDescriptorSet set, const void* params, VkExtent2D size);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[4]{};   // down first, down, up, composite
    VkPipeline pipelines_[4]{};
    VkSampler sampler_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;   // down first, down 1..5, up 4..1, then per slot: up 0 (into the glow) and composite
    std::array<Image,kLevels> level_{};
    std::array<VkExtent2D,kLevels> level_extent_{};
    std::vector<Image> glow_;
    std::vector<bool> valid_;
    VkExtent2D region_{};
    VkImageView bound_color_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0, generation_=0;
    uint32_t slots_=0;
    std::function<void()> idle_;
};
}
