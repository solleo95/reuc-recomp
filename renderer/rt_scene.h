#pragma once
#include "rt_mesh_cache.h"
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
    uint32_t record=~0u;   // the draw's material record (rt_material.h), ~0u without one: what a ray that hits this geometry looks up (RtHitGeometry)
    bool skinned=false;   // per-vertex matrices (a character or an enemy): its instance carries kRtSkinned, a static draw's kRtStatic
    // A static draw of a cached mesh (mesh_id != 0, not skinned) is not transformed per frame: its object-space BLAS is built once (RtMeshCachePolicy) straight from
    // the mesh's vertex/index buffers (`vertices` addresses gx::GpuMeshVertex records) and the TLAS instance carries `transform`, the draw's object-to-view rows.
    uint64_t mesh_id=0;
    std::array<std::array<float,4>,3> transform{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
};
// What a ray that hit a triangle of a BLAS geometry needs to know about it (the path tracer, docs/superpowers/plans/2026-10-20-path-tracing-spike.md). std430, 32 bytes.
// positions/indices: device addresses of the geometry's vertex positions (float3 at `stride_words` words apart: 3 for a per-frame view-space buffer, 27 for a cached
// gx::GpuMeshVertex buffer in object space) and of its triangle-list indices; the instance's object-to-world rows bring the positions to view space.
// The table is the frame's flat list in build order; `bases` gives, per TLAS instance (instanceCustomIndex), where its geometries start, so the entry of a hit is
// table[bases[instance] + geometry index].
struct RtHitGeometry { VkDeviceAddress positions=0, indices=0; uint32_t record=~0u, stride_words=3, flags=0, padding=0; };
static_assert(sizeof(RtHitGeometry)==32);
// Instance masks of the two occluder classes (docs/superpowers/specs/2026-10-19-rt-level-shadows-design.md): a lamp's ray sees both, a rig light's only the skinned one.
constexpr uint32_t kRtStatic=1, kRtSkinned=2;
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
    VkAccelerationStructureKHR tlas() const;
    uint64_t buffer_growths() const { return growths_; }   // tests: how often a buffer was (re)allocated
    // The latest frame: blas/skinned_blas/geometries/triangles are the per-frame view-space builds (skinned draws), static_instances the instances of cached BLASes,
    // instances the TLAS total.
    struct Stats { uint32_t blas=0, skinned_blas=0, static_instances=0, instances=0, geometries=0, hit_geometries=0, hit_records=0; uint64_t triangles=0; };   // hit_*: entries of the hit table (all, and those with a record)
    VkBuffer hit_table() const { return frame_ ? frame_->hits.buffer : VK_NULL_HANDLE; }     // RtHitGeometry per geometry; valid after build_tlas
    VkBuffer hit_bases() const { return frame_ ? frame_->bases.buffer : VK_NULL_HANDLE; }    // uint32 per TLAS instance: its first table entry
    VkDeviceSize hit_table_size() const { return frame_ ? frame_->hits.size : 0; }
    VkDeviceSize hit_bases_size() const { return frame_ ? frame_->bases.size : 0; }
    Stats stats() const { return frame_ ? frame_->stats : Stats{}; }
    struct CacheStats { uint64_t builds=0, hits=0, evictions=0, invalidated=0, deferred=0, bytes=0; uint32_t resident=0; };
    CacheStats cache_stats() const;
    void invalidate_mesh(uint64_t mesh_id);   // the mesh was freed: its cached BLASes are retired (destroyed once no frame in flight can use them)
    void shutdown();
private:
    struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkDeviceAddress address=0; void* mapped=nullptr; VkDeviceSize size=0; };
    struct Frame {
        Buffer positions, scratch, blas_storage, tlas_storage, instances, hits, bases;
        std::vector<RtHitGeometry> hit_geometries;   // the hit table in build order (copied to `hits` with the TLAS)
        std::vector<uint32_t> blas_hit_base;         // per entry of `blas`: the table index of its first geometry
        VkDeviceSize blas_used=0;
        std::vector<VkAccelerationStructureKHR> blas;
        std::vector<VkDeviceAddress> blas_addresses;
        std::vector<uint32_t> blas_masks;   // the instance mask of each BLAS (kRtStatic or kRtSkinned)
        struct StaticInstance { std::array<std::array<float,4>,3> matrix; VkDeviceAddress blas; uint32_t hit_base; };
        std::vector<StaticInstance> static_instances;   // this frame's instances of cached BLASes
        VkAccelerationStructureKHR tlas=VK_NULL_HANDLE;
        std::vector<Buffer> retired;   // replaced by a growth during the frame: earlier packets of the frame may still read them
        Stats stats;
    };
    void build_blas(VkCommandBuffer command, std::span<const RtGeometry> items, uint32_t mask);   // one BLAS over `items` (positions_out assigned), after their positions were allocated
    struct CachedBlas { VkAccelerationStructureKHR as=VK_NULL_HANDLE; Buffer storage; VkDeviceAddress address=0; };
    void build_cached(VkCommandBuffer command, const RtGeometry& item, uint64_t key);   // the object-space BLAS of a static mesh, straight from its buffers
    void retire(uint64_t key);   // out of the cache; destroyed once no frame in flight can use it
    bool ensure(Frame& frame, Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host);   // true when it was (re)allocated
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool count=true);
    void destroy(Buffer& buffer);
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkDeviceSize scratch_alignment_=128;
    PFN_vkCreateAccelerationStructureKHR create_as_=nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_as_=nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes_=nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build_=nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address_=nullptr;
    VkShaderModule transform_module_=VK_NULL_HANDLE;
    VkPipelineLayout transform_layout_=VK_NULL_HANDLE;
    VkPipeline transform_=VK_NULL_HANDLE;
    std::vector<Frame> frames_;
    Frame* frame_=nullptr;
    uint32_t slot_=0;
    uint64_t frame_number_=0;
    static constexpr uint64_t kMeshCacheBytes=uint64_t(256)<<20;   // the byte budget of the cached BLASes
    static constexpr uint32_t kBuildsPerFrame=16;                  // BLAS builds of cache misses per frame; the rest wait (their draws do not occlude meanwhile)
    RtMeshCachePolicy policy_{kMeshCacheBytes,kBuildsPerFrame};
    std::unordered_map<uint64_t,CachedBlas> cached_;
    struct Retired { CachedBlas blas; uint64_t frame; };
    std::vector<Retired> retired_;
    uint64_t invalidated_=0, deferred_=0;
    uint64_t growths_=0;
};
}
