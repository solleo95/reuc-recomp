#pragma once
#include "dlss5_settings.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace uc {
// DLSS 5 Neural Rendering through the standard NGX route (docs/superpowers/specs/2026-10-20-dlss5-neural-rendering-design.md). The runtime nvngx_dlssnr.dll is
// loaded by the driver's NGX core from the exe directory; nothing here touches it. NgxBackend is the thin seam to the NGX SDK (ngx_vulkan.cpp); the logic
// above it (support check, failure reasons, parameter names, feature lifetime) is tested with a fake.
constexpr uint32_t kNgxFeatureDlss5 = 18;         // NVSDK_NGX_Feature_Reserved18
constexpr uint32_t kNgxSuccess = 1;               // NVSDK_NGX_Result_Success; failures are 0xBADxxxxx
constexpr const char* kDlss5Runtime = "nvngx_dlssnr.dll";
struct NgxImage { VkImage image=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkFormat format=VK_FORMAT_UNDEFINED; VkExtent2D extent{}; bool read_write=false; VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT; };
// The parameter object an NGX call reads (a recording fake in the tests).
class NgxParameters {
public:
    virtual ~NgxParameters()=default;
    virtual void set_uint(const char* name, uint32_t value)=0;
    virtual void set_int(const char* name, int value)=0;
    virtual void set_float(const char* name, float value)=0;
    virtual void set_image(const char* name, const NgxImage& image)=0;
};
struct NgxRequirement { uint32_t unsupported_bits=0; uint32_t min_architecture=0; std::string min_os; };   // NVSDK_NGX_Feature_Support_Result bits; 0 = supported
struct NgxInit {
    VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr instance_proc=nullptr; PFN_vkGetDeviceProcAddr device_proc=nullptr;
    std::filesystem::path runtime_directory;   // where nvngx_dlssnr.dll lives (the exe directory)
    std::filesystem::path log_file;            // the NGX log callback appends here (debug/)
    bool verbose=false;
};
class NgxBackend {
public:
    virtual ~NgxBackend()=default;
    virtual uint32_t init(const NgxInit&)=0;                          // NGX result, kNgxSuccess when ready
    virtual uint32_t requirements(NgxRequirement&)=0;                 // NVSDK_NGX_VULKAN_GetFeatureRequirements for the DLSS 5 feature
    virtual std::unique_ptr<NgxParameters> allocate_parameters(uint32_t& result)=0;
    virtual uint32_t create_feature(VkCommandBuffer, NgxParameters&, void*& handle)=0;
    virtual uint32_t evaluate(VkCommandBuffer, void* handle, NgxParameters&)=0;
    virtual void release_feature(void* handle)=0;
    virtual void shutdown()=0;
    virtual std::string diagnostic() const=0;                         // the most informative recent NGX log line, empty when none
};
std::unique_ptr<NgxBackend> make_ngx_backend();                                              // ngx_vulkan.cpp
// Vulkan extensions the feature needs (NGX is asked; no init needed). Instance extensions before vkCreateInstance, device ones for the chosen physical device.
std::vector<std::string> ngx_dlss5_instance_extensions(const std::filesystem::path& runtime_directory);
std::vector<std::string> ngx_dlss5_device_extensions(VkInstance, VkPhysicalDevice, const std::filesystem::path& runtime_directory);
// Reasons shown in the UI (English, like the other support reasons).
std::string ngx_result_name(uint32_t result);                                                // "FAIL_UnableToInitializeFeature (0xbad0000b)"
std::string dlss5_requirement_reason(const NgxRequirement&);                                 // empty when supported
std::string dlss5_failure_reason(const char* step, uint32_t result, std::string_view log_line);
std::string ngx_informative_line(const std::vector<std::string>& recent);                    // the line that explains a failure best
// The parameters of one feature creation and one run, with the names nvngx_dlssnr.dll reads.
struct Dlss5FeatureKey {
    uint32_t width=0, height=0; Dlss5Preset preset=Dlss5Preset::p1;
    bool operator==(const Dlss5FeatureKey&) const=default;
};
struct Dlss5Run {
    NgxImage color, depth, motion, output;     // color/output at the processing size, depth and motion at the scene's render size
    VkExtent2D color_region{}, depth_region{}, motion_region{}, output_region{};   // top-left sub-rectangles holding the picture
    float jitter[2]{};                         // in colour pixels, as added to the projection (0 once an upscaler resolved it)
    float motion_scale[2]{1,1};                // motion vector pixels to colour pixels
    bool reset=false;
    Dlss5Settings settings;
};
void dlss5_create_parameters(NgxParameters&, const Dlss5FeatureKey&);
void dlss5_run_parameters(NgxParameters&, const Dlss5Run&);
// The feature on a device: support check, creation (one instance per pass, each with its own temporal history) and the runs. Failures never throw: `reason`
// tells the player what is missing and the runtime stays unusable.
class Dlss5Runtime {
public:
    // Creates the NGX session. Fails with a reason when the runtime file is missing, NGX refuses the GPU/driver, or init fails.
    static std::unique_ptr<Dlss5Runtime> open(std::unique_ptr<NgxBackend> backend, const NgxInit&, std::string& reason);
    ~Dlss5Runtime();
    // Makes `passes` features for `key` (re-creating them when the key changed). `command` is open. false: `reason()` says why; the same key is not retried.
    bool prepare(VkCommandBuffer command, const Dlss5FeatureKey& key, uint32_t passes, const std::function<void()>& idle);
    bool evaluate(VkCommandBuffer command, uint32_t pass, const Dlss5Run&);
    const std::string& reason() const { return reason_; }
    uint32_t passes() const { return uint32_t(handles_.size()); }
private:
    Dlss5Runtime()=default;
    void release();
    std::unique_ptr<NgxBackend> backend_;
    std::vector<void*> handles_;
    std::unique_ptr<NgxParameters> run_parameters_;
    Dlss5FeatureKey key_; uint32_t wanted_passes_=0; bool failed_=false;
    std::string reason_;
};
}
