#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "fsr_frame_generation.h"
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
void on_message(uint32_t type, const wchar_t* message) { std::fprintf(stderr, "FSR FG %s: %ls\n", type==FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message); }
void check(ffxReturnCode_t result, const char* what) {
    if (result!=FFX_API_RETURN_OK) throw std::runtime_error(std::string("FSR FG: ")+what+" failed: code "+std::to_string(result));
}
FfxApiResource resource(const UpscaleImage& image) {   // GENERAL layout: the declared generic-read state maps to it
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.format=image.format; info.extent={image.extent.width,image.extent.height,1};
    info.mipLevels=1; info.arrayLayers=1; info.usage=image.usage;
    return ffxApiGetResourceVK(image.image, ffxApiGetImageResourceDescriptionVK(image.image, info, 0), FFX_API_RESOURCE_STATE_GENERIC_READ);
}
}
struct FsrFrameGeneration::Impl {
    VkDevice device=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE; Queues queues;
    HMODULE module=nullptr; ffxFunctions api{};
    ffxContext swapchain_context=nullptr, context=nullptr;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    ffxQueryDescSwapchainReplacementFunctionsVK replacements{};
    VkExtent2D display{};
    bool active=false, reset=false;
    uint64_t rendered=0, reported=0;   // presented-frame statistics (the swapchain's present counter)

