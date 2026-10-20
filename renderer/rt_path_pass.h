#pragma once
#include "graphics_effects.h"
#include "rt_path_math.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace uc {
// What the path tracer needs of a scene: the frame's TLAS and hit table (RtScene::hit_table/hit_bases) and the internal scene targets in VK_IMAGE_LAYOUT_GENERAL (D32 depth,
// the G-buffer, the R32_UINT material-record index per pixel, the RGBA8 colour with storage usage and the RT albedo target) and the frame's material records.
struct RtPathInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas=VK_NULL_HANDLE;
    VkImageView depth=VK_NULL_HANDLE, gbuffer=VK_NULL_HANDLE, material=VK_NULL_HANDLE, color=VK_NULL_HANDLE, albedo=VK_NULL_HANDLE;
    VkBuffer records=VK_NULL_HANDLE, hit_table=VK_NULL_HANDLE, hit_bases=VK_NULL_HANDLE;
    uint32_t record_count=0;
    VkExtent2D region{};
    std::array<float,6> viewport{};
    ViewParams view{};
    std::array<float,3> overhead{0.f,1.f,0.f};   // unit direction toward the synthetic overhead light, view space
    RtPathSettings settings;
    uint32_t frame=0;                     // noise frame
    uint32_t slot=0;                      // the frame slot: its descriptor sets are rewritten here
};
// The path-tracing pass (shaders/rt_path.slang): accumulate() takes the records' mean albedo, trace() writes the gathered light per signal pixel (RGBA16F at 1/divisor
// of the scene resolution), the caller denoises it (RtDenoiser, RtSignal::radiance) and composite() replaces the lit surfaces' colour with albedo x light.
// Nothing is allocated until the first accumulate().
class RtPathPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle={});
    VkImageView trace(const RtPathInputs& inputs);   // accumulates the albedo, then traces; returns the noisy signal
    VkExtent2D signal_extent() const { return signal_extent_; }
    VkImage signal_image() const { return signal_.image; }
    void composite(const RtPathInputs& inputs, VkImageView filtered);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    void write_set(VkDescriptorSet set, const RtPathInputs& inputs, VkImageView signal_in);
    void ensure_signal(VkExtent2D extent);
    void ensure_sums(uint32_t records);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[3]{};   // accumulate, trace, composite
    VkPipeline pipelines_[3]{};
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,16> trace_sets_{}, composite_sets_{};
    uint32_t slots_=0;
    Image signal_;
    VkExtent2D signal_extent_{};
    VkBuffer sums_=VK_NULL_HANDLE; VkDeviceMemory sums_memory_=VK_NULL_HANDLE; uint32_t sums_capacity_=0;   // 8 words per record: the albedo's sums, the pixel count, the shown colour's sums
    bool signal_initialized_=false;
    std::function<void()> idle_;
};
}
