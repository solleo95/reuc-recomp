#pragma once
#include "native_upscaler.h"
#include <cstdint>
#include <string>

struct ID3D12CommandQueue;
struct IDXGIFactory2;
struct IDXGISwapChain3;
struct DXGI_SWAP_CHAIN_DESC1;

namespace uc {
// Render-thread-owned DX12 frame generation. Caller submits the prepared command list before Present.
// HUD-less color covers the whole output window, in SDR UNORM or HDR10 RGB10/PQ like the swapchain.
class NativeFrameGeneration {
public:
    virtual ~NativeFrameGeneration()=default;
    // Returns one owned COM reference. Adapter retains its own reference until destruction.
    virtual void create_swapchain(IDXGIFactory2*,void* hwnd,const DXGI_SWAP_CHAIN_DESC1&,IDXGISwapChain3**)=0;
    virtual void configure(RenderExtent display,RenderExtent max_render,uint32_t format,float hdr_peak=0)=0;
    // Before ResizeBuffers: disables generation and waits for SDK and application GPU work.
    virtual void release_context()=0;
    virtual void begin_frame(bool active,uint64_t frame,bool reset)=0;
    virtual void prepare(const NativeUpscaleInputs&)=0;
    virtual void tag_hudless(void* command,const NativeUpscaleImage&)=0;
    // Commit the final mode after all scene/menu/display decisions, immediately before Present.
    virtual void before_present() {}
    virtual void after_present()=0;
    // Before overwriting any tagged input: DLSS enqueues a GPU wait; FSR waits for paced presents.
    virtual void wait_for_inputs()=0;
    virtual void drain()=0;
    virtual bool active() const=0;
    virtual uint32_t multiplier() const=0;
    virtual uint64_t presented() const=0;
    virtual uint64_t generated() const=0;
};
std::unique_ptr<NativeFrameGeneration> make_native_frame_generation(UpscalerKind,ID3D12Device*,ID3D12CommandQueue*,
    const std::filesystem::path&,Streamline*,uint32_t multiplier);
// Creates a real small FSR interpolation context. Swapchain replacement is validated during renderer initialization.
bool native_fsr_frame_generation_supported(ID3D12Device*,const std::filesystem::path&,std::string& reason);
}
