#pragma once
#include "hdr_math.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <vector>

namespace uc {
// HDR10 encode (shaders/hdr.slang): an RGBA8 SDR picture, plus optionally a premultiplied RGBA8 overlay layer of the target's size, drawn
// into an A2B10G10R10 target as PQ / BT.2020. Used for the present into the HDR swapchain and for frame generation's HUD-less copy.
// The bloom's glow (BloomPass::glow_view, GENERAL layout) and which part of it the source rectangle shows: the glow's texture coordinates at the
// target's top-left corner and the span of the whole target. A null view means no glow.
struct BloomLayer { VkImageView view=VK_NULL_HANDLE; float uv_origin[2]{}, uv_size[2]{1,1}; };
class HdrPass {
public:
    static constexpr VkFormat kFormat=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    void initialize(VkDevice device, const std::filesystem::path& shaders);
    // `source` and `overlay` (VK_NULL_HANDLE: none) in VK_IMAGE_LAYOUT_GENERAL; `target` in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
    // origin/size: the source rectangle, scaled (nearest) over the whole target.
    void record(VkCommandBuffer command, VkImageView source, VkOffset2D origin, VkExtent2D size, VkImageView overlay,
                VkImageView target, VkExtent2D extent, const HdrSettings& settings, const BloomLayer& bloom={});
    void forget();   // the source views are about to be destroyed; the caller made sure no submitted frame still uses their sets
    void shutdown();
private:
    struct Bound { VkImageView source, overlay, glow; VkDescriptorSet set; };
    VkDevice device_=VK_NULL_HANDLE;
    VkSampler sampler_=VK_NULL_HANDLE;   // linear, clamped: the glow is half resolution
    VkShaderModule vertex_=VK_NULL_HANDLE, fragment_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    std::vector<Bound> bound_;   // one set per (source, overlay) pair: the slots' images and the EFB
};
}
