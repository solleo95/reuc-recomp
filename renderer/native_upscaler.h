#pragma once
#include "gx_transforms.h"
#include "render_scale.h"
#include "upscale_mode.h"
#include <filesystem>
#include <memory>

struct ID3D12Device;
namespace uc {
class Streamline;
struct NativeUpscaleImage { void* resource=nullptr; uint32_t format=0,state=0; RenderExtent extent{}; };
struct NativeUpscaleInputs {
    void* command=nullptr;
    NativeUpscaleImage color,depth,motion,output;
    bool ray_reconstruction=false;
    NativeUpscaleImage diffuse_albedo,specular_albedo,normal_roughness;
    RenderExtent render_size{},output_size{};
    std::array<float,2> jitter{};
    bool reset=false,sharpening=false;
    float sharpness=.5f,frame_ms=16.7f;
    gx::Projection projection{},clip_to_prev_clip{};
};
class NativeUpscaler {
public:
    virtual ~NativeUpscaler()=default;
    virtual void evaluate(const NativeUpscaleInputs&)=0;
    virtual bool sharpens() const=0;
    virtual RenderExtent minimum_internal(RenderExtent output) const=0;
};
std::unique_ptr<NativeUpscaler> make_native_dlss_upscaler(const Streamline&,UpscaleMode);
std::unique_ptr<NativeUpscaler> make_native_fsr_upscaler(ID3D12Device*,const std::filesystem::path&);
}
