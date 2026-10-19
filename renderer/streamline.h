#pragma once
#include "present_functions.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <memory>
#include <string>

namespace uc {
struct StreamlineApi;   // streamline_api.h: the loaded sl* entry points (DLSS upscaler and frame generation use them)
// NVIDIA Streamline with manual hooking (docs/ProgrammingGuideManualHooking.md): sl.interposer.dll is loaded from the exe folder after its
// NVIDIA signature is checked, slInit runs before the Vulkan instance exists, and only the calls SL intercepts (instance/device creation,
// surface, swapchain, present, device idle) go through its proxies.
class Streamline {
public:
    // nullptr and `reason` when Streamline cannot run (missing or unsigned DLLs, slInit failure).
    static std::unique_ptr<Streamline> load(const std::filesystem::path& directory, bool frame_generation, std::string& reason, bool ray_reconstruction=false);
    ~Streamline();
    Streamline(const Streamline&)=delete;
    Streamline& operator=(const Streamline&)=delete;
    VkResult create_instance(const VkInstanceCreateInfo& info, VkInstance& instance) const;
    VkResult create_device(VkInstance instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) const;
    // Through SL: its vkCreateDevice proxy finds the instance of a physical device only in what its own enumeration recorded.
    VkResult enumerate_physical_devices(VkInstance instance, uint32_t* count, VkPhysicalDevice* devices) const;
    VkResult create_surface(VkInstance instance, void* hwnd, VkSurfaceKHR& surface) const;
    void destroy_surface(VkInstance instance, VkSurfaceKHR surface) const;
    bool dlss_supported(VkPhysicalDevice physical, std::string& reason) const;
    bool ray_reconstruction_supported(VkPhysicalDevice physical, std::string& reason) const;
    bool dlss_g_supported(VkPhysicalDevice physical, std::string& reason) const;
    bool frame_generation_loaded() const { return frame_generation_; }
    bool active() const { return initialized_; }
    PresentFunctions present_functions(VkDevice device) const;
    const StreamlineApi& api() const { return *api_; }
    // One token per guest frame, taken when its first packet renders; the presented frame must carry the same index (DLSS-G guide §7-8).
    void begin_frame(uint32_t index);
    enum class Marker { simulation_start, simulation_end, render_submit_start, render_submit_end, present_start, present_end };
    void marker(Marker marker) const;   // PC latency markers (Reflex/PCL), only with frame generation loaded
    void enable_reflex() const;         // low-latency mode, required by DLSS-G
    void shutdown();   // slShutdown; before the device and instance are destroyed
private:
    Streamline()=default;
    std::unique_ptr<StreamlineApi> api_;
    void* module_=nullptr;
    bool frame_generation_=false, initialized_=false;
};
}
