#pragma once
#include "dlss5_settings.h"
#include "ngx_dlss5.h"
#include "upscaler.h"
#include <vulkan/vulkan.h>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace uc {
// DLSS 5 Neural Rendering on a region of the finished picture (docs/superpowers/specs/2026-10-20-dlss5-neural-rendering-design.md): a compute pass brings the
// RGBA8 region to the network's signal at the processing size, NGX runs the feature `passes` times, a second compute pass writes the result back.
struct Dlss5Context {   // the device the pass works on
    VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr instance_proc=nullptr; PFN_vkGetDeviceProcAddr device_proc=nullptr;
    VkPhysicalDeviceMemoryProperties memory{};
    std::filesystem::path shaders;     // data/shaders
    std::filesystem::path directory;   // the exe directory: nvngx_dlssnr.dll, debug/
    std::function<void()> idle;        // waits for every submission (before something in use is replaced)
    std::function<std::unique_ptr<NgxBackend>()> backend;   // tests: a stand-in for NGX; empty = the real SDK
};
struct Dlss5Frame {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage picture;                       // RGBA8 in VK_IMAGE_LAYOUT_GENERAL, read and written
    VkOffset2D origin{}; VkExtent2D size{};     // the region of the picture the network sees
    UpscaleImage depth, motion;                 // the scene targets (GENERAL), motion in internal pixels
    VkExtent2D scene_size{};                    // the part of them that holds the picture
    std::array<float,2> jitter{};               // internal pixels, as added to the projection; zero once an upscaler resolved the picture
    bool reset=false;                           // history is invalid
    uint64_t targets_generation=0;              // changes when the images above were recreated (a new image may reuse a destroyed one's handle)
    Dlss5Settings settings;
};
class Dlss5Pass {
public:
    ~Dlss5Pass() { shutdown(); }
    void configure(const Dlss5Context& context) { context_=context; }
    // Runs the pass; false when it did nothing (no runtime, feature refused, or a failed run): status() says why and the pass stays off until the settings change.
    bool record(const Dlss5Frame& frame);
    const std::string& status() const { return status_; }   // empty while it works
    bool failed() const { return failed_; }
    bool ran() const { return ran_; }                        // the latest record() ran the network
    void settings_changed() { if(runtime_) failed_=false; ran_=false; }   // a new setting may repair a failed run (the feature key is compared again)
    void shutdown();
private:
    struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
    void ensure_pipelines();
    void ensure_images(VkExtent2D extent);
    void bind(VkImageView picture, uint64_t generation);
    Image make_image(VkExtent2D extent);
    void destroy(Image&);
    void fail(const std::string& reason);
    Dlss5Context context_;
    std::unique_ptr<Dlss5Runtime> runtime_;
    bool open_failed_=false, failed_=false, ran_=false, bridge_ready_=false;
    std::string status_;
    VkShaderModule prepare_module_=VK_NULL_HANDLE, resolve_module_=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline prepare_=VK_NULL_HANDLE, resolve_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet prepare_set_=VK_NULL_HANDLE, resolve_from_[2]{};   // picture -> A, A -> picture, B -> picture
    Image bridge_[2]{};
    VkExtent2D bridge_extent_{};
    VkImageView bound_picture_=VK_NULL_HANDLE;
    uint64_t bound_generation_=0, frames_=0;
};
}
