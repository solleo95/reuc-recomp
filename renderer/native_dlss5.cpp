#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "native_dlss5.h"
#include "dlss5_math.h"
#include "public_release.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
struct Constants { int32_t source_size[2],target_size[2],source_origin[2],target_origin[2]; float gain,white; int32_t encoding,nr_encoding,domain,filter,residual,unused; };
static_assert(sizeof(Constants)==64);
void check(HRESULT value,const char* what) {
    if(FAILED(value)) throw std::runtime_error(std::string("DX12 DLSS 5: ")+what+" failed: "+std::to_string(uint32_t(value)));
}
std::vector<char> shader(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),{});
    if(bytes.empty()) throw std::runtime_error("DX12 DLSS 5 shader missing: "+path.string()); return bytes;
}
void transition(ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES& current,D3D12_RESOURCE_STATES next) {
    if(current==next) return;
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,current,next}; command->ResourceBarrier(1,&barrier); current=next;
}
void memory_barrier(ID3D12GraphicsCommandList* command) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; command->ResourceBarrier(1,&barrier);
}
NgxImage ngx_image(const NativeUpscaleImage& image,bool writable=false) {
    NgxImage result; result.native_resource=image.resource; result.extent={image.extent.width,image.extent.height}; result.read_write=writable; return result;
}
}
struct NativeDlss5Pass::Work {
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> prepare,resolve;
    std::vector<ComPtr<ID3D12Resource>> resources;
};
struct NativeDlss5Pass::Impl {
    struct Image { ComPtr<ID3D12Resource> resource; D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON; RenderExtent extent{}; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; };
    NativeDlss5Context context;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> prepare,resolve;
    std::unique_ptr<Dlss5Runtime> runtime;
    std::array<Image,3> bridge;
    Image snapshot;
    std::vector<std::shared_ptr<Work>> pool;
    std::string status;
    bool failed=false,open_failed=false,ran=false;
    void fail(const std::string& reason) {
        failed=true; ran=false;
        if(status!=reason) std::fprintf(stderr,"DX12 DLSS 5 unavailable: %s\n",reason.c_str()); status=reason;
    }
    void pipelines() {
        if(root) return;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,16};
        for(size_t i=0;i<2;++i) { parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[i+1].DescriptorTable={1,&ranges[i]}; }
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=3; desc.pParameters=parameters;
        ComPtr<ID3DBlob> bytes,error; check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error),"serialize bridge root");
        check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)),"bridge root");
        const auto first=shader(context.shaders/"dlss5.dx12.prepare.cso"),last=shader(context.shaders/"dlss5.dx12.resolve.cso");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature=root.Get(); pipeline.CS={first.data(),first.size()};
        check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&prepare)),"prepare pipeline");
        pipeline.CS={last.data(),last.size()}; check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&resolve)),"resolve pipeline");
    }
    Image image(RenderExtent extent,DXGI_FORMAT format) {
        Image result; result.extent=extent; result.format=format;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=extent.width; desc.Height=extent.height;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.Format=format; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,result.state,nullptr,IID_PPV_ARGS(&result.resource)),"bridge texture"); return result;
    }
    void images(RenderExtent processing,const NativeUpscaleImage& picture) {
        const bool changed=!bridge[0].resource || bridge[0].extent.width!=processing.width || bridge[0].extent.height!=processing.height;
        const bool snapshot_changed=!snapshot.resource || snapshot.extent.width!=picture.extent.width || snapshot.extent.height!=picture.extent.height || snapshot.format!=DXGI_FORMAT(picture.format);
        if(!changed && !snapshot_changed) return;
        if(bridge[0].resource || snapshot.resource) context.idle();
        if(changed) for(auto& entry:bridge) entry=image(processing,DXGI_FORMAT_R16G16B16A16_FLOAT);
        if(snapshot_changed) snapshot=image(picture.extent,DXGI_FORMAT(picture.format));
    }
    std::shared_ptr<Work> work() {
        for(const auto& item:pool) if(item.use_count()==1) { item->resources.clear(); return item; }
        if(pool.size()>=64) throw std::runtime_error("DX12 DLSS 5 submissions were not retired");
        auto result=std::make_shared<Work>();
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors=8; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&result->descriptors)),"bridge descriptors"); pool.push_back(result); return result;
    }
    void views(Work& work,const NativeUpscaleImage& picture,uint32_t last) {
        const auto start=work.descriptors->GetCPUDescriptorHandleForHeapStart(); const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto srv=[&](UINT index,ID3D12Resource* resource,DXGI_FORMAT format) {
            auto at=start; at.ptr+=size_t(index)*stride; D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1; device->CreateShaderResourceView(resource,&view,at);
        };
        const auto uav=[&](UINT index,ID3D12Resource* resource,DXGI_FORMAT format) {
            auto at=start; at.ptr+=size_t(index)*stride; D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; device->CreateUnorderedAccessView(resource,nullptr,&view,at);
        };
        srv(0,snapshot.resource.Get(),snapshot.format); srv(1,bridge[2].resource.Get(),bridge[2].format); srv(2,snapshot.resource.Get(),snapshot.format); uav(3,bridge[0].resource.Get(),bridge[0].format);
        srv(4,bridge[last].resource.Get(),bridge[last].format); srv(5,bridge[2].resource.Get(),bridge[2].format); srv(6,snapshot.resource.Get(),snapshot.format); uav(7,static_cast<ID3D12Resource*>(picture.resource),DXGI_FORMAT(picture.format));
    }
    void dispatch(ID3D12GraphicsCommandList* command,Work& work,bool first,const Constants& constants,RenderExtent extent) {
        ID3D12DescriptorHeap* heaps[]{work.descriptors.Get()}; command->SetDescriptorHeaps(1,heaps); command->SetComputeRootSignature(root.Get()); command->SetPipelineState(first ? prepare.Get() : resolve.Get());
        command->SetComputeRoot32BitConstants(0,16,&constants,0);
        auto read=work.descriptors->GetGPUDescriptorHandleForHeapStart(); const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if(!first) read.ptr+=4ull*stride; auto write=read; write.ptr+=3ull*stride;
        command->SetComputeRootDescriptorTable(1,read); command->SetComputeRootDescriptorTable(2,write); command->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);
    }
};
NativeDlss5Pass::NativeDlss5Pass() : impl_(std::make_unique<Impl>()) {}
NativeDlss5Pass::~NativeDlss5Pass() {
    try { shutdown(); } catch(const std::exception& error) { std::fprintf(stderr,"DX12 DLSS 5 shutdown: %s\n",error.what()); }
}
void NativeDlss5Pass::configure(const NativeDlss5Context& context) {
    shutdown();
    if(!context.device || !context.idle) throw std::invalid_argument("native DLSS 5 requires a device and GPU idle callback");
    impl_->context=context; impl_->device=context.device;
}
std::shared_ptr<NativeDlss5Pass::Work> NativeDlss5Pass::record(const NativeDlss5Frame& frame) {
    auto& i=*impl_; i.ran=false;
    if(i.open_failed || !i.device || !frame.settings.enabled()) return {};
    std::shared_ptr<Work> work;
    auto* command=static_cast<ID3D12GraphicsCommandList*>(frame.command);
    std::array<ID3D12Resource*,3> inputs{static_cast<ID3D12Resource*>(frame.picture.resource),static_cast<ID3D12Resource*>(frame.depth.resource),static_cast<ID3D12Resource*>(frame.motion.resource)};
    std::array<D3D12_RESOURCE_STATES,3> states{D3D12_RESOURCE_STATES(frame.picture.state),D3D12_RESOURCE_STATES(frame.depth.state),D3D12_RESOURCE_STATES(frame.motion.state)};
    const auto original=states;
    const auto restore=[&] { if(command) for(size_t n=0;n<inputs.size();++n) if(inputs[n]) transition(command,inputs[n],states[n],original[n]); };
    try {
        if(!command || !inputs[0] || !inputs[1] || !inputs[2] || inputs[0]==inputs[1] || inputs[0]==inputs[2] || inputs[1]==inputs[2] ||
            !frame.size.width || !frame.size.height || !frame.scene_size.width || !frame.scene_size.height || frame.origin[0]<0 || frame.origin[1]<0 ||
            uint64_t(frame.origin[0])+frame.size.width>frame.picture.extent.width || uint64_t(frame.origin[1])+frame.size.height>frame.picture.extent.height ||
            frame.scene_size.width>frame.depth.extent.width || frame.scene_size.height>frame.depth.extent.height || frame.scene_size.width>frame.motion.extent.width || frame.scene_size.height>frame.motion.extent.height)
            throw std::invalid_argument("invalid native DLSS 5 image region");
        if(frame.picture.format!=DXGI_FORMAT_R8G8B8A8_UNORM && frame.picture.format!=DXGI_FORMAT_R16G16B16A16_FLOAT) throw std::invalid_argument("native DLSS 5 picture requires RGBA8 or RGBA16F");
        if(!(inputs[0]->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) throw std::invalid_argument("native DLSS 5 picture lacks UAV access");
        for(const auto& field:dlss5_fields()) if(const auto error=dlss5_value_error(field,field.get(frame.settings)); !error.empty()) throw std::invalid_argument(error);
        if(!i.runtime) {
            NgxInit init; init.native_device=i.device.Get(); init.runtime_directory=i.context.directory;
            if(!public_release()) init.log_file=i.context.directory/"debug"/"ngx_dlss5_dx12.log";
            std::string reason; i.runtime=Dlss5Runtime::open(i.context.backend ? i.context.backend() : make_native_ngx_backend(),init,reason);
            if(!i.runtime) { i.open_failed=true; i.fail(reason); return {}; }
        }
        if(i.failed) return {};
        const auto& settings=frame.settings;
        const auto processing=dlss5_processing_extent(frame.size.width,frame.size.height,settings.resolution_scale);
        if(!i.runtime->prepare(command,{processing.width,processing.height,dlss5_preset_hint(settings)},settings.passes,i.context.idle)) { i.fail(i.runtime->reason()); return {}; }
        i.pipelines(); i.images({processing.width,processing.height},frame.picture); work=i.work();
        work->root=i.root; work->prepare=i.prepare; work->resolve=i.resolve;
        for(auto* resource:inputs) work->resources.emplace_back(resource);
        for(const auto& image:i.bridge) work->resources.push_back(image.resource); work->resources.push_back(i.snapshot.resource);
        i.views(*work,frame.picture,i.runtime->passes()&1);
        transition(command,inputs[0],states[0],D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(command,i.snapshot.resource.Get(),i.snapshot.state,D3D12_RESOURCE_STATE_COPY_DEST); command->CopyResource(i.snapshot.resource.Get(),inputs[0]);
        transition(command,i.snapshot.resource.Get(),i.snapshot.state,kRead);
        transition(command,i.bridge[0].resource.Get(),i.bridge[0].state,kWrite); transition(command,i.bridge[2].resource.Get(),i.bridge[2].state,kRead);
        Constants constants{}; constants.gain=float(dlss5_pre_gain(settings)); constants.white=float(settings.diffuse_white_nits);
        constants.encoding=int(settings.encoding); constants.nr_encoding=int(settings.nr_encoding); constants.domain=int(settings.scaling_domain);
        constexpr int down[]{3,1,0},up[]{1,2,0};
        const bool residual=(processing.width!=frame.size.width || processing.height!=frame.size.height) && settings.scale_reconstruction==Dlss5Reconstruction::rgb_residual;
        constants.source_size[0]=int32_t(frame.size.width); constants.source_size[1]=int32_t(frame.size.height); constants.target_size[0]=int32_t(processing.width); constants.target_size[1]=int32_t(processing.height);
        constants.source_origin[0]=frame.origin[0]; constants.source_origin[1]=frame.origin[1]; constants.filter=down[int(settings.downsample_filter)];
        i.dispatch(command,*work,true,constants,{processing.width,processing.height}); memory_barrier(command);
        if(residual) {
            transition(command,i.bridge[0].resource.Get(),i.bridge[0].state,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(command,i.bridge[2].resource.Get(),i.bridge[2].state,D3D12_RESOURCE_STATE_COPY_DEST); command->CopyResource(i.bridge[2].resource.Get(),i.bridge[0].resource.Get());
            transition(command,i.bridge[2].resource.Get(),i.bridge[2].state,kRead);
        }
        transition(command,inputs[1],states[1],kRead); transition(command,inputs[2],states[2],kRead);
        Dlss5Run run; run.depth=ngx_image(frame.depth); run.motion=ngx_image(frame.motion);
        run.depth_region=run.motion_region={frame.scene_size.width,frame.scene_size.height}; run.color_region=run.output_region={processing.width,processing.height};
        run.motion_scale[0]=float(processing.width)/frame.scene_size.width; run.motion_scale[1]=float(processing.height)/frame.scene_size.height;
        run.jitter[0]=frame.jitter[0]*run.motion_scale[0]; run.jitter[1]=frame.jitter[1]*run.motion_scale[1]; run.reset=frame.reset; run.settings=settings;
        for(uint32_t pass=0;pass<i.runtime->passes();++pass) {
            auto& source=i.bridge[pass&1]; auto& target=i.bridge[(pass&1)^1];
            transition(command,source.resource.Get(),source.state,kRead); transition(command,target.resource.Get(),target.state,kWrite);
            run.color=ngx_image({source.resource.Get(),uint32_t(source.format),uint32_t(source.state),source.extent});
            run.output=ngx_image({target.resource.Get(),uint32_t(target.format),uint32_t(target.state),target.extent},true);
            if(!i.runtime->evaluate(command,pass,run)) { memory_barrier(command); restore(); i.fail(i.runtime->reason()); return work; }
            memory_barrier(command);
        }
        auto& final=i.bridge[i.runtime->passes()&1]; transition(command,final.resource.Get(),final.state,kRead); transition(command,inputs[0],states[0],kWrite);
        constants.source_size[0]=int32_t(processing.width); constants.source_size[1]=int32_t(processing.height); constants.target_size[0]=int32_t(frame.size.width); constants.target_size[1]=int32_t(frame.size.height);
        constants.source_origin[0]=constants.source_origin[1]=0; constants.target_origin[0]=frame.origin[0]; constants.target_origin[1]=frame.origin[1]; constants.filter=up[int(settings.upsample_filter)]; constants.residual=residual;
        i.dispatch(command,*work,false,constants,frame.size); memory_barrier(command); restore(); i.status.clear(); i.ran=true; return work;
    } catch(const std::exception& error) { restore(); i.fail(error.what()); return work; }
}
const std::string& NativeDlss5Pass::status() const { return impl_->status; }
bool NativeDlss5Pass::failed() const { return impl_->failed; }
bool NativeDlss5Pass::ran() const { return impl_->ran; }
void NativeDlss5Pass::settings_changed() { if(impl_->runtime) impl_->failed=false; impl_->ran=false; }
void NativeDlss5Pass::shutdown() {
    if(impl_->device) impl_->context.idle();
    impl_=std::make_unique<Impl>();
}
bool probe_native_dlss5(const std::filesystem::path& directory,std::string& reason) {
    std::unique_ptr<Dlss5Runtime> runtime;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;
    try {
        std::error_code error;
        if(!std::filesystem::exists(directory/kDlss5Runtime,error)) { reason=std::string(kDlss5Runtime)+" missing next to the game"; return false; }
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"probe factory");
        for(UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> adapter; const auto result=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
            if(result==DXGI_ERROR_NOT_FOUND) break; check(result,"probe adapter"); DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc),"probe adapter description");
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) break;
        }
        if(!device) throw std::runtime_error("DLSS 5: no DirectX 12 hardware device");
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"probe queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"probe allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)),"probe command list");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"probe fence"); event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) throw std::runtime_error("DLSS 5: probe event creation failed");
        NgxInit init; init.native_device=device.Get(); init.runtime_directory=directory;
        if(!public_release()) init.log_file=directory/"debug"/"ngx_dlss5_dx12_probe.log";
        runtime=Dlss5Runtime::open(make_native_ngx_backend(),init,reason);
        if(!runtime) { CloseHandle(event); return false; }
        const bool created=runtime->prepare(command.Get(),{1280,720,dlss5_preset_hint(Dlss5Settings{})},1,{});
        if(!created) reason=runtime->reason();
        check(command->Close(),"close probe commands"); ID3D12CommandList* lists[]{command.Get()}; queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),1),"signal probe fence"); check(fence->SetEventOnCompletion(1,event),"probe completion event");
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0) throw std::runtime_error("DLSS 5: probe GPU timeout");
        check(device->GetDeviceRemovedReason(),"probe device status"); runtime.reset(); CloseHandle(event); return created;
    } catch(const std::exception& error) {
        reason=error.what(); runtime.reset(); if(event) CloseHandle(event); return false;
    }
}
}
