#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "native_rt_effects.h"
#include "rt_ao_math.h"
#include "rt_shadow_math.h"
#include "rt_light_math.h"
#include "rt_reflect_math.h"
#include "rt_path_math.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
using Resource=ComPtr<ID3D12Resource>;
void check(HRESULT result,const char* action) {
    if (FAILED(result)) throw std::runtime_error(std::string("DX12 RT effects: ")+action+" failed ("+std::to_string(uint32_t(result))+")");
}
void transition(ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    if (before==after) return;
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; command->ResourceBarrier(1,&b);
}
void uav_barrier(ID3D12GraphicsCommandList* command,ID3D12Resource* resource=nullptr) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource=resource; command->ResourceBarrier(1,&b);
}
struct Image {
    Resource resource; RenderExtent extent{}; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
struct BaseParams {
    float viewport[4],view_a[4],view_b[2],depth_range[2]; uint32_t region[2],signal_size[2];
};
struct ShadowParams { BaseParams base; uint32_t divisor,frame,sun_rays,local_lights; float sun_cone,light_radius,ao_radius,trace_lamps,overhead[4],overhead_cone,overhead_reach,ao_strength,ao_rays; };
struct ReflectParams { BaseParams base; uint32_t divisor,frame,rays; float cone,reach,strength; uint32_t mode; float padding; };
struct PathParams { BaseParams base; uint32_t divisor_frame,counts; float ao_radius,ao_strength,reach,emission,ambient,key,overhead[4],mix,threshold; uint32_t record_count; float clamp_radiance; };
struct DenoiseParams { uint32_t size[2],region[2]; float depth[2],depth_range[2]; uint32_t signal,flags,step,first,max_history; };
static_assert(sizeof(BaseParams)==64 && sizeof(ShadowParams)==128 && sizeof(ReflectParams)==96 && sizeof(PathParams)==128 && sizeof(DenoiseParams)==52);
BaseParams base_params(const NativeRtInputs& in,RenderExtent signal) {
    BaseParams p{}; std::copy_n(in.viewport.begin(),4,p.viewport);
    p.view_a[0]=in.view.x_scale; p.view_a[1]=in.view.y_scale; p.view_a[2]=in.view.x_offset; p.view_a[3]=in.view.y_offset;
    p.view_b[0]=in.view.depth_a; p.view_b[1]=in.view.depth_b; p.depth_range[0]=in.viewport[4]; p.depth_range[1]=in.viewport[5];
    p.region[0]=in.region.width; p.region[1]=in.region.height; p.signal_size[0]=signal.width; p.signal_size[1]=signal.height; return p;
}
RenderExtent signal_extent(RenderExtent region,uint32_t divisor) { return {(region.width+divisor-1)/divisor,(region.height+divisor-1)/divisor}; }
struct EffectBank {
    Resource upload,sums,list;
    ComPtr<ID3D12DescriptorHeap> heap,clear_heap;
    uint64_t bytes=0;
    uint32_t descriptors=0,last_frame=0;
    bool pooled=false;
};
}
struct NativeRtWork::Impl {
    std::shared_ptr<EffectBank> bank; // Allocation storage only: idle banks never retain scene/source/history pins.
    std::vector<Resource> resources;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12DescriptorHeap> clear_heap;
    std::shared_ptr<NativeRtScene::Frame> scene;
    Resource upload;
};
struct NativeRtEffects::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    std::map<std::string,ComPtr<ID3D12PipelineState>> pipelines;
    std::map<std::string,std::shared_ptr<Image>> images;
    struct History { std::shared_ptr<Image> history[2],moments[2],guide[2],filter[2]; uint32_t next=0; bool valid=false; EffectLevel level=EffectLevel::off; };
    History histories[4];
    std::array<EffectLevel,3> shadow_levels{};
    EffectLevel path_level=EffectLevel::off;
    uint32_t descriptor_size=0;
    uint32_t last_frame=0;
    RenderExtent last_region{},last_extent{};
    bool ran=false;
    std::vector<std::shared_ptr<EffectBank>> banks;
    struct Call {
        Impl& owner; const NativeRtInputs& in; std::shared_ptr<NativeRtWork::Impl> work;
        ID3D12GraphicsCommandList* command;
        uint32_t descriptor_next=0,descriptor_capacity=0,raw_base=0;
        uint64_t upload_next=0,upload_size=0;
        std::byte* mapped=nullptr;
        uint64_t records_offset=0,lights_offset=0;
        ~Call() { if (mapped) work->upload->Unmap(0,nullptr); }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t i) const { auto p=work->heap->GetCPUDescriptorHandleForHeapStart(); p.ptr+=SIZE_T(i)*owner.descriptor_size; return p; }
        D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t i) const { auto p=work->heap->GetGPUDescriptorHandleForHeapStart(); p.ptr+=UINT64(i)*owner.descriptor_size; return p; }
        uint64_t put(const void* data,uint64_t bytes,uint64_t alignment=256) {
            const uint64_t offset=(upload_next+alignment-1)/alignment*alignment;
            if (offset+bytes>upload_size) throw std::runtime_error("DX12 RT effects: upload arena exhausted");
            if (bytes) std::memcpy(mapped+offset,data,bytes); upload_next=offset+bytes; return offset;
        }
        void keep(ID3D12Resource* resource) { if (resource) work->resources.emplace_back(resource); }
        void state(const std::shared_ptr<Image>& image,D3D12_RESOURCE_STATES state) {
            transition(command,image->resource.Get(),image->state,state); image->state=state; work->images.push_back(image);
        }
        void srv(uint32_t at,ID3D12Resource* resource,DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format=format; desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels=1;
            owner.device->CreateShaderResourceView(resource,&desc,cpu(at)); keep(resource);
        }
        void srv(uint32_t at,const NativeUpscaleImage& image) { srv(at,static_cast<ID3D12Resource*>(image.resource),DXGI_FORMAT(image.format)); }
        void srv(uint32_t at,const std::shared_ptr<Image>& image) { state(image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); srv(at,image->resource.Get(),image->format); }
        void structured(uint32_t at,ID3D12Resource* resource,uint64_t offset,uint32_t count,uint32_t stride,bool writable=false) {
            if (writable) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC desc{}; desc.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
                desc.Buffer.FirstElement=offset/stride; desc.Buffer.NumElements=count; desc.Buffer.StructureByteStride=stride;
                owner.device->CreateUnorderedAccessView(resource,nullptr,&desc,cpu(at));
            } else {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
                desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.FirstElement=offset/stride; desc.Buffer.NumElements=count; desc.Buffer.StructureByteStride=stride;
                owner.device->CreateShaderResourceView(resource,&desc,cpu(at));
            }
            keep(resource);
        }
        void uav(uint32_t at,ID3D12Resource* resource,DXGI_FORMAT format) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{}; desc.Format=format; desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            owner.device->CreateUnorderedAccessView(resource,nullptr,&desc,cpu(at)); keep(resource);
        }
        void uav(uint32_t at,const std::shared_ptr<Image>& image) { state(image,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); uav(at,image->resource.Get(),image->format); }
        uint32_t table() {
            if (descriptor_next+19>descriptor_capacity) throw std::runtime_error("DX12 RT effects: descriptor arena exhausted");
            const uint32_t at=descriptor_next; descriptor_next+=19;
            for (uint32_t i=0;i<11;++i) srv(at+i,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
            for (uint32_t i=0;i<8;++i) uav(at+11+i,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
            return at;
        }
        uint32_t common(const std::shared_ptr<Image>& signal,const std::shared_ptr<Image>& color_read,const std::shared_ptr<Image>& filtered={}) {
            const uint32_t at=table();
            D3D12_SHADER_RESOURCE_VIEW_DESC acceleration{}; acceleration.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            acceleration.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; acceleration.RaytracingAccelerationStructure.Location=in.scene->tlas_address();
            owner.device->CreateShaderResourceView(nullptr,&acceleration,cpu(at));
            srv(at+1,in.depth); srv(at+2,in.gbuffer); srv(at+3,in.material);
            structured(at+4,work->upload.Get(),records_offset,uint32_t(in.records.size()),sizeof(RtMaterialRecord));
            structured(at+5,work->upload.Get(),lights_offset,uint32_t(std::max<size_t>(in.lights.size(),1)),sizeof(gx::LightUniform));
            if (filtered) srv(at+6,filtered);
            srv(at+7,color_read); if (in.albedo.resource) srv(at+8,in.albedo);
            if (in.scene->hit_table) structured(at+9,in.scene->hit_table.Get(),0,in.scene->stats.hit_geometries,sizeof(NativeRtHitGeometry));
            if (in.scene->hit_bases) structured(at+10,in.scene->hit_bases.Get(),0,in.scene->stats.instances,4);
            if (!filtered) uav(at+11,signal);
            uav(at+12,static_cast<ID3D12Resource*>(in.color.resource),DXGI_FORMAT(in.color.format));
            return at;
        }
        template<class Params> void dispatch(const char* name,uint32_t table,const Params& params,RenderExtent extent) {
            const uint64_t offset=put(&params,sizeof params);
            auto pipeline=owner.pipelines.at(name); work->pipelines.push_back(pipeline);
            command->SetComputeRootSignature(owner.root.Get()); command->SetPipelineState(pipeline.Get());
            ID3D12DescriptorHeap* heaps[]={work->heap.Get()}; command->SetDescriptorHeaps(1,heaps);
            command->SetComputeRootConstantBufferView(0,work->upload->GetGPUVirtualAddress()+offset);
            command->SetComputeRootDescriptorTable(1,gpu(table)); command->SetComputeRootDescriptorTable(2,gpu(table+11));
            command->SetComputeRootDescriptorTable(3,gpu(raw_base)); command->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);
            uav_barrier(command);        }
        std::shared_ptr<Image> snapshot(const std::string& key) {
            auto image=owner.image(key,in.color.extent,DXGI_FORMAT(in.color.format));
            state(image,D3D12_RESOURCE_STATE_COPY_DEST);
            auto* color=static_cast<ID3D12Resource*>(in.color.resource);
            transition(command,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            command->CopyResource(image->resource.Get(),color);
            transition(command,color,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            state(image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); return image;
        }
        std::shared_ptr<Image> denoise(uint32_t index,const std::shared_ptr<Image>& noisy,uint32_t passes,uint32_t maximum,bool reset) {
            auto& history=owner.histories[index];
            const auto extent=noisy->extent;
            bool resize=!history.history[0] || history.history[0]->extent.width!=extent.width || history.history[0]->extent.height!=extent.height;
            if (resize) {
                history.valid=false;
                for (uint32_t i=0;i<2;++i) {
                    const std::string prefix="denoise"+std::to_string(index)+"_"+std::to_string(i);
                    history.history[i]=owner.image(prefix+"h",extent,DXGI_FORMAT_R16G16B16A16_FLOAT);
                    history.moments[i]=owner.image(prefix+"m",extent,DXGI_FORMAT_R16G16B16A16_FLOAT);
                    history.guide[i]=owner.image(prefix+"g",extent,DXGI_FORMAT_R16G16B16A16_FLOAT);
                    history.filter[i]=owner.image(prefix+"f",extent,DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
            }
            const uint32_t next=history.next,previous=next^1;
            DenoiseParams p{{extent.width,extent.height},{in.region.width,in.region.height},{in.view.depth_a,in.view.depth_b},{in.viewport[4],in.viewport[5]},index==0 ? 0u : 2u,(in.reset || reset || !history.valid) ? 1u : 0u,0,0,maximum};
            uint32_t at=table(); srv(at,noisy); srv(at+1,in.depth); srv(at+2,in.motion); srv(at+3,in.gbuffer);
            uav(at+11,history.history[previous]); uav(at+12,history.moments[previous]); uav(at+13,history.guide[previous]);
            uav(at+14,history.history[next]); uav(at+15,history.moments[next]); uav(at+16,history.guide[next]);
            dispatch("rt_denoise.dx12.temporal.cso",at,p,extent);
            std::shared_ptr<Image> source=history.history[next];
            for (uint32_t pass=0;pass<passes;++pass) {
                auto target=history.filter[pass&1]; at=table();
                uav(at+15,history.moments[next]); uav(at+16,history.guide[next]); uav(at+17,source); uav(at+18,target);
                p.step=1u<<pass; p.first=pass==0; dispatch("rt_denoise.dx12.atrous.cso",at,p,extent); source=target;
            }
            history.next^=1; history.valid=true; state(source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); return source;
        }
    };
    Resource buffer(uint64_t bytes,bool host=false) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=host ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=std::max<uint64_t>(256,(bytes+255)&~uint64_t(255));
        desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags=host ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Resource result;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,host ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result)),"buffer");
        return result;
    }
    std::shared_ptr<EffectBank> acquire_bank(uint32_t frame) {
        std::erase_if(banks,[&](const auto& bank) { return bank.use_count()==1 && uint32_t(frame-bank->last_frame)>120; });
        for (const auto& bank : banks) if (bank.use_count()==1) { bank->last_frame=frame; return bank; }
        auto bank=std::make_shared<EffectBank>(); bank->last_frame=frame;
        if (banks.size()<8) { bank->pooled=true; banks.push_back(bank); }
        return bank;
    }
    Resource ensure_buffer(EffectBank& bank,Resource& resource,uint64_t bytes,bool host=false) {
        if (!bank.pooled) return buffer(bytes,host);
        const uint64_t old=resource ? resource->GetDesc().Width : 0;
        if (old>=bytes && old<=std::max<uint64_t>(bytes*4,65536)) return resource;
        const uint64_t grown=(std::max<uint64_t>({bytes,old>std::max<uint64_t>(bytes*4,65536) ? 0 : old*2,65536})+65535)&~uint64_t(65535);
        uint64_t total=0; for (const auto& retained : banks) total+=retained->bytes;
        bank.bytes-=old; resource.Reset();
        // At most eight banks, 16 MiB each and 128 MiB overall; larger inputs remain submission-local.
        if (bank.bytes+grown>(uint64_t(16)<<20) || total-old+grown>(uint64_t(128)<<20)) return buffer(bytes,host);
        resource=buffer(grown,host); bank.bytes+=resource->GetDesc().Width; return resource;
    }
    std::shared_ptr<Image> image(const std::string& key,RenderExtent extent,DXGI_FORMAT format) {
        auto& result=images[key];
        if (result && result->extent.width==extent.width && result->extent.height==extent.height && result->format==format) return result;
        auto next=std::make_shared<Image>(); next->extent=extent; next->format=format;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=extent.width; desc.Height=extent.height;
        desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=format; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&next->resource)),"image");
        result=next; return result;
    }
};

