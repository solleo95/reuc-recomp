#pragma once
#include "graphics_effects.h"
#include "scene_lights.h"
#include "upscaler.h"
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace uc {
// Where the light is on screen, in pixels of the scene targets: a directional light's far point is its direction. None when the light is behind the
// camera or more than one screen width past an edge (its shafts would point nowhere useful).
inline std::optional<std::array<float,2>> light_screen_position(const gx::CollectedLight& light, const ViewParams& view, const std::array<float,6>& viewport) {
    auto v=light.position;
    if (light.kind==gx::LightKind::directional) {
        const float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if (length<=0) return std::nullopt;
        for (auto& c:v) c/=length;
    }
    if (v[2]>=-1e-3f) return std::nullopt;
    const float ndc_x=(view.x_scale*v[0]+view.x_offset*v[2])/-v[2], ndc_y=(view.y_scale*v[1]+view.y_offset*v[2])/-v[2];
    if (std::abs(ndc_x)>2 || std::abs(ndc_y)>2) return std::nullopt;
    return std::array<float,2>{viewport[0]+(ndc_x*0.5f+0.5f)*viewport[2],viewport[1]+(ndc_y*0.5f+0.5f)*viewport[3]};
}
struct VolumetricInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color;                    // RGBA8 with storage usage, VK_IMAGE_LAYOUT_GENERAL (modified in place)
    VkExtent2D region{};                   // the top-left part of the target that holds the scene
    std::array<float,6> viewport{};        // x, y, width, height in target pixels, then depth range
    ViewParams view{};
    EffectLevel level=EffectLevel::off;
    std::optional<gx::CollectedLight> light;
    uint64_t targets_generation=0;
};
struct ShaftPreset { uint32_t divisor, samples; float intensity; };
ShaftPreset shaft_preset(EffectLevel level);
// Screen-space light shafts (shaders/volumetric_shafts.slang). Compute only. Returns false when nothing was recorded.
class VolumetricPass {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, std::function<void()> idle={});
    bool record(const VolumetricInputs& inputs);
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; bool initialized=false; };
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Image make_image(VkExtent2D extent);
    void destroy_image(Image& image);
    void write_image(uint32_t binding, VkImageView view, VkDescriptorType type);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkShaderModule modules_[2]{};   // shafts, composite
    VkPipeline pipelines_[2]{};
    VkSampler sampler_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    Image shafts_;
    VkExtent2D shafts_size_{};
    VkImageView bound_color_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0;
    std::function<void()> idle_;
};
}
