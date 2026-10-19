#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "upscaler_probe.h"
#include "streamline.h"
#include "streamline_api.h"
#include <ffx_api/ffx_api_loader.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace uc {
const char* ray_tracing_gap(VkPhysicalDevice physical) {
    uint32_t n=0; vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> extensions(n); vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, extensions.data());
    const auto has=[&](const char* name) { return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name)==0; }); };
    if (!has(VK_KHR_RAY_QUERY_EXTENSION_NAME)) return "the GPU lacks VK_KHR_ray_query";
    if (!has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) return "the GPU lacks VK_KHR_acceleration_structure";
    if (!has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) return "the GPU lacks VK_KHR_deferred_host_operations";
    VkPhysicalDeviceRayQueryFeaturesKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR structure{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; structure.pNext=&query;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; features12.pNext=&structure;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&features12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.limits.maxColorAttachments<5) return "the GPU supports fewer than 5 colour attachments";
    if (!query.rayQuery) return "the GPU lacks VK_KHR_ray_query";
    if (!structure.accelerationStructure || !features12.bufferDeviceAddress) return "the GPU lacks VK_KHR_acceleration_structure";
    if (!features12.descriptorIndexing || !features12.runtimeDescriptorArray || !features12.shaderSampledImageArrayNonUniformIndexing) return "the GPU lacks descriptor indexing";
    return nullptr;
}
namespace {
// DLSS and DLSS-G through Streamline on the first NVIDIA GPU (the one the game picks: discrete first).
void probe_dlss(const std::filesystem::path& directory, UpscalerSupport& support) {
    std::string reason;
    const bool rr=std::filesystem::exists(directory / "sl.dlss_d.dll");
    auto sl=Streamline::load(directory, true, reason, rr);
    std::string rr_failed;
    if (!sl && rr) {   // a Ray Reconstruction plugin that fails slInit must not take DLSS down: retry once without it
        std::string retry_reason;
        sl=Streamline::load(directory, true, retry_reason);
        if (sl) rr_failed="the Ray Reconstruction plugin failed to initialise";
    }
    if (!sl) { support.dlss_reason=support.dlss_g_reason=support.ray_reconstruction_reason=reason; return; }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo=&application;
    const char* debug_utils=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;   // see VulkanRenderer::initialize: Streamline's backend needs it
    instance_info.enabledExtensionCount=1; instance_info.ppEnabledExtensionNames=&debug_utils;
    VkInstance instance=VK_NULL_HANDLE;
    if (sl->create_instance(instance_info, instance)!=VK_SUCCESS) { support.dlss_reason=support.dlss_g_reason=support.ray_reconstruction_reason="Vulkan instance through Streamline failed"; return; }
    uint32_t count=0; sl->enumerate_physical_devices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count); sl->enumerate_physical_devices(instance, &count, devices.data());
    VkPhysicalDevice chosen=VK_NULL_HANDLE; int score=-1;
    for (auto device:devices) {
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(device, &properties);
        const int value=(properties.vendorID==0x10DE)*2+(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (value>score) { score=value; chosen=device; }
    }
    if (!chosen) support.dlss_reason=support.dlss_g_reason=support.ray_reconstruction_reason="no Vulkan GPU";
    else {
        if (rr_failed.empty()) support.ray_reconstruction=sl->ray_reconstruction_supported(chosen, support.ray_reconstruction_reason);
        else support.ray_reconstruction_reason=rr_failed;
        support.dlss=sl->dlss_supported(chosen, support.dlss_reason);
        support.dlss_g=support.dlss && sl->dlss_g_supported(chosen, support.dlss_g_reason);
        if (!support.dlss) support.dlss_g_reason=support.dlss_reason;
    }
    if (support.dlss_g) {   // the multiplier needs a device: one graphics queue, the features the game enables
        uint32_t families=0; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families); vkGetPhysicalDeviceQueueFamilyProperties(chosen, &families, properties.data());
        uint32_t family=0; while (family<families && !(properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        float priority=1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue.queueFamilyIndex=family; queue.queueCount=1; queue.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.dynamicRendering=VK_TRUE; features13.synchronization2=VK_TRUE;
        const char* swapchain=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext=&features13;
        device_info.queueCreateInfoCount=1; device_info.pQueueCreateInfos=&queue; device_info.enabledExtensionCount=1; device_info.ppEnabledExtensionNames=&swapchain;
        VkDevice device=VK_NULL_HANDLE;
        support.dlss_g_max_multiplier=2;   // single frame generation unless the state reports more
        if (family<families && sl->create_device(instance, chosen, device_info, device)==VK_SUCCESS) {
            sl::DLSSGState state{};
            if (UC_SL_CALL(sl::kFeatureDLSS_G, slDLSSGGetState, sl::ViewportHandle(0), state, nullptr)==sl::Result::eOk && state.numFramesToGenerateMax>=1)
                support.dlss_g_max_multiplier=state.numFramesToGenerateMax+1;
            sl->shutdown();
            vkDestroyDevice(device, nullptr);
        }
    }
    sl->shutdown();
    vkDestroyInstance(instance, nullptr);
}
// FSR frame generation needs what the renderer asks of the device: Vulkan 1.3, timeline semaphores and three queues in the graphics family
// (the game's, the interpolation swapchain's present queue and its image-acquire queue).
bool probe_fsr_fg(std::string& reason) {
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo=&application;
    VkInstance instance=VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance)!=VK_SUCCESS) { reason="no Vulkan 1.3 instance"; return false; }
    uint32_t count=0; vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice chosen=VK_NULL_HANDLE; int score=-1;
    for (auto device:devices) {   // the same pick as the game: NVIDIA/discrete first
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(device, &properties);
        const int value=(properties.vendorID==0x10DE)*2+(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (value>score) { score=value; chosen=device; }
    }
    bool ok=false;
    if (!chosen) reason="no Vulkan GPU";
    else {
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&features12;
        vkGetPhysicalDeviceFeatures2(chosen, &features);
        uint32_t families=0; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families); vkGetPhysicalDeviceQueueFamilyProperties(chosen, &families, properties.data());
        uint32_t queues=0;
        for (const auto& family:properties) if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) queues=std::max(queues, family.queueCount);
        if (!features12.timelineSemaphore) reason="the GPU lacks timeline semaphores";
        else if (queues<3) reason="the GPU's graphics queue family has fewer than 3 queues";
        else ok=true;
    }
    vkDestroyInstance(instance, nullptr);
    return ok;
}
// What the renderer asks of the device for the effects (the same pick as the game: NVIDIA/discrete first).
void probe_effects(UpscalerSupport& support) {
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo=&application;
    VkInstance instance=VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance)!=VK_SUCCESS) { support.effects_reason=support.ray_tracing_reason="no Vulkan 1.3 instance"; return; }
    uint32_t count=0; vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice chosen=VK_NULL_HANDLE; int score=-1;
    for (auto device:devices) {
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(device, &properties);
        const int value=(properties.vendorID==0x10DE)*2+(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (value>score) { score=value; chosen=device; }
    }
    if (!chosen) support.effects_reason=support.ray_tracing_reason="no Vulkan GPU";
    else {
        VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unused{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&unused;
        vkGetPhysicalDeviceFeatures2(chosen, &features);
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(chosen, &properties);
        if (!unused.dynamicRenderingUnusedAttachments) support.effects_reason="the GPU lacks VK_EXT_dynamic_rendering_unused_attachments";
        else if (!features.features.independentBlend) support.effects_reason="the GPU lacks independent blending";
        else support.effects=true;
        if (!support.effects) support.ray_tracing_reason="the effects path is unavailable";
        else if (const char* gap=ray_tracing_gap(chosen)) support.ray_tracing_reason=gap;
        else support.ray_tracing=true;
        if (features.features.samplerAnisotropy) {
            uint32_t level=1; while (level<16 && float(level*2)<=properties.limits.maxSamplerAnisotropy) level*=2;
            support.anisotropy_max=level;
        }
    }
    vkDestroyInstance(instance, nullptr);
}
void probe_fsr(const std::filesystem::path& directory, UpscalerSupport& support) {
    const auto path=directory / "amd_fidelityfx_vk.dll";
    HMODULE module=LoadLibraryW(path.c_str());
    if (!module) { support.fsr_reason="amd_fidelityfx_vk.dll missing"; return; }
    ffxFunctions functions{}; ffxLoadFunctions(&functions, module);
    support.fsr=functions.CreateContext && functions.Dispatch && functions.Query;
    if (!support.fsr) support.fsr_reason="amd_fidelityfx_vk.dll lacks the FFX API";
    FreeLibrary(module);
    if (support.fsr) support.fsr_fg=probe_fsr_fg(support.fsr_fg_reason);
}
}
UpscalerSupport probe_upscalers(const std::filesystem::path& directory) {
    UpscalerSupport support;
    probe_dlss(directory, support);
    probe_fsr(directory, support);
    probe_effects(support);
    return support;
}
}