NativeRtEffects::NativeRtEffects(ID3D12Device* device,const std::filesystem::path& shaders):impl_(std::make_unique<Impl>()) {
    if (!device) throw std::invalid_argument("DX12 RT effects: missing device");
    impl_->device=device; impl_->descriptor_size=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,11,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,8,0,0,0};
    ranges[2]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,1,0};
    D3D12_ROOT_PARAMETER parameters[4]{}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (uint32_t i=0;i<3;++i) { parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[i+1].DescriptorTable={1,&ranges[i]}; }
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=4; desc.pParameters=parameters;
    ComPtr<ID3DBlob> blob,errors; check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"root serialize");
    check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&impl_->root)),"root signature");
    for (const char* name : {"rt_shadow.dx12.trace.cso","rt_shadow.dx12.composite.cso","rt_reflect.dx12.trace.cso","rt_reflect.dx12.composite.cso","rt_path.dx12.accumulate.cso","rt_path.dx12.list.cso","rt_path.dx12.trace.cso","rt_path.dx12.composite.cso","rt_denoise.dx12.temporal.cso","rt_denoise.dx12.atrous.cso","rr_inputs.dx12.inputs.cso"}) {
        std::ifstream file(shaders/name,std::ios::binary); std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
        if (code.empty()) throw std::runtime_error(std::string("DX12 RT effects: missing shader ")+name);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature=impl_->root.Get(); pipeline.CS={code.data(),code.size()};
        check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&impl_->pipelines[name])),name);
    }
}
NativeRtEffects::~NativeRtEffects()=default;

