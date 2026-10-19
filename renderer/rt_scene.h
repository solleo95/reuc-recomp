#pragma once
#include "view_reconstruct.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace uc {
// Whether a scene draw becomes a ray tracing instance: the shadow-caster rule (a mesh triangle list with indices, perspective, writing depth). nullopt: not an
// instance; true: opaque; false: an alpha cut-out (an alpha compare other than "always" in pixel[0], GX alpha compare word), non-opaque geometry.
inline std::optional<bool> rt_instance_opaque(bool mesh, bool has_indices, bool perspective, bool z_write, uint32_t alpha_word, VkPrimitiveTopology topology) {
    if (!mesh || !has_indices || !perspective || !z_write || topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) return std::nullopt;
    return ((alpha_word>>16)&7)==7 && ((alpha_word>>19)&7)==7;
}
// One instance of a packet's BLAS. vertices: the draw's gx::GpuMeshVertex buffer at its vertex offset (vertex first_vertex + i is the draw's vertex i);
// indices: its index buffer at its index offset (the draw's indices start at first_index). palettes: the slot's gx::PaletteBlock buffer, palette/current/
// indexed as the draw's push constants. positions_out is assigned by RtScene::record_packet (callers leave it 0).
struct RtGeometry {
    VkDeviceAddress vertices=0, indices=0, positions_out=0;
    uint32_t vertex_count=0, index_count=0, first_index=0;
    int32_t first_vertex=0;
    uint32_t palette=0, current=0, indexed=0;
    bool opaque=true;
    VkDeviceAddress palettes=0;
};
// What the debug probe (UC_RT_PROBE) needs of the scene: the depth target in VK_IMAGE_LAYOUT_GENERAL and how to reconstruct view positions from it.
struct RtProbeInputs {
    VkImageView depth=VK_NULL_HANDLE;
    VkExtent2D region{};
    std::array<float,6> viewport{};   // x, y, width, height in target pixels, then the viewport's near and far depth
    ViewParams view{};
    std::array<float,3> to_light{};   // unit view-space direction to the dominant directional light
};
// Per-frame acceleration structures (docs/superpowers/plans/2026-10-19-ray-tracing-p0-1.md, Ruling 1): every scene packet's instances go through a transform pass
// into view space and one BLAS in that packet's command buffer; the scene boundary builds the TLAS over the frame's BLASes (identity transforms, view space).
// Buffers live per frame-in-flight slot, grow geometrically and never shrink.
class RtScene {
public:
    void initialize(VkDevice device, VkPhysicalDevice physical, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t frames_in_flight);
    bool initialized() const { return device_!=VK_NULL_HANDLE; }
    void begin_frame(uint32_t frame_slot);   // the slot's previous frame has completed on the GPU: its BLASes are dropped, its buffers reused
    void record_packet(VkCommandBuffer command, std::span<const RtGeometry> geometries);
    bool build_tlas(VkCommandBuffer command);   // false when the frame has no BLAS (nothing recorded)
    void record_probe(VkCommandBuffer command, const RtProbeInputs& inputs);   // after build_tlas returned true
    uint32_t probe_occluded() const;   // the count of the latest probe (read once its submission completed)
    VkAccelerationStructureKHR tlas() const;
    uint64_t buffer_growths() const { return growths_; }   // tests: how often a buffer was (re)allocated
    struct Stats { uint32_t blas=0, geometries=0; uint64_t triangles=0; };
    Stats stats() const { return frame_ ? frame_->stats : Stats{}; }
    void shutdown();
private:
    struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkDeviceAddress address=0; void* mapped=nullptr; VkDeviceSize size=0; };
    struct Frame {
        Buffer positions, scratch, blas_storage, tlas_storage, instances;
        VkDeviceSize blas_used=0;
        std::vector<VkAccelerationStructureKHR> blas;
        std::vector<VkDeviceAddress> blas_addresses;
        VkAccelerationStructureKHR tlas=VK_NULL_HANDLE;
        std::vector<Buffer> retired;   // replaced by a growth during the frame: earlier packets of the frame may still read them
        VkDescriptorSet probe_set=VK_NULL_HANDLE;
        Stats stats;
    };
    bool ensure(Frame& frame, Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host);   // true when it was (re)allocated
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void destroy(Buffer& buffer);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkDeviceSize scratch_alignment_=128;
    PFN_vkCreateAccelerationStructureKHR create_as_=nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_as_=nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes_=nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build_=nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address_=nullptr;
    VkShaderModule transform_module_=VK_NULL_HANDLE, probe_module_=VK_NULL_HANDLE;
    VkPipelineLayout transform_layout_=VK_NULL_HANDLE, probe_layout_=VK_NULL_HANDLE;
    VkPipeline transform_=VK_NULL_HANDLE, probe_=VK_NULL_HANDLE;
    VkDescriptorSetLayout probe_set_layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    Buffer counters_;   // the probe's occluded-pixel counter, one per frame slot at kCounterStride
    std::vector<Frame> frames_;
    Frame* frame_=nullptr;
    uint32_t slot_=0, probe_slot_=UINT32_MAX;
    uint64_t growths_=0;
};
}
