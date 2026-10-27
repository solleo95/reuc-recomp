#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "upscaler.h"
#include "native_upscaler.h"
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include "ffx_dx12.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
void on_message(uint32_t type, const wchar_t* message) { std::fprintf(stderr, "FSR %s: %ls\n", type==FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message); }
void check(ffxReturnCode_t result, const char* what) {
    if (result!=FFX_API_RETURN_OK) throw std::runtime_error(std::string("FSR: ")+what+" failed: code "+std::to_string(result));
}
FfxApiResource resource(const UpscaleImage& image, uint32_t state, uint32_t extra_usage=0) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.format=image.format; info.extent={image.extent.width,image.extent.height,1};
    info.mipLevels=1; info.arrayLayers=1; info.usage=image.usage;
    return ffxApiGetResourceVK(image.image, ffxApiGetImageResourceDescriptionVK(image.image, info, extra_usage), state);
}
// FSR 3.1 upscaler through the FFX API (amd_fidelityfx_vk.dll next to the exe). All images arrive in VK_IMAGE_LAYOUT_GENERAL; the declared
// states map to GENERAL in the SDK's Vulkan backend, so FFX transitions what it needs and hands them back as they came.
class FsrUpscaler final : public Upscaler,public NativeUpscaler {
public:
    FsrUpscaler(VkDevice device, VkPhysicalDevice physical, const std::filesystem::path& directory)
        : device_(device), physical_(physical) {
        load(directory/"amd_fidelityfx_vk.dll");
    }
    FsrUpscaler(ID3D12Device* device,const std::filesystem::path& directory) : native_device_(device) {load(directory/"amd_fidelityfx_dx12.dll");}
    ~FsrUpscaler() override {
        destroy_context();
        if (module_) FreeLibrary(module_);
    }
    void evaluate(const UpscaleInputs& in) override {
        evaluate(in,in.command,resource(in.color,FFX_API_RESOURCE_STATE_COMPUTE_READ|FFX_API_RESOURCE_STATE_COPY_SRC),
            resource(in.depth,FFX_API_RESOURCE_STATE_COMPUTE_READ|FFX_API_RESOURCE_STATE_COPY_SRC),
            resource(in.motion,FFX_API_RESOURCE_STATE_COMPUTE_READ|FFX_API_RESOURCE_STATE_COPY_SRC),resource(in.output,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    void evaluate(const NativeUpscaleInputs& native) override {
        UpscaleInputs in;
        in.render_size={native.render_size.width,native.render_size.height};in.output_size={native.output_size.width,native.output_size.height};
        in.color.extent={native.color.extent.width,native.color.extent.height};
        in.jitter=native.jitter;in.reset=native.reset;in.sharpening=native.sharpening;in.sharpness=native.sharpness;in.frame_ms=native.frame_ms;in.projection=native.projection;
        // Native callers leave inputs in NON_PIXEL_SHADER_RESOURCE, output in UNORDERED_ACCESS.
        evaluate(in,native.command,native_fsr_resource(static_cast<ID3D12Resource*>(native.color.resource)),
            native_fsr_resource(static_cast<ID3D12Resource*>(native.depth.resource)),
            native_fsr_resource(static_cast<ID3D12Resource*>(native.motion.resource)),
            native_fsr_resource(static_cast<ID3D12Resource*>(native.output.resource),FFX_API_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    void evaluate(const UpscaleInputs& in,void* command,FfxApiResource color,FfxApiResource depth,FfxApiResource motion,FfxApiResource output) {
        if (!context_ || in.output_size.width>max_output_.width || in.output_size.height>max_output_.height ||
            in.render_size.width>max_render_.width || in.render_size.height>max_render_.height || in.color.extent.width!=scene_target_.width || in.color.extent.height!=scene_target_.height) {
            create_context(in);
        }
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList=command;
        dispatch.color=color;dispatch.depth=depth;dispatch.motionVectors=motion;dispatch.output=output;
        // As added to the geometry (pixel space, y down); the same value DLSS gets, proven against the exact edge coverage of a still scene.
        dispatch.jitterOffset={in.jitter[0], in.jitter[1]};
        dispatch.motionVectorScale={1.0f, 1.0f};   // vectors are already previous minus current in internal pixels
        dispatch.renderSize={in.render_size.width, in.render_size.height};
        dispatch.upscaleSize={in.output_size.width, in.output_size.height};
        dispatch.enableSharpening=in.sharpening;
        dispatch.sharpness=in.sharpness;
        dispatch.frameTimeDelta=in.frame_ms;
        dispatch.preExposure=1.0f;
        dispatch.reset=in.reset;
        const auto& p=in.projection;   // GX perspective: z' = p22*z + p23, w = -z
        if (p[2][2]!=0 && p[1][1]!=0) {
            dispatch.cameraNear=std::abs(p[2][3]/p[2][2]);
            dispatch.cameraFar=std::abs(p[2][3]/(p[2][2]+1.0f));
            dispatch.cameraFovAngleVertical=2.0f*std::atan(1.0f/p[1][1]);
        } else { dispatch.cameraNear=0.1f; dispatch.cameraFar=1000.0f; dispatch.cameraFovAngleVertical=0.8f; }
        dispatch.viewSpaceToMetersFactor=1.0f;
        dispatch.flags=FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;   // RGBA8 UNORM holding gamma-encoded colour
        check(api_.Dispatch(&context_, &dispatch.header), "dispatch");
    }
    bool sharpens() const override { return true; }   // the RCAS pass of FSR is part of the dispatch (a disabled slider just turns it off)
    RenderExtent minimum_internal(RenderExtent) const override { return {1,1}; }
private:
    void load(const std::filesystem::path& path) {
        module_=LoadLibraryW(path.c_str());
        if(!module_)throw std::runtime_error("FSR: missing runtime "+path.filename().string());
        ffxLoadFunctions(&api_,module_);
        if(!api_.CreateContext || !api_.Dispatch || !api_.DestroyContext) {FreeLibrary(module_);module_=nullptr;throw std::runtime_error("FSR: runtime lacks the FFX API");}
    }
    void destroy_context() {
        if (context_) { api_.DestroyContext(&context_, nullptr); context_=nullptr; }
    }
    void create_context(const UpscaleInputs& in) {
        destroy_context();
        max_output_={in.output_size.width, in.output_size.height};
        max_render_={in.render_size.width, in.render_size.height};
        scene_target_=in.color.extent;
        ffxCreateBackendVKDesc backend{};
        backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice=device_; backend.vkPhysicalDevice=physical_; backend.vkDeviceProcAddr=vkGetDeviceProcAddr;
        ffxCreateContextDescUpscale create{};
        create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext=&backend.header;
        create.flags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;   // LDR game: no exposure texture
        create.maxRenderSize={max_render_.width, max_render_.height};
        create.maxUpscaleSize={max_output_.width, max_output_.height};
        create.fpMessage=on_message;
        check(native_device_ ? create_native_fsr_context(api_,context_,native_device_,create) : api_.CreateContext(&context_, &create.header, nullptr), "context");
        std::fprintf(stderr, "FSR 3.1: %ux%u <- %ux%u\n", max_output_.width, max_output_.height, max_render_.width, max_render_.height);
    }
    VkDevice device_=VK_NULL_HANDLE; VkPhysicalDevice physical_=VK_NULL_HANDLE;
    ID3D12Device* native_device_=nullptr;
    HMODULE module_=nullptr;
    ffxFunctions api_{};
    ffxContext context_=nullptr;
    RenderExtent max_output_{}, max_render_{};
    VkExtent2D scene_target_{};
};
}
std::unique_ptr<Upscaler> make_fsr_upscaler(VkDevice device, VkPhysicalDevice physical, const std::filesystem::path& directory) {
    return std::make_unique<FsrUpscaler>(device,physical,directory);
}
std::unique_ptr<NativeUpscaler> make_native_fsr_upscaler(ID3D12Device* device,const std::filesystem::path& directory) {return std::make_unique<FsrUpscaler>(device,directory);}
}
