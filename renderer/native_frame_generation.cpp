#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "native_frame_generation.h"
#include "dlss_frame_generation.h"
#include "streamline.h"
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result,const char* what) {
    if(FAILED(result)) throw std::runtime_error(std::string("DX12 Frame Generation: ")+what+" failed: "+std::to_string(uint32_t(result)));
}
void ffx_check(ffxReturnCode_t result,const char* what) {
    if(result!=FFX_API_RETURN_OK) throw std::runtime_error(std::string("FSR Frame Generation: ")+what+" failed: "+std::to_string(result));
}
struct FfxRuntime {
    HMODULE module=nullptr;
    ffxFunctions api{};
    explicit FfxRuntime(const std::filesystem::path& directory) {
        module=LoadLibraryW((directory/"amd_fidelityfx_dx12.dll").c_str());
        if(!module) throw std::runtime_error("FSR Frame Generation: amd_fidelityfx_dx12.dll could not be loaded");
        ffxLoadFunctions(&api,module);
        if(!api.CreateContext || !api.DestroyContext || !api.Configure || !api.Dispatch || !api.Query) {
            FreeLibrary(module); module=nullptr; throw std::runtime_error("FSR Frame Generation: runtime lacks the FFX API");
        }
    }
    ~FfxRuntime() { if(module) FreeLibrary(module); }
};
void create_fsr_context(FfxRuntime& runtime,ffxContext& context,ID3D12Device* device,RenderExtent display,RenderExtent render,DXGI_FORMAT format,float hdr_peak) {
    if(!device || !display.width || !display.height || !render.width || !render.height || !std::isfinite(hdr_peak) || hdr_peak<0)
        throw std::invalid_argument("invalid native frame generation dimensions or luminance");
    ffxCreateBackendDX12Desc backend{}; backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device=device;
    ffxCreateContextDescFrameGenerationHudless hudless{}; hudless.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.hudlessBackBufferFormat=ffxApiGetSurfaceFormatDX12(hdr_peak>0 ? format : DXGI_FORMAT_R8G8B8A8_UNORM);
    ffxCreateContextDescFrameGeneration create{}; create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext=&backend.header; backend.header.pNext=&hudless.header;
    create.flags=hdr_peak>0 ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
    create.displaySize={display.width,display.height}; create.maxRenderSize={render.width,render.height}; create.backBufferFormat=ffxApiGetSurfaceFormatDX12(format);
    ffx_check(runtime.api.CreateContext(&context,&create.header,nullptr),"create interpolation context");
}
FfxApiResource fsr_resource(const NativeUpscaleImage& image) {
    if(!image.resource) throw std::invalid_argument("FSR Frame Generation: missing native image");
    uint32_t state=0;
    if(image.state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) state|=FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if(image.state&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) state|=FFX_API_RESOURCE_STATE_PIXEL_READ;
    if(image.state&D3D12_RESOURCE_STATE_COPY_SOURCE) state|=FFX_API_RESOURCE_STATE_COPY_SRC;
    if(!state) throw std::invalid_argument("FSR Frame Generation: inputs must be in shader-readable states");
    auto result=ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(image.resource),state);
    // Color/depth resources may be typeless to allow multiple native views. FFX needs their sampled format.
    const auto format=DXGI_FORMAT(image.format);
    result.description.format=ffxApiGetSurfaceFormatDX12(format==DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : format);
    return result;
}
class NativeGenerationBase : public NativeFrameGeneration {
public:
    NativeGenerationBase(ID3D12Device* device,ID3D12CommandQueue* queue) : device_(device),queue_(queue) {
        if(!device || !queue) throw std::invalid_argument("native frame generation requires a DX12 device and queue");
        check(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)),"create completion fence");
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event_) throw std::runtime_error("DX12 Frame Generation: create completion event failed");
    }
    ~NativeGenerationBase() override { if(event_) CloseHandle(event_); }
