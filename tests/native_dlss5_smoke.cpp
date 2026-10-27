// Standalone DX12 counterpart of renderer_dlss5_smoke: copy/fill/refusal stand-ins exercise the real bridge shaders.
// --runtime <directory> additionally requires successful feature creation and evaluation of the owner's local DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "native_dlss5.h"
#include "dlss5_math.h"
#include "render_pixels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
constexpr auto read_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
void check(HRESULT result,const char* text) { if(FAILED(result)) throw std::runtime_error(std::string(text)+": "+std::to_string(uint32_t(result))); }
fs::path executable_dir() {
    std::vector<wchar_t> path(32768); const DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    require(size && size<path.size(),"executable path unavailable"); return fs::path(std::wstring(path.data(),size)).parent_path();
}
void barrier(ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    if(before==after) return; D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; command->ResourceBarrier(1,&b);
}
struct Texture {
    ComPtr<ID3D12Resource> resource;
    uint32_t width=0,height=0,bytes=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;
    uc::NativeUpscaleImage image() const { return {resource.Get(),uint32_t(format),uint32_t(state),{width,height}}; }
};
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    std::vector<ComPtr<ID3D12Resource>> staging;
    HANDLE event=nullptr;
    uint64_t value=0;
    uint32_t waits=0;
    Gpu() {
        ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer"); debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> adapter; const auto found=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
            if(found==DXGI_ERROR_NOT_FOUND) break; check(found,"adapter enumeration"); DXGI_ADAPTER_DESC1 description{}; check(adapter->GetDesc1(&description),"adapter description");
            if(!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) break;
        }
        require(bool(device),"no DX12 hardware device"); check(device.As(&info),"DX12 validation queue");
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)),"command list"); check(command->Close(),"initial close");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence"); event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"fence event");
    }
    ~Gpu() { if(event) CloseHandle(event); }
    void idle() {
        ++waits; check(queue->Signal(fence.Get(),++value),"queue fence"); check(fence->SetEventOnCompletion(value,event),"fence completion");
        require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU wait timed out"); check(device->GetDeviceRemovedReason(),"device status");
    }
    void begin() { staging.clear(); check(allocator->Reset(),"allocator reset"); check(command->Reset(allocator.Get(),nullptr),"command reset"); }
    void finish() { check(command->Close(),"close commands"); ID3D12CommandList* lists[]{command.Get()}; queue->ExecuteCommandLists(1,lists); idle(); }
    ComPtr<ID3D12Resource> buffer(uint64_t bytes,D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=type; D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"staging buffer"); return result;
    }
    Texture texture(uint32_t width,uint32_t height,DXGI_FORMAT format,uint32_t bytes) {
        Texture result; result.width=width; result.height=height; result.format=format; result.bytes=bytes;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width; desc.Height=height; desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1; desc.Format=format; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,result.state,nullptr,IID_PPV_ARGS(&result.resource)),"test texture"); return result;
    }
    void upload(Texture& target,const void* data,D3D12_RESOURCE_STATES desired) {
        const auto desc=target.resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 size=0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&size); auto source=buffer(size,D3D12_HEAP_TYPE_UPLOAD);
        void* mapped=nullptr; const D3D12_RANGE none{0,0}; check(source->Map(0,&none,&mapped),"upload map");
        for(uint32_t y=0;y<target.height;++y) std::memcpy(static_cast<std::byte*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,static_cast<const std::byte*>(data)+size_t(y)*target.width*target.bytes,size_t(target.width)*target.bytes);
        source->Unmap(0,nullptr); barrier(command.Get(),target.resource.Get(),target.state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=source.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=target.resource.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(command.Get(),target.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,desired); target.state=desired; staging.push_back(source);
    }
    std::vector<uint8_t> read(Texture& texture) {
        begin(); const auto desc=texture.resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 size=0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&size); auto target=buffer(size,D3D12_HEAP_TYPE_READBACK);
        barrier(command.Get(),texture.resource.Get(),texture.state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=texture.resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=target.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=footprint; command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(command.Get(),texture.resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,texture.state); finish();
        void* mapped=nullptr; const D3D12_RANGE range{0,SIZE_T(size)}; check(target->Map(0,&range,&mapped),"readback map"); std::vector<uint8_t> bytes(size_t(texture.width)*texture.height*texture.bytes);
        for(uint32_t y=0;y<texture.height;++y) std::memcpy(bytes.data()+size_t(y)*texture.width*texture.bytes,static_cast<const uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(texture.width)*texture.bytes);
        const D3D12_RANGE none{0,0}; target->Unmap(0,&none); return bytes;
    }
    void validation() {
        uint32_t errors=0;
        for(UINT64 index=0;index<info->GetNumStoredMessagesAllowedByRetrievalFilter();++index) {
            SIZE_T size=0; check(info->GetMessage(index,nullptr,&size),"validation message size"); std::vector<std::byte> bytes(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); check(info->GetMessage(index,message,&size),"validation message");
            if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) { ++errors; std::fprintf(stderr,"DX12 validation [%u]: %s\n",unsigned(message->ID),message->pDescription); }
        }
        require(errors==0,"DLSS 5 produced DX12 validation errors");
    }
};
struct Stand {
    enum Mode { copy,fill,refuse,fail_run } mode=copy;
    uint32_t creates=0,evaluates=0,releases=0;
    float paint=.5f;
    std::map<std::string,float> floats;
    std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
};
struct Parameters final : uc::NgxParameters {
    std::map<std::string,uc::NgxImage> images;
    std::map<std::string,float> floats;
    void set_uint(const char*,uint32_t) override {}
    void set_int(const char*,int) override {}
    void set_float(const char* name,float value) override { floats[name]=value; }
    void set_image(const char* name,const uc::NgxImage& image) override { images[name]=image; }
};
struct StandBackend final : uc::NgxBackend {
    std::shared_ptr<Stand> state;
    ComPtr<ID3D12Device> device;
    explicit StandBackend(std::shared_ptr<Stand> value):state(std::move(value)) {}
    uint32_t init(const uc::NgxInit& init) override { device=static_cast<ID3D12Device*>(init.native_device); require(bool(device),"fake backend got no native device"); return uc::kNgxSuccess; }
    uint32_t requirements(uc::NgxRequirement&) override { return uc::kNgxSuccess; }
    std::unique_ptr<uc::NgxParameters> allocate_parameters(uint32_t& result) override { result=uc::kNgxSuccess; return std::make_unique<Parameters>(); }
    uint32_t create_feature(void* command,uc::NgxParameters&,void*& handle) override {
        require(command!=nullptr,"native feature got no command list"); ++state->creates;
        if(state->mode==Stand::refuse) return 0xBAD0000B; handle=reinterpret_cast<void*>(uintptr_t(state->creates)); return uc::kNgxSuccess;
    }
    uint32_t evaluate(void* opaque,void*,uc::NgxParameters& parameters) override {
        ++state->evaluates; auto* command=static_cast<ID3D12GraphicsCommandList*>(opaque); const auto& values=static_cast<Parameters&>(parameters); state->floats=values.floats;
        const auto& color=values.images.at("DLSSNR.Color"); const auto& output=values.images.at("DLSSNR.Output");
        require(color.native_resource && output.native_resource && color.image==VK_NULL_HANDLE && output.image==VK_NULL_HANDLE,"native resources crossed Vulkan handles");
        require(values.images.at("DLSSNR.Depth").native_resource && values.images.at("DLSSNR.MVec").native_resource && output.read_write && !color.read_write,"NGX resource roles incorrect");
        if(state->mode==Stand::fail_run) return 0xBAD0000B;
        auto* source=static_cast<ID3D12Resource*>(color.native_resource); auto* target=static_cast<ID3D12Resource*>(output.native_resource);
        if(state->mode==Stand::copy) {
            barrier(command,source,read_state,D3D12_RESOURCE_STATE_COPY_SOURCE); barrier(command,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            command->CopyResource(target,source); barrier(command,source,D3D12_RESOURCE_STATE_COPY_SOURCE,read_state); barrier(command,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors=1; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ComPtr<ID3D12DescriptorHeap> gpu,cpu; check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&gpu)),"fake fill GPU heap"); desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE; check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&cpu)),"fake fill CPU heap");
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(target,nullptr,&view,gpu->GetCPUDescriptorHandleForHeapStart()); device->CreateUnorderedAccessView(target,nullptr,&view,cpu->GetCPUDescriptorHandleForHeapStart());
            ID3D12DescriptorHeap* heaps[]{gpu.Get()}; command->SetDescriptorHeaps(1,heaps); const float paint[]{state->paint,state->paint,state->paint,1};
            command->ClearUnorderedAccessViewFloat(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),target,paint,0,nullptr); state->heaps.push_back(gpu); state->heaps.push_back(cpu);
        }
        return uc::kNgxSuccess;
    }
    void release_feature(void*) override { ++state->releases; }
    void shutdown() override {}
    std::string diagnostic() const override { return "stand-in feature failure"; }
};
std::vector<uint8_t> pattern(uint32_t width,uint32_t height,bool real) {
    std::vector<uint8_t> bytes(size_t(width)*height*4);
    for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        const auto offset=(size_t(y)*width+x)*4;
        if(real) { bytes[offset]=uint8_t(32+x*190/width); bytes[offset+1]=uint8_t(20+y*200/height); bytes[offset+2]=uint8_t(48+((x/11+y/13)%2)*80); }
        else { const bool side=x+y>width*3/4; bytes[offset]=side ? 190 : 45; bytes[offset+1]=side ? 70 : 150; bytes[offset+2]=side ? 25 : 85; }
        bytes[offset+3]=real ? 255 : uint8_t(32+(x*13+y*7)%224);
    }
    return bytes;
}
void same(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,int tolerance) {
    require(a.size()==b.size(),"picture extent mismatch");
    for(size_t n=0;n<a.size();++n) if(std::abs(int(a[n])-int(b[n]))>(n%4==3 ? 0 : tolerance)) throw std::runtime_error("bridge changed pixel "+std::to_string(n/4)+" channel "+std::to_string(n%4)+": "+std::to_string(a[n])+" vs "+std::to_string(b[n]));
}
}
int main(int argc,char** argv) {
    try {
        fs::path runtime_directory;
        if(argc==3 && std::string_view(argv[1])=="--runtime") runtime_directory=argv[2];
        else require(argc==1,"usage: native_dlss5_smoke [--runtime <directory>]");
        const auto directory=executable_dir();
        const auto fake_directory=directory/("native-dlss5-stand-"+std::to_string(GetCurrentProcessId())); fs::create_directory(fake_directory);
        const auto marker=fake_directory/uc::kDlss5Runtime; { std::ofstream file(marker,std::ios::binary); file<<"stand-in marker, never loaded"; }
        Gpu gpu; auto state=std::make_shared<Stand>(); uc::NativeDlss5Pass pass;
        uc::NativeDlss5Context context{gpu.device.Get(),directory/"data/shaders",fake_directory,[&]{gpu.idle();},[state]{return std::make_unique<StandBackend>(state);}};
        pass.configure(context);
        auto picture=gpu.texture(320,180,DXGI_FORMAT_R8G8B8A8_UNORM,4),depth=gpu.texture(160,90,DXGI_FORMAT_R32_FLOAT,4),motion=gpu.texture(160,90,DXGI_FORMAT_R16G16_FLOAT,4);
        const auto baseline=pattern(picture.width,picture.height,false); std::vector<float> depth_values(size_t(depth.width)*depth.height,.5f); std::vector<uint16_t> vectors(size_t(motion.width)*motion.height*2,0);
        for(size_t n=0;n<vectors.size();n+=2) { vectors[n]=0xbc00; vectors[n+1]=0x3800; }
        gpu.begin(); gpu.upload(depth,depth_values.data(),read_state); gpu.upload(motion,vectors.data(),D3D12_RESOURCE_STATE_COPY_SOURCE); gpu.finish();
        uc::NativeDlss5Frame frame; frame.origin={16,8}; frame.size={288,164}; frame.scene_size={160,90}; frame.jitter={.25f,-.125f}; frame.reset=true;
        const auto run=[&](uc::Dlss5Settings settings) {
            gpu.begin(); gpu.upload(picture,baseline.data(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|read_state);
            frame.command=gpu.command.Get(); frame.picture=picture.image(); frame.depth=depth.image(); frame.motion=motion.image(); frame.settings=settings;
            auto work=pass.record(frame); gpu.finish(); work.reset(); return gpu.read(picture);
        };
        uc::Dlss5Settings settings; same(run(settings),baseline,0); require(!pass.ran() && state->creates==0,"off hook ran the network");
        settings.hook=uc::Dlss5Hook::present; same(run(settings),baseline,2); require(pass.ran() && pass.status().empty(),"identity bridge failed");
        const auto first_creates=state->creates; same(run(settings),baseline,2); require(state->creates==first_creates,"identical settings recreated NGX feature");
        require(std::abs(state->floats.at("DLSSNR.MVecScaleX")-288.f/160)<1e-5f && std::abs(state->floats.at("DLSSNR.JitterOffsetY")+.125f*164/90)<1e-5f,"motion or jitter scaling changed");
        // Shared Vulkan fixture semantics: zero network residual preserves every original pixel, including sharp edges.
        settings.resolution_scale=.5; settings.passes=2;
        for(const auto down:{uc::Dlss5DownFilter::area,uc::Dlss5DownFilter::bilinear,uc::Dlss5DownFilter::nearest})
            for(const auto up:{uc::Dlss5UpFilter::bilinear,uc::Dlss5UpFilter::bicubic,uc::Dlss5UpFilter::nearest}) { settings.downsample_filter=down; settings.upsample_filter=up; same(run(settings),baseline,3); }
        require(state->releases>0 && gpu.waits>0,"settings recreation did not retire old features");
        settings.scale_reconstruction=uc::Dlss5Reconstruction::processed; settings.resolution_scale=.625; settings.downsample_filter=uc::Dlss5DownFilter::area; settings.upsample_filter=uc::Dlss5UpFilter::bilinear;
        const auto filtered=run(settings); settings.downsample_filter=uc::Dlss5DownFilter::nearest; settings.upsample_filter=uc::Dlss5UpFilter::nearest; const auto nearest=run(settings);
        require(filtered!=nearest,"filter selection did not change diagonal-edge coverage");
        for(const auto* result:{&filtered,&nearest}) for(const auto at:{size_t(40)*320+40,size_t(130)*320+250}) for(size_t channel=0;channel<3;++channel) require(std::abs(int((*result)[at*4+channel])-int(baseline[at*4+channel]))<=3,"processed filter changed flat color");
        settings.scale_reconstruction=uc::Dlss5Reconstruction::rgb_residual; settings.resolution_scale=.75; settings.passes=3; settings.encoding=uc::Dlss5Encoding::scrgb;
        settings.nr_encoding=uc::Dlss5NrEncoding::pq_sdr; settings.scaling_domain=uc::Dlss5Domain::signed_pq; settings.exposure_value=uc::Dlss5ExposureValue::on; settings.exposure=2;
        const auto before=state->evaluates; same(run(settings),baseline,3); require(state->evaluates-before==3,"multipass ping-pong count incorrect");
        state->mode=Stand::fill; settings=uc::Dlss5Settings{}; settings.hook=uc::Dlss5Hook::present; settings.exposure_value=uc::Dlss5ExposureValue::on; settings.exposure=2;
        const auto painted=run(settings); const int expected=int(std::lround(std::clamp(uc::dlss5_encode_source(settings,uc::dlss5_nr_decode(settings,.5f)/float(uc::dlss5_pre_gain(settings))),0.f,1.f)*255));
        for(uint32_t y=0;y<picture.height;++y) for(uint32_t x=0;x<picture.width;++x) {
            const size_t at=(size_t(y)*picture.width+x)*4; const bool inside=x>=16 && x<304 && y>=8 && y<172;
            for(size_t c=0;c<4;++c) require(std::abs(int(painted[at+c])-int(inside && c<3 ? expected : baseline[at+c]))<=(inside && c<3 ? 2 : 0),"fill altered alpha/outside region or encoding math");
        }
        same(gpu.read(depth),std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(depth_values.data()),reinterpret_cast<const uint8_t*>(depth_values.data()+depth_values.size())),0);
        same(gpu.read(motion),std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(vectors.data()),reinterpret_cast<const uint8_t*>(vectors.data()+vectors.size())),0);
        state->mode=Stand::fail_run; pass.settings_changed(); same(run(settings),baseline,0); require(pass.failed() && !pass.ran() && pass.status().find("evaluate")!=std::string::npos,"evaluation failure was hidden");
        const auto failed_evaluations=state->evaluates; same(run(settings),baseline,0); require(state->evaluates==failed_evaluations,"failed evaluation retried every frame");
        state->mode=Stand::copy; settings.passes=2; pass.settings_changed(); same(run(settings),baseline,3); require(pass.ran(),"changed feature key did not recover after failure");
        pass.shutdown(); state->mode=Stand::refuse; pass.configure(context); same(run(settings),baseline,0);
        require(pass.failed() && pass.status().find("stand-in")!=std::string::npos,"feature refusal reason lost"); const auto refused_creates=state->creates; same(run(settings),baseline,0); require(state->creates==refused_creates,"refused feature retried every frame");
        pass.shutdown(); gpu.validation(); fs::remove(marker); fs::remove(fake_directory);
        std::puts("native DLSS 5 deterministic bridge: filters/residual/multipass/encoding/alpha/failure/state restore passed");
        if(!runtime_directory.empty()) {
            std::string reason; const bool supported=uc::probe_native_dlss5(runtime_directory,reason); require(supported,reason.c_str());
            uc::NativeDlss5Pass real; real.configure({gpu.device.Get(),directory/"data/shaders",runtime_directory,[&]{gpu.idle();},{}});
            auto color=gpu.texture(1280,720,DXGI_FORMAT_R8G8B8A8_UNORM,4),z=gpu.texture(640,360,DXGI_FORMAT_R32_FLOAT,4),m=gpu.texture(640,360,DXGI_FORMAT_R16G16_FLOAT,4);
            const auto input=pattern(1280,720,true); std::vector<float> depths(640*360,.6f); std::vector<uint32_t> zero_motion(640*360,0);
            gpu.begin(); gpu.upload(z,depths.data(),read_state); gpu.upload(m,zero_motion.data(),read_state); gpu.finish();
            uc::NativeDlss5Frame actual; actual.size={1280,720}; actual.scene_size={640,360}; actual.settings.hook=uc::Dlss5Hook::present;
            for(int n=0;n<4;++n) {
                gpu.begin(); gpu.upload(color,input.data(),read_state); actual.command=gpu.command.Get(); actual.picture=color.image(); actual.depth=z.image(); actual.motion=m.image(); actual.reset=n==0;
                auto work=real.record(actual); gpu.finish(); require(real.ran(),real.status().c_str()); work.reset();
            }
            const auto output=gpu.read(color); size_t changed=0; uint64_t difference=0; unsigned maximum=0;
            for(size_t p=0;p<output.size();p+=4) { unsigned delta=0; for(size_t c=0;c<3;++c) { const auto d=unsigned(std::abs(int(output[p+c])-int(input[p+c]))); delta=std::max(delta,d); difference+=d; maximum=std::max(maximum,d); } changed+=delta>2; require(output[p+3]==input[p+3],"real DLSS 5 altered alpha"); }
            std::printf("real DX12 DLSS 5 changed=%zu max=%u total=%llu\n",changed,maximum,static_cast<unsigned long long>(difference));
            require(changed>1280*720/100 && difference>changed*3,"real DLSS 5 produced no meaningful picture change");
            uc::save_png({0,1280,720,output},directory/"dlss5-dx12-real.png"); real.shutdown(); gpu.validation(); std::puts("native DLSS 5 real feature and evaluation passed");
        }
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"native DLSS 5 smoke: %s\n",error.what()); return 1; }
}
