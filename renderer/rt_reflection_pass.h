#pragma once
#include "graphics_effects.h"
#include "rt_light_math.h"
#include "rt_reflect_math.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace uc {
// What the ray-traced reflections need of a scene: the frame's TLAS and the internal scene targets in VK_IMAGE_LAYOUT_GENERAL (D32 depth, the G-buffer, the R32_UINT
// material-record index per pixel, the RGBA8 colour with storage usage, modified by composite()) and the frame's material records (reflectivity in their flags).
// The pass traces two effects with the same machinery (shaders/rt_reflect.slang): the reflections and the lighting (one diffuse bounce, plan p6); one instance per effect.
enum class RtRadianceMode : uint32_t { reflections, lighting };
struct RtReflectInputs {
    RtRadianceMode mode=RtRadianceMode::reflections;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas=VK_NULL_HANDLE;
    VkImageView depth=VK_NULL_HANDLE, gbuffer=VK_NULL_HANDLE, material=VK_NULL_HANDLE, color=VK_NULL_HANDLE, albedo=VK_NULL_HANDLE;   // albedo: the lighting composite's reflectance
    VkBuffer records=VK_NULL_HANDLE;
    VkExtent2D region{};
    std::array<float,6> viewport{};
    ViewParams view{};
    EffectLevel level=EffectLevel::off;
    uint32_t frame=0;                     // noise frame for the roughness cone
    uint32_t slot=0;                      // the frame slot: its descriptor sets are rewritten here
};
// The reflection pass (shaders/rt_reflect.slang): trace() writes the reflected radiance per signal pixel (RGBA16F at 1/divisor of the scene resolution), the caller
// denoises it (RtDenoiser, RtSignal::radiance) and composite() lerps the scene colour toward the filtered radiance. Nothing is allocated until the first trace().
class RtReflectionPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle={});
    VkImageView trace(const RtReflectInputs& inputs);
    VkExtent2D signal_extent() const { return signal_extent_; }
    VkImage signal_image() const { return signal_.image; }
    void composite(const RtReflectInputs& inputs, VkImageView filtered);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    void write_set(VkDescriptorSet set, const RtReflectInputs& inputs, VkImageView signal_in);
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
    uint32_t divisor_=1;
    bool signal_initialized_=false;
    std::function<void()> idle_;
};
}
