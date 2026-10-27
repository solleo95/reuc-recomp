#pragma once
#include "rt_scene.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>

namespace uc {
struct NativeRtGeometry {
    RtGeometry geometry;
    ID3D12Resource* vertex_buffer=nullptr;
    ID3D12Resource* index_buffer=nullptr;
    ID3D12Resource* palette_buffer=nullptr;
};
struct NativeRtHitGeometry {
    uint32_t positions_buffer=0, positions_word=0, indices_buffer=0, indices_word=0;
    uint32_t record=~0u, stride_words=3, flags=0, padding=0;
};
static_assert(sizeof(NativeRtHitGeometry)==32);
class NativeRtScene {
    struct Bank;
public:
    using Resource=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct RawBuffer { Resource resource; uint64_t offset=0, size=0; };
    struct Frame {
        Resource tlas, hit_table, hit_bases;
        std::vector<RawBuffer> raw_buffers;
        RtScene::Stats stats;
        D3D12_GPU_VIRTUAL_ADDRESS tlas_address() const { return tlas ? tlas->GetGPUVirtualAddress() : 0; }
    private:
        friend class NativeRtScene;
        std::shared_ptr<Bank> bank;
        struct RawRange { uint32_t index; uint64_t address,size; };
        std::unordered_map<ID3D12Resource*,RawRange> raw_indices;
        std::vector<Resource> retained;
        std::vector<NativeRtHitGeometry> hits;
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        std::vector<uint32_t> bases;
    };
    static bool supported(ID3D12Device* device, std::string& reason);
    void initialize(ID3D12Device* device, const std::filesystem::path& shaders);
    bool initialized() const { return bool(device_); }
    std::shared_ptr<Frame> begin_frame(uint64_t frame_number);
    void record_packet(ID3D12GraphicsCommandList4* command, std::span<const NativeRtGeometry> geometries);
    bool build_tlas(ID3D12GraphicsCommandList4* command);
    std::shared_ptr<Frame> frame() const { return frame_; }
    RtScene::Stats stats() const { return frame_ ? frame_->stats : RtScene::Stats{}; }
    RtScene::CacheStats cache_stats() const;
    uint64_t buffer_growths() const { return growths_; }
    void invalidate_mesh(uint64_t mesh_id);
    void shutdown();
private:
    struct Cached { Resource storage, vertices, indices; };
    Resource buffer(uint64_t size, D3D12_RESOURCE_STATES state, bool upload=false, bool pooled=true);
    Resource upload(const void* data, uint64_t size);
    Resource build(ID3D12GraphicsCommandList4*, std::span<const D3D12_RAYTRACING_GEOMETRY_DESC>, bool cached);
    const Frame::RawRange& raw(ID3D12Resource* resource);
    NativeRtHitGeometry hit(const NativeRtGeometry&, ID3D12Resource* positions, uint64_t address, uint32_t stride);
    void instance(Resource blas, const std::array<std::array<float,4>,3>& matrix, uint32_t mask, uint32_t base);
    Microsoft::WRL::ComPtr<ID3D12Device5> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> transform_root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> transform_;
    std::shared_ptr<Frame> frame_;
    std::vector<std::shared_ptr<Bank>> banks_;
    ID3D12GraphicsCommandList4* command_=nullptr;
    RtMeshCachePolicy policy_{uint64_t(256)<<20,16};
    std::unordered_map<uint64_t,Cached> cached_;
    uint64_t invalidated_=0, deferred_=0, growths_=0;
};
}
