#include "vulkan_renderer.h"
#include <algorithm>
#include <limits>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace uc {
void save_png(const RenderPixels& pixels,const std::filesystem::path& path) {
    if (!pixels.width || !pixels.height || pixels.width>UINT32_MAX/4 ||
        pixels.height>std::numeric_limits<size_t>::max()/(size_t(pixels.width)*4) ||
        pixels.rgba.size()!=size_t(pixels.width)*pixels.height*4 || pixels.rgba.size()>UINT32_MAX)
        throw std::invalid_argument("invalid PNG readback dimensions/data");
    auto require=[](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("PNG encoding failed: HRESULT "+std::to_string(static_cast<uint32_t>(hr))); };
    struct ComSession {
        HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    if (com.result!=RPC_E_CHANGED_MODE) require(com.result);
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    require(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICStream> stream; require(factory->CreateStream(&stream));
    require(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; require(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
    require(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; require(encoder->CreateNewFrame(&frame,nullptr));
    require(frame->Initialize(nullptr)); require(frame->SetSize(pixels.width,pixels.height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    require(frame->SetPixelFormat(&format));
    if (format!=GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed required pixel format");
    auto bgra=pixels.rgba;
    for (size_t i=0;i<bgra.size();i+=4) std::swap(bgra[i],bgra[i+2]);
    require(frame->WritePixels(pixels.height,pixels.width*4,UINT(bgra.size()),bgra.data()));
    require(frame->Commit()); require(encoder->Commit());
}
}
