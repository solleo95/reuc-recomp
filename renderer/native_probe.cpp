#define NOMINMAX
#include "upscaler_probe.h"
#include "streamline.h"
#include "ffx_dx12.h"
#include "native_frame_generation.h"
#include "dlss_frame_generation.h"
#include "native_rt_scene.h"
#include "native_dlss5.h"
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <stdexcept>

namespace uc {
using Microsoft::WRL::ComPtr;
UpscalerSupport probe_native_upscalers(const std::filesystem::path& directory,RendererBackend backend) {
    if(backend!=RendererBackend::directx11 && backend!=RendererBackend::directx12)throw std::invalid_argument("native renderer probe requires DirectX");
    UpscalerSupport support;
    if(backend==RendererBackend::directx11) {
        support.dlss_g_reason="the supplied DLSS Frame Generation SDK requires DirectX 12 or Vulkan";
        support.fsr_fg_reason="the supplied FidelityFX Frame Generation SDK has no DirectX 11 backend";
    }
    support.ray_tracing_reason=backend==RendererBackend::directx11 ? "DirectX 11 has no hardware ray tracing API" : "DirectX 12 hardware ray tracing unavailable";
    support.ray_reconstruction_reason=support.ray_tracing_reason;
    // Streamline shutdown leaves process-wide NGX state; probe the independent runtime first.
    if(backend==RendererBackend::directx12) support.dlss5=probe_native_dlss5(directory,support.dlss5_reason);
    else support.dlss5_reason="the supplied DLSS 5 runtime does not support DirectX 11";
    auto streamline=Streamline::load(directory,backend==RendererBackend::directx12,support.dlss_reason,backend==RendererBackend::directx12,backend);    if(!streamline && backend==RendererBackend::directx12) {
        support.dlss_g_reason=support.dlss_reason;
        // An unavailable FG plugin must not hide usable Super Resolution support.
        streamline=Streamline::load(directory,false,support.dlss_reason,false,backend);
    }
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context11;ComPtr<ID3D12Device> device12;
    LUID luid{};void* device=nullptr;
    try {
        const auto check=[](HRESULT result,const char* what) {if(FAILED(result))throw std::runtime_error(std::string(what)+" (HRESULT "+std::to_string(uint32_t(result))+")");};
        if(backend==RendererBackend::directx11) {
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL selected{};
            check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&device11,&selected,&context11),"D3D11 hardware device");
            ComPtr<ID3D11Device1> device1;check(device11.As(&device1),"D3D11.1 device");
            D3D11_FEATURE_DATA_D3D11_OPTIONS options{};check(device11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof options),"D3D11 options");
            if(!options.OutputMergerLogicOp)throw std::runtime_error("GPU lacks GX logic operations");
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(device11.As(&dxgi),"D3D11 adapter interface");check(dxgi->GetAdapter(&adapter),"D3D11 adapter");
            DXGI_ADAPTER_DESC desc{};check(adapter->GetDesc(&desc),"D3D11 adapter description");luid=desc.AdapterLuid;device=device11.Get();
            support.fsr_reason="the supplied FidelityFX SDK has no DirectX 11 backend";
        } else {
            ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
            for(UINT index=0;;++index) {
                ComPtr<IDXGIAdapter1> adapter;const auto found=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
                if(found==DXGI_ERROR_NOT_FOUND)break;check(found,"DXGI adapter enumeration");
                DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"DXGI adapter description");if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
                if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12))))break;
            }
            if(!device12)throw std::runtime_error("no D3D12 hardware device");
            D3D12_FEATURE_DATA_SHADER_MODEL shader{D3D_SHADER_MODEL_6_0};check(device12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader,sizeof shader),"D3D12 shader model");
            if(shader.HighestShaderModel<D3D_SHADER_MODEL_6_0)throw std::runtime_error("GPU lacks shader model 6.0");
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};check(device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof options),"D3D12 options");
            if(!options.OutputMergerLogicOp)throw std::runtime_error("GPU lacks GX logic operations");
            luid=device12->GetAdapterLuid();device=device12.Get();
            support.ray_tracing_reason.clear(); support.ray_tracing=NativeRtScene::supported(device12.Get(),support.ray_tracing_reason);
            support.ray_reconstruction_reason=support.ray_tracing ? "DLSS Ray Reconstruction runtime unavailable" : support.ray_tracing_reason;
        }
        support.effects=true;support.anisotropy_max=16;        if(streamline && streamline->set_native_device(device,support.dlss_reason)) {
            std::array<uint8_t,8> bytes{};static_assert(sizeof(luid)==bytes.size());std::memcpy(bytes.data(),&luid,sizeof luid);
            support.dlss=streamline->native_dlss_supported(bytes,support.dlss_reason);            if(device12 && support.ray_tracing) {
                support.ray_reconstruction=streamline->native_ray_reconstruction_supported(bytes,support.ray_reconstruction_reason);                if(support.ray_reconstruction) support.ray_reconstruction_reason.clear();
            }
            if(device12 && streamline->frame_generation_loaded()) {
                support.dlss_g=support.dlss && streamline->native_dlss_g_supported(bytes,support.dlss_g_reason);
                if(support.dlss_g) {
                    DlssFrameGeneration generation(*streamline,6);
                    if(generation.state_valid()) support.dlss_g_max_multiplier=generation.multiplier();
                    else { support.dlss_g=false; support.dlss_g_reason="DLSS Frame Generation state query failed"; }
                } else if(!support.dlss) support.dlss_g_reason=support.dlss_reason;
            }
        }
        if(device12 && !support.dlss_g && support.dlss_g_reason.empty()) support.dlss_g_reason=support.dlss_reason;
        if(device12) {
            HMODULE module=LoadLibraryW((directory/"amd_fidelityfx_dx12.dll").c_str());
            if(!module)support.fsr_reason="amd_fidelityfx_dx12.dll missing";
            else {
                ffxFunctions api{};ffxLoadFunctions(&api,module);
                if(!api.CreateContext || !api.DestroyContext || !api.Dispatch)support.fsr_reason="AMD runtime lacks the FFX API";
                else {
                    ffxContext context=nullptr;ffxCreateContextDescUpscale create{};create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                    create.flags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;create.maxRenderSize={64,64};create.maxUpscaleSize={128,128};
                    const auto result=create_native_fsr_context(api,context,device12.Get(),create);support.fsr=result==FFX_API_RETURN_OK;
                    if(!support.fsr)support.fsr_reason="FSR context failed: "+std::to_string(result);
                    if(context)api.DestroyContext(&context,nullptr);
                }
                FreeLibrary(module);
            }
            support.fsr_fg=native_fsr_frame_generation_supported(device12.Get(),directory,support.fsr_fg_reason);
        }
    } catch(const std::exception& error) {
        support.effects=false;support.effects_reason=error.what();support.dlss_reason=support.fsr_reason=error.what();
        if(device12) { support.dlss_g=false; support.fsr_fg=false; support.dlss_g_max_multiplier=0; support.dlss_g_reason=support.fsr_fg_reason=error.what(); }
    }
    // SDK shutdown must precede destruction of its device.
    streamline.reset();return support;
}
}