protected:
    void wait_gpu() {
        const uint64_t value=++fence_value_; check(queue_->Signal(fence_.Get(),value),"signal completion fence");
        if(fence_->GetCompletedValue()<value) {
            check(fence_->SetEventOnCompletion(value,event_),"completion event");
            if(WaitForSingleObject(event_,30000)!=WAIT_OBJECT_0) throw std::runtime_error("DX12 Frame Generation: GPU completion timed out");
        }
        check(device_->GetDeviceRemovedReason(),"device status");
    }
    void check_hudless(const NativeUpscaleImage& image) const {
        if(!image.resource || image.extent.width!=display_.width || image.extent.height!=display_.height)
            throw std::invalid_argument("frame generation HUD-less image must cover the output window");
        if(image.format!=uint32_t(hdr_peak_>0 ? format_ : DXGI_FORMAT_R8G8B8A8_UNORM))
            throw std::invalid_argument("frame generation HUD-less format must match presentation encoding");
    }
    void check_inputs(const NativeUpscaleInputs& input) const {
        if(!input.command || !input.depth.resource || !input.motion.resource || !input.render_size.width || !input.render_size.height ||
            input.render_size.width>max_render_.width || input.render_size.height>max_render_.height ||
            !std::isfinite(input.frame_ms) || input.frame_ms<=0 || !std::isfinite(input.jitter[0]) || !std::isfinite(input.jitter[1]))
            throw std::invalid_argument("invalid native frame generation scene inputs");
    }
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    RenderExtent display_{},max_render_{};
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    float hdr_peak_=0;
private:
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_=nullptr;
    uint64_t fence_value_=0;
};
class NativeDlssGeneration final : public NativeGenerationBase {
public:
    NativeDlssGeneration(ID3D12Device* device,ID3D12CommandQueue* queue,Streamline& streamline,uint32_t multiplier)
        : NativeGenerationBase(device,queue),streamline_(streamline),requested_multiplier_(multiplier) {
        if(!streamline.frame_generation_loaded()) throw std::runtime_error("DLSS Frame Generation requires Streamline initialization before the DX12 device");
    }
    ~NativeDlssGeneration() override {
        try { release_context(); } catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); }
        generation_.reset(); swapchain_.Reset();
    }
    void create_swapchain(IDXGIFactory2* factory,void* hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,IDXGISwapChain3** out) override {
        if(!factory || !hwnd || !out || swapchain_) throw std::invalid_argument("invalid DLSS frame generation swapchain creation");
        // The factory proxy intercepts CreateSwapChainForHwnd and installs Streamline's present hook.
        ComPtr<IDXGIFactory2> owned=factory; void* upgraded=owned.Detach(); std::string reason;
        const bool upgraded_ok=streamline_.upgrade_native_interface(&upgraded,reason); owned.Attach(static_cast<IDXGIFactory2*>(upgraded));
        if(!upgraded_ok) throw std::runtime_error(reason);
        ComPtr<IDXGISwapChain1> created;
        check(owned->CreateSwapChainForHwnd(queue_.Get(),static_cast<HWND>(hwnd),&desc,nullptr,nullptr,&created),"create DLSS proxy swapchain");
        check(created.As(&swapchain_),"query DLSS proxy swapchain"); check(swapchain_.CopyTo(out),"retain DLSS proxy swapchain");
    }
    void configure(RenderExtent display,RenderExtent render,uint32_t format,float hdr_peak) override {
        release_context();
        if(!swapchain_ || !display.width || !display.height || !render.width || !render.height || !std::isfinite(hdr_peak) || hdr_peak<0)
            throw std::invalid_argument("invalid DLSS frame generation configuration");
        display_=display; max_render_=render; format_=DXGI_FORMAT(format); hdr_peak_=hdr_peak;
        generation_=std::make_unique<DlssFrameGeneration>(streamline_,requested_multiplier_);
        if(!generation_->state_valid()) throw std::runtime_error("DLSS Frame Generation: state query failed");
        if(std::min(display.width,display.height)<generation_->minimum_dimension()) {
            generation_.reset(); throw std::runtime_error("DLSS Frame Generation: output window is below SDK minimum dimensions");
        }
    }
    void release_context() override {
        if(!generation_) return;
        if(!generation_->set_active(false,true)) throw std::runtime_error("DLSS Frame Generation: disabling before resize failed");
        drain();
        generation_->clear_native_tags();
        presented_+=generation_->presented(); rendered_+=generation_->rendered(); generation_.reset(); enabled_=prepared_=tagged_=false;
    }
    void begin_frame(bool enabled,uint64_t,bool reset) override {
        if(!generation_) { if(enabled) throw std::logic_error("DLSS Frame Generation context is not configured"); return; }
        enabled_=enabled; reset_=reset;
        if(!enabled) generation_->clear_native_tags();
        prepared_=tagged_=false;
    }
    void prepare(const NativeUpscaleInputs& input) override {
        if(!active()) return;
        check_inputs(input);
        // DLSS SR has already set the shared viewport's depth, vectors and common constants for this token.
        prepared_=true;
    }
    void tag_hudless(void* command,const NativeUpscaleImage& image) override {
        if(!active()) return;
        if(!prepared_) throw std::logic_error("DLSS Frame Generation requires prepared scene inputs");
        check_hudless(image); generation_->tag_hudless_native(command,image); tagged_=true;
    }
    void after_present() override {
        if(!generation_) return;
        if(active() && !tagged_) throw std::logic_error("DLSS Frame Generation presented without HUD-less color");
        generation_->after_present();
        if(!generation_->state_valid()) throw std::runtime_error("DLSS Frame Generation: presented-state query failed");
        const auto rendered=rendered_+generation_->rendered();
        if(presented()>rendered) generated_=std::max(generated_,presented()-rendered);
        if(active() && generation_->status()!=0) {
            const auto status=generation_->status(); generation_->set_active(false,true);
            throw std::runtime_error("DLSS Frame Generation: SDK status "+std::to_string(status));
        }
    }
    void before_present() override {
        if(!generation_) return;
        if(enabled_ && (!prepared_ || !tagged_)) throw std::logic_error("DLSS Frame Generation requires complete inputs before Present");
        if(!generation_->set_active(enabled_,reset_)) throw std::runtime_error("DLSS Frame Generation: options were rejected");
    }
    void wait_for_inputs() override {
        if(generation_ && generation_->native_inputs_fence() && generation_->inputs_fence_value())
            check(queue_->Wait(static_cast<ID3D12Fence*>(generation_->native_inputs_fence()),generation_->inputs_fence_value()),"wait for DLSS inputs");
    }
    void drain() override { wait_for_inputs(); wait_gpu(); }
    bool active() const override { return generation_ && enabled_; }
    uint32_t multiplier() const override { return generation_ ? generation_->multiplier() : requested_multiplier_; }
    uint64_t presented() const override { return presented_+(generation_ ? generation_->presented() : 0); }
    uint64_t generated() const override { return generated_; }
