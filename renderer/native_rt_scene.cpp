#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "native_rt_scene.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace uc {
namespace {
void check(HRESULT result, const char* action) {
    if (FAILED(result)) throw std::runtime_error(std::string("DX12 ray tracing: ")+action+" failed ("+std::to_string(uint32_t(result))+")");
}
void barrier(ID3D12GraphicsCommandList4* command, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource=resource;
    command->ResourceBarrier(1,&b);
}
D3D12_RAYTRACING_GEOMETRY_DESC triangles(const RtGeometry& g, uint64_t positions, uint64_t stride) {
    D3D12_RAYTRACING_GEOMETRY_DESC desc{};
    desc.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    desc.Flags=g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    desc.Triangles.VertexBuffer={positions,stride}; desc.Triangles.VertexCount=g.vertex_count;
    desc.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
    desc.Triangles.IndexBuffer=g.indices+uint64_t(g.first_index)*4;
    desc.Triangles.IndexCount=(g.index_count/3)*3; desc.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
    return desc;
}
constexpr std::array<std::array<float,4>,3> identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
}
struct NativeRtScene::Bank {
    struct Block { Resource resource; D3D12_RESOURCE_STATES state; };
    std::vector<Block> blocks[3]; // upload, AS storage, UAV/position storage
    size_t used[3]{};
    uint64_t bytes=0;
    uint64_t last_frame=0;
    bool pooled=false;
    Block* latest=nullptr;
};
bool NativeRtScene::supported(ID3D12Device* device, std::string& reason) {
    if (!device) { reason="DX12 device unavailable"; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof options)) || options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1) {
        reason="DXR 1.1 required"; return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof model)) || model.HighestShaderModel<D3D_SHADER_MODEL_6_5) {
        reason="Shader Model 6.5 required"; return false;
    }
    reason.clear(); return true;
}
void NativeRtScene::initialize(ID3D12Device* device, const std::filesystem::path& shaders) {
    if (device_) return;
    std::string reason; if (!supported(device,reason)) throw std::runtime_error("DX12 ray tracing: "+reason);
    check(device->QueryInterface(IID_PPV_ARGS(&device_)),"ID3D12Device5");
    try {
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants={0,0,5};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[1].Descriptor.ShaderRegister=0;
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[2].Descriptor.ShaderRegister=1;
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[3].Descriptor.ShaderRegister=0;
        D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters=4; root.pParameters=parameters;
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"transform root serialization");
        check(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&transform_root_)),"transform root");
        std::ifstream file(shaders/"rt_transform.dx12.main.cso",std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
        if (code.empty()) throw std::runtime_error("DX12 ray tracing: rt_transform.dx12.main.cso missing");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=transform_root_.Get(); desc.CS={code.data(),code.size()};
        check(device_->CreateComputePipelineState(&desc,IID_PPV_ARGS(&transform_)),"transform pipeline");
    } catch (...) { shutdown(); throw; }
}
NativeRtScene::Resource NativeRtScene::buffer(uint64_t size, D3D12_RESOURCE_STATES state, bool host, bool pooled) {
    Bank::Block* block=nullptr;
    Bank* bank=pooled && frame_ && frame_->bank->pooled ? frame_->bank.get() : nullptr;
    uint64_t previous_bytes=0;
    if (bank) {
        bank->latest=nullptr;
        const size_t kind=host ? 0 : state==D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE ? 1 : 2;
        const size_t index=bank->used[kind]++;
        auto& blocks=bank->blocks[kind];
        if (index<blocks.size()) block=&blocks[index];
        else if (index==blocks.size() && bank->blocks[0].size()+bank->blocks[1].size()+bank->blocks[2].size()<4096) {
            blocks.push_back({{},state}); block=&blocks.back();
        }
        if (block && block->resource) {
            previous_bytes=block->resource->GetDesc().Width;
            if (previous_bytes>=size && previous_bytes<=std::max<uint64_t>(size*4,65536)) {
                if (block->state!=state) {
                    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition={block->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,block->state,state};
                    command_->ResourceBarrier(1,&barrier); block->state=state;
                }
                bank->latest=block;
                return block->resource;
            }
        }
    }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=host ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=std::max<uint64_t>(256,(size+255)&~uint64_t(255));
    if (block) {
        const bool shrinking=previous_bytes>std::max<uint64_t>(size*4,65536);
        const uint64_t grown=(std::max<uint64_t>({desc.Width,shrinking ? 0 : previous_bytes*2,65536})+65535)&~uint64_t(65535);
        uint64_t pooled_bytes=0;
        for (const auto& retained : banks_) pooled_bytes+=retained->bytes;
        if (bank->bytes-previous_bytes+grown<=(uint64_t(64)<<20) && pooled_bytes-previous_bytes+grown<=(uint64_t(256)<<20)) desc.Width=grown;
        else block=nullptr; // Oversized/excess work remains frame-local, keeping idle bank storage bounded.
    }
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags=host ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Resource resource;
    check(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"buffer");
    if (block) { bank->bytes=bank->bytes-previous_bytes+desc.Width; *block={resource,state}; bank->latest=block; }
    ++growths_; return resource;
}
NativeRtScene::Resource NativeRtScene::upload(const void* data, uint64_t size) {
    auto resource=buffer(size,D3D12_RESOURCE_STATE_GENERIC_READ,true);
    void* mapped=nullptr; const D3D12_RANGE read{0,0}; check(resource->Map(0,&read,&mapped),"upload map");
    if (size) std::memcpy(mapped,data,size);
    resource->Unmap(0,nullptr); frame_->retained.push_back(resource); return resource;
}
std::shared_ptr<NativeRtScene::Frame> NativeRtScene::begin_frame(uint64_t number) {
    if (!device_) throw std::runtime_error("DX12 ray tracing: scene uninitialized");
    frame_.reset();
    // Drop idle level peaks; pinned banks remain alive until their renderer fence owners release them.
    std::erase_if(banks_,[&](const auto& bank) { return bank.use_count()==1 && number>bank->last_frame && number-bank->last_frame>120; });
    auto frame=std::make_shared<Frame>();
    for (const auto& bank : banks_) if (bank.use_count()==1) { frame->bank=bank; break; }
    if (!frame->bank) {
        frame->bank=std::make_shared<Bank>();
        if (banks_.size()<8) { frame->bank->pooled=true; banks_.push_back(frame->bank); }
    }
    for (size_t kind=0;kind<3;++kind) {
        auto& blocks=frame->bank->blocks[kind];
        for (size_t i=frame->bank->used[kind];i<blocks.size();++i) if (blocks[i].resource) frame->bank->bytes-=blocks[i].resource->GetDesc().Width;
        blocks.resize(std::min(blocks.size(),frame->bank->used[kind]));
    }
    std::fill(std::begin(frame->bank->used),std::end(frame->bank->used),0);
    frame->bank->last_frame=number;
    frame->bank->latest=nullptr;
    frame_=std::move(frame); policy_.begin_frame(number); return frame_;
}
const NativeRtScene::Frame::RawRange& NativeRtScene::raw(ID3D12Resource* resource) {
    if (!resource) throw std::runtime_error("DX12 ray tracing: missing buffer resource");
    if (const auto found=frame_->raw_indices.find(resource);found!=frame_->raw_indices.end()) return found->second;
    const auto desc=resource->GetDesc();
    if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER) throw std::runtime_error("DX12 ray tracing: missing buffer resource");
    if (desc.Width/4>UINT32_MAX) throw std::runtime_error("DX12 ray tracing: raw buffer exceeds descriptor range");
    const Frame::RawRange range{uint32_t(frame_->raw_buffers.size()),resource->GetGPUVirtualAddress(),desc.Width};
    frame_->raw_buffers.push_back({resource,0,desc.Width});
    return frame_->raw_indices.emplace(resource,range).first->second;
}
NativeRtHitGeometry NativeRtScene::hit(const NativeRtGeometry& item, ID3D12Resource* positions, uint64_t address, uint32_t stride) {
    const auto word=[](const Frame::RawRange& range,uint64_t at) {
        if (at<range.address || at-range.address>=range.size || (at&3))
            throw std::runtime_error("DX12 ray tracing: buffer address outside resource");
        return uint32_t((at-range.address)/4);
    };
    const auto& position_range=raw(positions); const uint32_t position_word=word(position_range,address);
    const auto& index_range=raw(item.index_buffer);
    return {position_range.index,position_word,index_range.index,word(index_range,item.geometry.indices+uint64_t(item.geometry.first_index)*4),item.geometry.record,stride};
}
NativeRtScene::Resource NativeRtScene::build(ID3D12GraphicsCommandList4* command, std::span<const D3D12_RAYTRACING_GEOMETRY_DESC> geometries, bool cached) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL; input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.Flags=cached ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    input.NumDescs=uint32_t(geometries.size()); input.pGeometryDescs=geometries.data();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{}; device_->GetRaytracingAccelerationStructurePrebuildInfo(&input,&sizes);
    if (!sizes.ResultDataMaxSizeInBytes || !sizes.ScratchDataSizeInBytes) throw std::runtime_error("DX12 ray tracing: invalid BLAS sizes");
    auto result=buffer(sizes.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,false,!cached);
    auto scratch=buffer(sizes.ScratchDataSizeInBytes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    frame_->retained.push_back(result); frame_->retained.push_back(scratch);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{}; desc.Inputs=input;
    desc.DestAccelerationStructureData=result->GetGPUVirtualAddress(); desc.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    command->BuildRaytracingAccelerationStructure(&desc,0,nullptr); barrier(command,result.Get()); return result;
}
void NativeRtScene::instance(Resource blas, const std::array<std::array<float,4>,3>& matrix, uint32_t mask, uint32_t base) {
    D3D12_RAYTRACING_INSTANCE_DESC desc{}; std::memcpy(desc.Transform,matrix.data(),sizeof desc.Transform);
    desc.InstanceID=uint32_t(frame_->instances.size()); desc.InstanceMask=mask;
    desc.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE; desc.AccelerationStructure=blas->GetGPUVirtualAddress();
    frame_->instances.push_back(desc); frame_->bases.push_back(base); frame_->retained.push_back(blas); ++frame_->stats.instances;
}
void NativeRtScene::record_packet(ID3D12GraphicsCommandList4* command, std::span<const NativeRtGeometry> geometries) {
    if (!frame_ || !command) throw std::runtime_error("DX12 ray tracing: record outside frame");
    command_=command;
    std::vector<NativeRtGeometry> dynamic[2];
    for (const auto& item : geometries) {
        const auto& g=item.geometry;
        if (!g.vertex_count || g.index_count<3) continue;
        if (g.skinned || !g.mesh_id) { dynamic[g.skinned ? 1 : 0].push_back(item); continue; }
        if (!g.opaque) continue;
        // Validate and retain the source buffers even on cache hits: hit shading reads them.
        const auto entry=hit(item,item.vertex_buffer,g.vertices,27);
        const uint64_t key=RtMeshCachePolicy::make_key(g.mesh_id,g.opaque);
        const auto action=policy_.request(key);
        if (action==RtMeshCachePolicy::Action::defer) { ++deferred_; continue; }
        if (action==RtMeshCachePolicy::Action::build) {
            const auto geometry=triangles(g,g.vertices,108);
            auto result=build(command,std::span(&geometry,1),true);
            cached_.emplace(key,Cached{result,item.vertex_buffer,item.index_buffer});
            for (auto evicted : policy_.commit(key,result->GetDesc().Width)) cached_.erase(evicted);
        }
        const uint32_t base=uint32_t(frame_->hits.size()); frame_->hits.push_back(entry);
        instance(cached_.at(key).storage,g.transform,kRtStatic,base); ++frame_->stats.static_instances;
    }
    for (uint32_t group=0;group<2;++group) {
        auto& items=dynamic[group]; if (items.empty()) continue;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descriptions;
        const uint32_t base=uint32_t(frame_->hits.size());
        command->SetComputeRootSignature(transform_root_.Get()); command->SetPipelineState(transform_.Get());
        std::vector<D3D12_RESOURCE_BARRIER> transitions;   // one barrier call for the group: a barrier per item drains the GPU between the small dispatches
        for (const auto& item : items) {
            const auto& g=item.geometry;
            if (!item.vertex_buffer || !item.index_buffer || !item.palette_buffer || !g.vertices || !g.indices || !g.palettes)
                throw std::runtime_error("DX12 ray tracing: missing transform source");
            auto positions=buffer(uint64_t(g.vertex_count)*12,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            frame_->retained.push_back(positions); frame_->retained.emplace_back(item.vertex_buffer); frame_->retained.emplace_back(item.index_buffer); frame_->retained.emplace_back(item.palette_buffer);
            struct Push { uint32_t count; int32_t first; uint32_t palette,current,indexed; };
            const Push push{g.vertex_count,g.first_vertex,g.palette,g.current,g.indexed};
            command->SetComputeRoot32BitConstants(0,5,&push,0); command->SetComputeRootShaderResourceView(1,g.vertices);
            command->SetComputeRootShaderResourceView(2,g.palettes); command->SetComputeRootUnorderedAccessView(3,positions->GetGPUVirtualAddress());
            command->Dispatch((g.vertex_count+63)/64,1,1);
            D3D12_RESOURCE_BARRIER transition{}; transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition={positions.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            transitions.push_back(transition);
            if (auto* block=frame_->bank->latest) block->state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            descriptions.push_back(triangles(g,positions->GetGPUVirtualAddress(),12));
            frame_->hits.push_back(hit(item,positions.Get(),positions->GetGPUVirtualAddress(),3)); frame_->stats.triangles+=g.index_count/3;
        }
        command->ResourceBarrier(UINT(transitions.size()),transitions.data());
        auto blas=build(command,descriptions,false); instance(blas,identity,group ? kRtSkinned : kRtStatic,base);
        ++frame_->stats.blas; if (group) ++frame_->stats.skinned_blas; frame_->stats.geometries+=uint32_t(items.size());
    }
}
bool NativeRtScene::build_tlas(ID3D12GraphicsCommandList4* command) {
    if (!frame_ || frame_->instances.empty()) return false;
    command_=command;
    auto instances=upload(frame_->instances.data(),frame_->instances.size()*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    frame_->hit_table=upload(frame_->hits.data(),frame_->hits.size()*sizeof(NativeRtHitGeometry));
    frame_->hit_bases=upload(frame_->bases.data(),frame_->bases.size()*sizeof(uint32_t));
    frame_->stats.hit_geometries=uint32_t(frame_->hits.size()); frame_->stats.hit_records=0;
    for (const auto& h : frame_->hits) frame_->stats.hit_records+=h.record!=~0u;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    input.NumDescs=uint32_t(frame_->instances.size()); input.InstanceDescs=instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{}; device_->GetRaytracingAccelerationStructurePrebuildInfo(&input,&sizes);
    if (!sizes.ResultDataMaxSizeInBytes || !sizes.ScratchDataSizeInBytes) throw std::runtime_error("DX12 ray tracing: invalid TLAS sizes");
    auto tlas=buffer(sizes.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch=buffer(sizes.ScratchDataSizeInBytes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    frame_->retained.push_back(tlas); frame_->retained.push_back(scratch);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{}; desc.Inputs=input;
    desc.DestAccelerationStructureData=tlas->GetGPUVirtualAddress(); desc.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    command->BuildRaytracingAccelerationStructure(&desc,0,nullptr); barrier(command,tlas.Get()); frame_->tlas=tlas; return true;
}
void NativeRtScene::invalidate_mesh(uint64_t mesh) { for (auto key : policy_.invalidate(mesh)) { cached_.erase(key); ++invalidated_; } }
RtScene::CacheStats NativeRtScene::cache_stats() const {
    return {policy_.builds_requested(),policy_.hits(),policy_.evictions(),invalidated_,deferred_,policy_.bytes(),uint32_t(policy_.resident())};
}
void NativeRtScene::shutdown() {
    frame_.reset(); banks_.clear(); command_=nullptr; cached_.clear(); transform_.Reset(); transform_root_.Reset(); device_.Reset();
    policy_=RtMeshCachePolicy{uint64_t(256)<<20,16}; invalidated_=deferred_=growths_=0;
}
}