    static ffxReturnCode_t interpolate(ffxDispatchDescFrameGeneration* params, void* user) {
        auto* self=static_cast<Impl*>(user);
        params->reset=self->reset;   // the swapchain fills in everything else (command list, outputs, rectangle, frame id)
        return self->api.Dispatch(&self->context, &params->header);
    }
    void create_swapchain_context(const VkSwapchainCreateInfoKHR& info) {
        ffxCreateContextDescFrameGenerationSwapChainVK create{};
        create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        create.physicalDevice=physical; create.device=device;
        VkSwapchainKHR handle=VK_NULL_HANDLE; create.swapchain=&handle;
        create.createInfo=info; create.createInfo.oldSwapchain=VK_NULL_HANDLE;
        create.gameQueue={queues.game,queues.family,nullptr};
        create.asyncComputeQueue={};   // no async workloads: the interpolation runs on the game queue
        create.presentQueue={queues.present,queues.family,nullptr};
        create.imageAcquireQueue={queues.acquire,queues.family,nullptr};
        check(api.CreateContext(&swapchain_context, &create.header, nullptr), "swapchain context");
        swapchain=handle;
        replacements={}; replacements.header.type=FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        check(api.Query(&swapchain_context, &replacements.header), "swapchain functions");
    }
    VkResult create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
        try {
            if (!swapchain_context) create_swapchain_context(*info);
            else {
                if (swapchain) { replacements.pOutDestroySwapchainFFXAPI(dev, swapchain, allocator, swapchain_context); swapchain=VK_NULL_HANDLE; }   // one at a time
                VkSwapchainCreateInfoKHR copy=*info; copy.oldSwapchain=VK_NULL_HANDLE;
                VkSwapchainKHR handle=VK_NULL_HANDLE;
                const VkResult result=replacements.pOutCreateSwapchainFFXAPI(dev, &copy, allocator, &handle, swapchain_context);
                if (result!=VK_SUCCESS) return result;
                swapchain=handle;
            }
        } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return VK_ERROR_INITIALIZATION_FAILED; }
        *out=swapchain; return VK_SUCCESS;
    }
    void destroy_swapchain(VkDevice dev, VkSwapchainKHR handle, const VkAllocationCallbacks* allocator) {
        if (!handle || handle!=swapchain || !swapchain_context) return;   // a retired chain this context no longer owns
        replacements.pOutDestroySwapchainFFXAPI(dev, handle, allocator, swapchain_context);
        swapchain=VK_NULL_HANDLE;
    }
    void wait_for_presents() {
        if (!swapchain_context || !swapchain) return;   // the chain is gone (shutdown order): nothing is presenting
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
        wait.header.type=FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        api.Dispatch(&swapchain_context, &wait.header);
    }
    void configure(bool enabled, const FfxApiResource& hudless, uint64_t frame) {
        ffxConfigureDescFrameGeneration config{};
        config.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain=swapchain;
        config.frameGenerationCallback=&Impl::interpolate; config.frameGenerationCallbackUserContext=this;
        config.frameGenerationEnabled=enabled;
        config.allowAsyncWorkloads=false;
        config.HUDLessColor=hudless;
        config.generationRect={0,0,int32_t(display.width),int32_t(display.height)};
        config.frameID=frame;
        const auto result=api.Configure(&context, &config.header);
        if (result!=FFX_API_RETURN_OK) std::fprintf(stderr, "FSR FG: configure failed: code %u\n", result);
    }
};
FsrFrameGeneration::FsrFrameGeneration(VkDevice device, VkPhysicalDevice physical, const Queues& queues, const std::filesystem::path& directory) : impl_(std::make_unique<Impl>()) {
    impl_->device=device; impl_->physical=physical; impl_->queues=queues;
    impl_->module=LoadLibraryW((directory / "amd_fidelityfx_vk.dll").c_str());
    if (!impl_->module) throw std::runtime_error("FSR FG: amd_fidelityfx_vk.dll missing");
    ffxLoadFunctions(&impl_->api, impl_->module);
    if (!impl_->api.CreateContext || !impl_->api.Dispatch || !impl_->api.Configure || !impl_->api.Query) throw std::runtime_error("FSR FG: amd_fidelityfx_vk.dll lacks the FFX API");
    if (!queues.game || !queues.present || !queues.acquire) throw std::runtime_error("FSR FG: needs three queues");
}
FsrFrameGeneration::~FsrFrameGeneration() {
    release_context();
    if (impl_->swapchain_context) {
        impl_->api.DestroyContext(&impl_->swapchain_context, nullptr);   // also releases the swapchain when the renderer did not
        impl_->swapchain_context=nullptr;
    }
    if (impl_->module) FreeLibrary(impl_->module);
}
PresentFunctions FsrFrameGeneration::present_functions() {
    PresentFunctions functions;
    Impl* impl=impl_.get();
    functions.create=[impl](VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) { return impl->create_swapchain(device,info,allocator,out); };
    functions.destroy=[impl](VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) { impl->destroy_swapchain(device,swapchain,allocator); };
    functions.images=[impl](VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) { return impl->replacements.pOutGetSwapchainImagesKHR(device,swapchain,count,images); };
    functions.acquire=[impl](VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) { return impl->replacements.pOutAcquireNextImageKHR(device,swapchain,timeout,semaphore,fence,index); };
    functions.present=[impl](VkQueue queue, const VkPresentInfoKHR* info) { return impl->replacements.pOutQueuePresentKHR(queue,info); };
    functions.wait_idle=[impl](VkDevice device) { impl->wait_for_presents(); return vkDeviceWaitIdle(device); };
    return functions;
}
void FsrFrameGeneration::create_context(VkExtent2D display, VkExtent2D max_render, VkFormat backbuffer) {
    auto& i=*impl_;
    release_context();
    i.display=display;
    ffxCreateBackendVKDesc backend{};
    backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice=i.device; backend.vkPhysicalDevice=i.physical; backend.vkDeviceProcAddr=vkGetDeviceProcAddr;
    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.hudlessBackBufferFormat=ffxApiGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    ffxCreateContextDescFrameGeneration create{};
    create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext=&backend.header; backend.header.pNext=&hudless.header;
    create.flags=0;   // SDR, not inverted, finite far plane, vectors in render pixels
    create.displaySize={display.width,display.height};
    create.maxRenderSize={max_render.width,max_render.height};
    create.backBufferFormat=ffxApiGetSurfaceFormatVK(backbuffer);
    check(i.api.CreateContext(&i.context, &create.header, nullptr), "context");
    i.configure(false, {}, 0);
    std::fprintf(stderr, "FSR 3.1 Frame Generation: 2x (one generated frame; other multipliers are DLSS only), %ux%u display, scene %ux%u\n", display.width, display.height, max_render.width, max_render.height);
}
void FsrFrameGeneration::release_context() {
    auto& i=*impl_;
    if (!i.context) return;
    i.wait_for_presents();
    vkDeviceWaitIdle(i.device);
    ffxConfigureDescFrameGeneration off{};   // the swapchain presents plain back buffers while no context drives it
    off.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    off.swapChain=i.swapchain; off.frameGenerationEnabled=false;
    i.api.Configure(&i.context, &off.header);
    i.api.DestroyContext(&i.context, nullptr);
    i.context=nullptr; i.active=false;
}
bool FsrFrameGeneration::has_context() const { return impl_->context!=nullptr; }
bool FsrFrameGeneration::active() const { return impl_->active; }
void FsrFrameGeneration::begin_frame(bool active, const UpscaleImage& hudless, uint64_t frame) {
    auto& i=*impl_;
    if (!i.context) return;
    i.active=active;
    i.configure(active, active ? resource(hudless) : FfxApiResource{}, frame);
}
void FsrFrameGeneration::prepare(const UpscaleInputs& in, uint64_t frame) {
    auto& i=*impl_;
    if (!i.context || !i.active) return;
    ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
    camera.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    camera.cameraUp[1]=1; camera.cameraRight[0]=1; camera.cameraForward[2]=-1;   // view space: the game moves the world, the camera stays at the origin looking down -z
    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.header.pNext=&camera.header;
    prepare.frameID=frame;
    prepare.commandList=in.command;
    prepare.renderSize={in.render_size.width,in.render_size.height};
    prepare.jitterOffset={in.jitter[0],in.jitter[1]};   // as the upscaler gets it
    prepare.motionVectorScale={1.0f,1.0f};
    prepare.frameTimeDelta=in.frame_ms;
    const auto& p=in.projection;   // GX perspective: z' = p22*z + p23, w = -z
    if (p[2][2]!=0 && p[1][1]!=0) {
        prepare.cameraNear=std::abs(p[2][3]/p[2][2]);
        prepare.cameraFar=std::abs(p[2][3]/(p[2][2]+1.0f));
        prepare.cameraFovAngleVertical=2.0f*std::atan(1.0f/p[1][1]);
    } else { prepare.cameraNear=0.1f; prepare.cameraFar=1000.0f; prepare.cameraFovAngleVertical=0.8f; }
    prepare.viewSpaceToMetersFactor=0.0f;
    prepare.depth=resource(in.depth);
    prepare.motionVectors=resource(in.motion);
    i.reset=in.reset;
    const auto result=i.api.Dispatch(&i.context, &prepare.header);
    if (result!=FFX_API_RETURN_OK) std::fprintf(stderr, "FSR FG: prepare failed: code %u\n", result);
}
void FsrFrameGeneration::after_present() {
    auto& i=*impl_;
    if (!i.swapchain || !i.replacements.pOutGetLastPresentCountFFXAPI) return;
    const uint64_t count=i.replacements.pOutGetLastPresentCountFFXAPI(i.swapchain);
    ++i.rendered;
    if (i.active && i.rendered%600==0) {
        std::fprintf(stderr, "FSR Frame Generation: %.2f presented per rendered frame\n", double(count-i.reported)/600.0);
    }
    if (i.rendered%600==0) i.reported=count;
}
}