private:
    Streamline& streamline_;
    std::unique_ptr<DlssFrameGeneration> generation_;
    uint32_t requested_multiplier_;
    uint64_t presented_=0,rendered_=0,generated_=0;
    bool enabled_=false,reset_=false,prepared_=false,tagged_=false;
};
class NativeFsrGeneration final : public NativeGenerationBase {
public:
    NativeFsrGeneration(ID3D12Device* device,ID3D12CommandQueue* queue,const std::filesystem::path& directory)
        : NativeGenerationBase(device,queue),runtime_(directory) {}
    ~NativeFsrGeneration() override {
        try { release_context(); } catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); }
        if(context_) { const auto result=runtime_.api.DestroyContext(&context_,nullptr); if(result!=FFX_API_RETURN_OK) std::fprintf(stderr,"FSR FG context cleanup failed: %u\n",result); }
        swapchain_.Reset();
        if(swapchain_context_) { const auto result=runtime_.api.DestroyContext(&swapchain_context_,nullptr); if(result!=FFX_API_RETURN_OK) std::fprintf(stderr,"FSR FG swapchain cleanup failed: %u\n",result); }
    }
    void create_swapchain(IDXGIFactory2* factory,void* hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,IDXGISwapChain3** out) override {
        if(!factory || !hwnd || !out || swapchain_context_) throw std::invalid_argument("invalid FSR frame generation swapchain creation");
        DXGI_SWAP_CHAIN_DESC1 description=desc; ComPtr<IDXGISwapChain4> created;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
        create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        create.swapchain=created.GetAddressOf(); create.hwnd=static_cast<HWND>(hwnd); create.desc=&description; create.dxgiFactory=factory; create.gameQueue=queue_.Get();
        ffx_check(runtime_.api.CreateContext(&swapchain_context_,&create.header,nullptr),"create replacement swapchain");
        check(created.As(&swapchain_),"query FSR replacement swapchain"); check(swapchain_.CopyTo(out),"retain FSR replacement swapchain");
        check(swapchain_->GetLastPresentCount(&last_present_count_),"read initial present count");
    }
    void configure(RenderExtent display,RenderExtent render,uint32_t format,float hdr_peak) override {
        release_context(); if(!swapchain_) throw std::logic_error("FSR Frame Generation requires replacement swapchain");
        create_fsr_context(runtime_,context_,device_.Get(),display,render,DXGI_FORMAT(format),hdr_peak);
        display_=display; max_render_=render; format_=DXGI_FORMAT(format); hdr_peak_=hdr_peak;
        configure_frame(false,{});
    }
    void release_context() override {
        if(!context_) return;
        drain(); configure_frame(false,{}); active_=false;
        // Disabling may enqueue presentation work; finish it before freeing callback state.
        drain(); ffx_check(runtime_.api.DestroyContext(&context_,nullptr),"destroy interpolation context"); context_=nullptr; prepared_=tagged_=false;
    }
    void begin_frame(bool enabled,uint64_t frame,bool reset) override {
        if(!context_) { if(enabled) throw std::logic_error("FSR Frame Generation context is not configured"); return; }
        frame_=frame; resets_[frame%resets_.size()].store(reset,std::memory_order_release); active_=enabled; prepared_=tagged_=false;
        if(!enabled) configure_frame(false,{});
    }
    void prepare(const NativeUpscaleInputs& input) override {
        if(!active_) return;
        check_inputs(input);
        ffxDispatchDescFrameGenerationPrepareCameraInfo camera{}; camera.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        camera.cameraUp[1]=1; camera.cameraRight[0]=1; camera.cameraForward[2]=-1;
        ffxDispatchDescFrameGenerationPrepare prepare{}; prepare.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE; prepare.header.pNext=&camera.header;
        prepare.frameID=frame_; prepare.commandList=input.command; prepare.renderSize={input.render_size.width,input.render_size.height};
        prepare.jitterOffset={input.jitter[0],input.jitter[1]}; prepare.motionVectorScale={1,1}; prepare.frameTimeDelta=input.frame_ms;
        const auto& p=input.projection;
        if(p[2][2]!=0 && p[1][1]!=0) { prepare.cameraNear=std::abs(p[2][3]/p[2][2]); prepare.cameraFar=std::abs(p[2][3]/(p[2][2]+1)); prepare.cameraFovAngleVertical=2*std::atan(1/p[1][1]); }
        else { prepare.cameraNear=.1f; prepare.cameraFar=1000; prepare.cameraFovAngleVertical=.8f; }
        prepare.viewSpaceToMetersFactor=0; prepare.depth=fsr_resource(input.depth); prepare.motionVectors=fsr_resource(input.motion);
        if(input.reset) resets_[frame_%resets_.size()].store(true,std::memory_order_release);
        ffx_check(runtime_.api.Dispatch(&context_,&prepare.header),"prepare scene"); prepared_=true;
    }
    void tag_hudless(void*,const NativeUpscaleImage& image) override {
        if(!active_) return;
        if(!prepared_) throw std::logic_error("FSR Frame Generation requires prepared scene inputs");
        check_hudless(image); configure_frame(true,fsr_resource(image)); tagged_=true;
    }
    void after_present() override {
        if(active_ && !tagged_) throw std::logic_error("FSR Frame Generation presented without HUD-less color");
        ++rendered_; update_count(); check_callback();
    }
    void wait_for_inputs() override {
        if(!swapchain_context_) return;
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{}; wait.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
        ffx_check(runtime_.api.Dispatch(&swapchain_context_,&wait.header),"wait for paced presents"); check_callback(); update_count();
    }
    void drain() override { wait_for_inputs(); wait_gpu(); }
    bool active() const override { return active_; }
    uint32_t multiplier() const override { return 2; }
    uint64_t presented() const override { return presented_; }
    uint64_t generated() const override { return generated_; }
