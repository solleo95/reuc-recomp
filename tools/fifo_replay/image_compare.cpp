#include "image_compare.h"
#include <algorithm>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace fifo_replay {
namespace {
size_t pixel_bytes(uint32_t width,uint32_t height) {
    uint64_t count=uint64_t(width)*height;
    if(!width || !height || count>64*1024*1024)throw std::invalid_argument("reference image must contain 1..64M pixels");
    return size_t(count)*4;
}
}
uc::RenderPixels load_reference(const std::filesystem::path& path) {
    auto require=[](HRESULT hr) {if(FAILED(hr))throw std::runtime_error("reference image decoding failed: HRESULT "+std::to_string(uint32_t(hr)));};
    struct Session {
        HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ~Session(){if(SUCCEEDED(result))CoUninitialize();}
    } session;
    if(session.result!=RPC_E_CHANGED_MODE)require(session.result);
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    require(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICBitmapDecoder> decoder;
    require(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder));
    ComPtr<IWICBitmapFrameDecode> frame;require(decoder->GetFrame(0,&frame));
    uc::RenderPixels result;require(frame->GetSize(&result.width,&result.height));
    result.rgba.resize(pixel_bytes(result.width,result.height));
    ComPtr<IWICFormatConverter> converter;require(factory->CreateFormatConverter(&converter));
    require(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    require(converter->CopyPixels(nullptr,result.width*4,UINT(result.rgba.size()),result.rgba.data()));
    return result;
}
ImageComparison compare_images(const uc::RenderPixels& actual,const uc::RenderPixels& reference) {
    auto bytes=pixel_bytes(actual.width,actual.height);
    if(actual.width!=reference.width || actual.height!=reference.height || actual.rgba.size()!=bytes || reference.rgba.size()!=bytes)
        throw std::invalid_argument("image comparison requires equal dimensions and complete RGBA data");
    ImageComparison result;result.diff={actual.serial,actual.width,actual.height,std::vector<uint8_t>(bytes,255)};
    uint64_t sum=0;
    for(size_t p=0;p<bytes;p+=4) {
        bool changed=false;
        for(size_t channel=0;channel<3;++channel) {
            auto error=uint32_t(std::abs(int(actual.rgba[p+channel])-int(reference.rgba[p+channel])));
            result.diff.rgba[p+channel]=uint8_t(error);sum+=error;result.peak=std::max(result.peak,error);changed|=error!=0;
        }
        result.changed_pixels+=changed;
    }
    result.mean=double(sum)/double(bytes/4*3);return result;
}
}