std::shared_ptr<NativeRtWork> NativeRtEffects::execute(const NativeRtInputs& in) {
    if (!in.command || !in.scene || !in.scene->tlas_address() || in.records.empty() || !in.region.width || !in.region.height)
        throw std::invalid_argument("DX12 RT effects: missing scene inputs");
    for (const auto& input : {in.color,in.depth,in.motion,in.gbuffer,in.material,in.albedo})
        if (!input.resource || input.extent.width<in.region.width || input.extent.height<in.region.height) throw std::invalid_argument("DX12 RT effects: invalid image inputs");
    if (in.viewport[2]<=0 || in.viewport[3]<=0 || in.viewport[4]==in.viewport[5] || in.view.x_scale==0 || in.view.y_scale==0)
        throw std::invalid_argument("DX12 RT effects: invalid view inputs");
    if (in.records.size()>UINT32_MAX || in.lights.size()>UINT32_MAX) throw std::invalid_argument("DX12 RT effects: oversized material inputs");
    for (const auto& record : in.records) for (uint32_t i=0;i<8;++i)
        if ((record.light_mask&(1u<<i)) && uint64_t(record.lights_offset)+i>=in.lights.size()) throw std::invalid_argument("DX12 RT effects: material light outside buffer");
    if (!impl_->ran || in.frame!=impl_->last_frame+1 || in.region.width!=impl_->last_region.width || in.region.height!=impl_->last_region.height ||
        in.color.extent.width!=impl_->last_extent.width || in.color.extent.height!=impl_->last_extent.height)
        for (auto& history : impl_->histories) history.valid=false;
    impl_->last_frame=in.frame; impl_->last_region=in.region; impl_->last_extent=in.color.extent; impl_->ran=true;
    auto result=std::make_shared<NativeRtWork>(); result->storage=std::make_shared<NativeRtWork::Impl>();
    auto& work=*result->storage; work.scene=in.scene; work.root=impl_->root; work.bank=impl_->acquire_bank(in.frame);
    if (!in.effects.path_tracing) {
        for (auto* resource : {std::addressof(work.bank->sums),std::addressof(work.bank->list)}) if (*resource) { work.bank->bytes-=(*resource)->GetDesc().Width; resource->Reset(); }
        work.bank->clear_heap.Reset();
    }
    Impl::Call call{*impl_,in,result->storage,in.command};
    call.upload_size=65536+in.records.size()*sizeof(RtMaterialRecord)+std::max<size_t>(in.lights.size(),1)*sizeof(gx::LightUniform)+512;
    work.upload=impl_->ensure_buffer(*work.bank,work.bank->upload,call.upload_size,true); const D3D12_RANGE no_read{0,0};
    void* mapped=nullptr; check(work.upload->Map(0,&no_read,&mapped),"upload map"); call.mapped=static_cast<std::byte*>(mapped);
    call.records_offset=call.put(in.records.data(),in.records.size()*sizeof(RtMaterialRecord),sizeof(RtMaterialRecord));
    gx::LightUniform zero{};
    call.lights_offset=call.put(in.lights.empty() ? &zero : in.lights.data(),std::max<size_t>(in.lights.size(),1)*sizeof(gx::LightUniform),sizeof(gx::LightUniform));
    call.raw_base=2048; call.descriptor_capacity=call.raw_base;
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap.NumDescriptors=call.raw_base+uint32_t(std::max<size_t>(in.scene->raw_buffers.size(),1));
    if (work.bank->heap && work.bank->descriptors>=heap.NumDescriptors && work.bank->descriptors<=uint64_t(heap.NumDescriptors)*4) work.heap=work.bank->heap;
    else {
        heap.NumDescriptors=std::max(heap.NumDescriptors,work.bank->descriptors>uint64_t(heap.NumDescriptors)*4 ? 0u : work.bank->descriptors*2);
        check(impl_->device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&work.heap)),"descriptor heap");
        if (work.bank->pooled && heap.NumDescriptors<=65536) { work.bank->heap=work.heap; work.bank->descriptors=heap.NumDescriptors; }
        else { work.bank->heap.Reset(); work.bank->descriptors=0; }
    }
    for (uint32_t i=0;i<std::max<size_t>(in.scene->raw_buffers.size(),1);++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; desc.Format=DXGI_FORMAT_R32_TYPELESS;
        desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        ID3D12Resource* resource=nullptr;
        if (i<in.scene->raw_buffers.size()) { const auto& raw=in.scene->raw_buffers[i]; resource=raw.resource.Get(); desc.Buffer.FirstElement=raw.offset/4; desc.Buffer.NumElements=UINT(raw.size/4); }
        else desc.Buffer.NumElements=1;
        impl_->device->CreateShaderResourceView(resource,&desc,call.cpu(call.raw_base+i)); call.keep(resource);
    }
    for (const auto& image : {in.color,in.depth,in.motion,in.gbuffer,in.material,in.albedo}) call.keep(static_cast<ID3D12Resource*>(image.resource));
    const auto& effects=in.effects;
    const bool shadow=effects.rt_shadows!=EffectLevel::off || effects.rt_local_shadows!=EffectLevel::off || effects.rt_ambient_occlusion!=EffectLevel::off;
    if (shadow) {
        auto sun=rt_shadow_preset(effects.rt_shadows); auto local=rt_local_preset(effects.rt_local_shadows); auto ao=rt_ao_preset(effects.rt_ambient_occlusion);
        const uint32_t divisor=std::min({sun.divisor ? sun.divisor : UINT32_MAX,local.divisor ? local.divisor : UINT32_MAX,ao.divisor ? ao.divisor : UINT32_MAX});
        const auto extent=signal_extent(in.region,divisor); auto signal=impl_->image("shadow_signal",extent,DXGI_FORMAT_R16_FLOAT); auto color=call.snapshot("shadow_color");
        ShadowParams p{}; p.base=base_params(in,extent); p.divisor=divisor; p.frame=in.frame%8+1;
        p.sun_rays=sun.rays; p.local_lights=local.lights; p.sun_cone=sun.soft ? kRtSunConeRadians : 0; p.light_radius=local.soft ? kRtLightRadiusFraction : 0;
        p.ao_radius=ao.radius; p.ao_strength=ao.strength; p.ao_rays=float(ao.rays);
        if (effects.rt_local_shadows!=EffectLevel::off && !effects.path_tracing) {
            std::copy_n(in.overhead.begin(),3,p.overhead); p.overhead[3]=kRtOverheadStrength;
            p.overhead_cone=local.soft ? kRtOverheadConeRadians : 0; p.overhead_reach=kRtOverheadReach;
        }
        auto table=call.common(signal,color); call.dispatch("rt_shadow.dx12.trace.cso",table,p,extent);
        const std::array<EffectLevel,3> levels{effects.rt_shadows,effects.rt_local_shadows,effects.rt_ambient_occlusion};
        const bool high=*std::max_element(levels.begin(),levels.end())>=EffectLevel::high;
        auto filtered=call.denoise(0,signal,high ? 3 : 2,32,levels!=impl_->shadow_levels); impl_->shadow_levels=levels;
        table=call.common(signal,color,filtered); call.dispatch("rt_shadow.dx12.composite.cso",table,p,in.region);
    } else impl_->histories[0].valid=false;
    if(in.raster_occlusion)in.raster_occlusion();
    if (effects.path_tracing) {
        const auto settings=rt_path_preset(effects.rt_quality); const auto extent=signal_extent(in.region,settings.divisor);
        auto signal=impl_->image("path_signal",extent,DXGI_FORMAT_R16G16B16A16_FLOAT); auto color=call.snapshot("path_color");
        if (!in.scene->hit_table || !in.scene->hit_bases) throw std::invalid_argument("DX12 RT effects: path hit table missing");
        PathParams p{}; p.base=base_params(in,extent); const auto packed=rt_path_pack(settings,in.frame%64+1);
        p.divisor_frame=packed.divisor_frame; p.counts=packed.counts; p.ao_radius=settings.ao_radius; p.ao_strength=settings.ao_strength;
        p.reach=settings.reach; p.emission=settings.emission; p.ambient=settings.ambient; p.key=settings.key;
        std::copy_n(in.overhead.begin(),3,p.overhead); p.overhead[3]=settings.key_reach;
        p.mix=settings.mix; p.threshold=settings.threshold; p.record_count=uint32_t(in.records.size()); p.clamp_radiance=settings.max_radiance;
        auto sums=impl_->ensure_buffer(*work.bank,work.bank->sums,in.records.size()*48);
        auto list=impl_->ensure_buffer(*work.bank,work.bank->list,2048); call.keep(sums.Get()); call.keep(list.Get());
        uint32_t table=call.common(signal,color);
        call.structured(table+13,sums.Get(),0,p.record_count*12,4,true); call.structured(table+14,list.Get(),0,512,4,true);
        const uint32_t zeros[4]{}; ID3D12DescriptorHeap* heaps[]={work.heap.Get()}; in.command->SetDescriptorHeaps(1,heaps);
        D3D12_DESCRIPTOR_HEAP_DESC clear_desc{}; clear_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; clear_desc.NumDescriptors=2;
        if (work.bank->clear_heap) work.clear_heap=work.bank->clear_heap;
        else {
            check(impl_->device->CreateDescriptorHeap(&clear_desc,IID_PPV_ARGS(&work.clear_heap)),"clear descriptor heap");
            if (work.bank->pooled) work.bank->clear_heap=work.clear_heap;
        }
        auto clear_sums=work.clear_heap->GetCPUDescriptorHandleForHeapStart(),clear_list=clear_sums;
        clear_list.ptr+=impl_->descriptor_size;
        const auto clear_table=call.table();
        D3D12_UNORDERED_ACCESS_VIEW_DESC clear_view{}; clear_view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        // D3D12 clears typed/raw UAVs; dispatch keeps its separate structured views.
        clear_view.Format=DXGI_FORMAT_R32_UINT; clear_view.Buffer.NumElements=p.record_count*12;
        impl_->device->CreateUnorderedAccessView(sums.Get(),nullptr,&clear_view,clear_sums);
        impl_->device->CreateUnorderedAccessView(sums.Get(),nullptr,&clear_view,call.cpu(clear_table+11));
        clear_view.Buffer.NumElements=512;
        impl_->device->CreateUnorderedAccessView(list.Get(),nullptr,&clear_view,clear_list);
        impl_->device->CreateUnorderedAccessView(list.Get(),nullptr,&clear_view,call.cpu(clear_table+12));
        in.command->ClearUnorderedAccessViewUint(call.gpu(clear_table+11),clear_sums,sums.Get(),zeros,0,nullptr);
        in.command->ClearUnorderedAccessViewUint(call.gpu(clear_table+12),clear_list,list.Get(),zeros,0,nullptr); uav_barrier(in.command);
        call.dispatch("rt_path.dx12.accumulate.cso",table,p,{(in.region.width+3)/4,(in.region.height+3)/4});
        call.dispatch("rt_path.dx12.list.cso",table,p,{1,1});
        call.dispatch("rt_path.dx12.trace.cso",table,p,extent);
        auto filtered=call.denoise(3,signal,3,16,impl_->path_level!=effects.rt_quality); impl_->path_level=effects.rt_quality;
        table=call.common(signal,color,filtered); call.dispatch("rt_path.dx12.composite.cso",table,p,in.region);
    } else impl_->histories[3].valid=false;
    const auto radiance=[&](EffectLevel level,bool lighting) {
        const uint32_t index=lighting ? 2 : 1; auto& history=impl_->histories[index];
        if (level==EffectLevel::off) { history.valid=false; return; }
        auto preset=rt_reflect_preset(level); float strength=kRtReflectStrength;
        if (lighting) { auto light=rt_light_preset(level); preset={light.divisor,light.rays,0,light.reach}; strength=light.strength; }
        const auto extent=signal_extent(in.region,preset.divisor); const std::string key=lighting ? "lighting" : "reflection";
        auto signal=impl_->image(key+"_signal",extent,DXGI_FORMAT_R16G16B16A16_FLOAT); auto color=call.snapshot(key+"_color");
        ReflectParams p{}; p.base=base_params(in,extent); p.divisor=preset.divisor; p.frame=in.frame%64+1; p.rays=preset.rays;
        p.cone=preset.cone; p.reach=preset.reach; p.strength=strength; p.mode=lighting ? 1 : 0;
        auto table=call.common(signal,color); call.dispatch("rt_reflect.dx12.trace.cso",table,p,extent);
        auto filtered=call.denoise(index,signal,level>=EffectLevel::high ? 3 : 2,lighting ? kRtLightHistory : kRtReflectHistory,history.level!=level); history.level=level;
        table=call.common(signal,color,filtered); call.dispatch("rt_reflect.dx12.composite.cso",table,p,in.region);
    };
    radiance(effects.rt_lighting,true); radiance(effects.rt_reflections,false);
    if (in.ray_reconstruction) {
        auto normal=impl_->image("rr_normal",in.color.extent,DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto specular=impl_->image("rr_specular",in.color.extent,DXGI_FORMAT_R8G8B8A8_UNORM);
        uint32_t table=call.table(); call.srv(table+1,in.depth); call.srv(table+2,in.gbuffer); call.srv(table+3,in.material);
        call.structured(table+4,work.upload.Get(),call.records_offset,uint32_t(in.records.size()),sizeof(RtMaterialRecord));
        call.uav(table+11,normal); call.uav(table+12,specular);
        // RR uses the same first 64 bytes; its final uint2 is padding.
        auto p=base_params(in,{}); call.dispatch("rr_inputs.dx12.inputs.cso",table,p,in.region);
        call.state(normal,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); call.state(specular,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        result->normal_roughness={normal->resource.Get(),uint32_t(normal->format),uint32_t(normal->state),normal->extent};
        result->specular_albedo={specular->resource.Get(),uint32_t(specular->format),uint32_t(specular->state),specular->extent};
    }
    return result;
}
}