private:
    static ffxReturnCode_t interpolate(ffxDispatchDescFrameGeneration* params,void* user) {
        auto& self=*static_cast<NativeFsrGeneration*>(user);
        params->reset=self.resets_[params->frameID%self.resets_.size()].load(std::memory_order_acquire);
        if(self.hdr_peak_>0 && params->minMaxLuminance[1]<=0) { params->minMaxLuminance[0]=0; params->minMaxLuminance[1]=self.hdr_peak_; }
        const auto result=self.runtime_.api.Dispatch(&self.context_,&params->header);
        if(result!=FFX_API_RETURN_OK) self.callback_error_.store(result,std::memory_order_release);
        return result;
    }
    void check_callback() {
        ffx_check(callback_error_.exchange(FFX_API_RETURN_OK,std::memory_order_acq_rel),"interpolation callback");
    }
    void configure_frame(bool enabled,FfxApiResource hudless) {
        ffxConfigureDescFrameGeneration config{}; config.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain=swapchain_.Get(); config.frameGenerationCallback=&interpolate; config.frameGenerationCallbackUserContext=this;
        config.frameGenerationEnabled=enabled; config.allowAsyncWorkloads=false; config.HUDLessColor=hudless;
        config.generationRect={0,0,int32_t(display_.width),int32_t(display_.height)}; config.frameID=frame_;
        ffx_check(runtime_.api.Configure(&context_,&config.header),"configure frame");
    }
    void update_count() {
        if(!swapchain_) return;
        UINT count=0; check(swapchain_->GetLastPresentCount(&count),"read actual present count");
        presented_+=UINT(count-last_present_count_); last_present_count_=count;
        if(presented_>rendered_) generated_=std::max(generated_,presented_-rendered_);
    }
    FfxRuntime runtime_;
    ffxContext swapchain_context_=nullptr,context_=nullptr;
    std::array<std::atomic<bool>,32> resets_{};
    std::atomic<ffxReturnCode_t> callback_error_{FFX_API_RETURN_OK};
    uint64_t frame_=0,presented_=0,rendered_=0,generated_=0;
    UINT last_present_count_=0;
    bool active_=false,prepared_=false,tagged_=false;
};
}
std::unique_ptr<NativeFrameGeneration> make_native_frame_generation(UpscalerKind kind,ID3D12Device* device,ID3D12CommandQueue* queue,
    const std::filesystem::path& directory,Streamline* streamline,uint32_t multiplier) {
    if(multiplier<2 || multiplier>6) throw std::invalid_argument("frame generation multiplier must be between 2 and 6");
    if(kind==UpscalerKind::dlss) {
        if(!streamline) throw std::runtime_error("DLSS Frame Generation requires Streamline initialization before DX12 device creation");
        return std::make_unique<NativeDlssGeneration>(device,queue,*streamline,multiplier);
    }
    if(kind==UpscalerKind::fsr) {
        if(multiplier!=2) std::fprintf(stderr,"FSR Frame Generation: 2x supported; %ux requested\n",multiplier);
        return std::make_unique<NativeFsrGeneration>(device,queue,directory);
    }
    throw std::invalid_argument("native frame generation requires DLSS or FSR");
}
bool native_fsr_frame_generation_supported(ID3D12Device* device,const std::filesystem::path& directory,std::string& reason) {
    try {
        FfxRuntime runtime(directory); ffxContext context=nullptr;
        create_fsr_context(runtime,context,device,{1280,720},{640,480},DXGI_FORMAT_R8G8B8A8_UNORM,0);
        ffx_check(runtime.api.DestroyContext(&context,nullptr),"destroy capability context"); return true;
    } catch(const std::exception& error) { reason=error.what(); return false; }
}
}
