#include "late_aim.h"
#include "post_effects.h"
#include "vulkan_renderer.h"
#include "render_scale.h"
#include "rebuild_gate.h"
#include "buffer_memory.h"
#include "upscale_math.h"
#include "upscaler_probe.h"
#include <tuple>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace uc {
namespace {
// Per-draw uniform block: the GX draw state followed by the pipeline key words the ubershader reads (key[1..67], padded to 17 vec4).
constexpr size_t kTevWords=68, kDrawUniformBytes=sizeof(gx::DrawUniform)+kTevWords*sizeof(uint32_t);
const std::shared_ptr<const gx::TexturePixels>& draw_texture(const gx::DrawPacket& draw,size_t map) { return map ? draw.texture_maps[map-1] : draw.texture; }
void vk_check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
             VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT,uint32_t levels=1,uint32_t base_mip=0) {
    VkImageMemoryBarrier2 image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image_barrier.srcStageMask = source; image_barrier.srcAccessMask = source_access;
    image_barrier.dstStageMask = destination; image_barrier.dstAccessMask = destination_access;
    image_barrier.oldLayout = before; image_barrier.newLayout = after;
    image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image; image_barrier.subresourceRange = {aspect, base_mip, levels, 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &image_barrier;
    vkCmdPipelineBarrier2(command, &info);
}
VkShaderModule shader(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (!file || size <= 0 || size % 4 != 0) throw std::runtime_error("invalid or missing SPIR-V: " + path.string());
    std::vector<uint32_t> words(size_t(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file || words[0] != 0x07230203) throw std::runtime_error("invalid SPIR-V header: " + path.string());
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = size_t(size); info.pCode = words.data();
    VkShaderModule module; vk_check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule"); return module;
}
}
void VulkanRenderer::owner() const {
    if (std::this_thread::get_id() != owner_) throw std::logic_error("Vulkan operation requires renderer owner thread");
}
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user) {
    auto& renderer = *static_cast<VulkanRenderer*>(user);
    // Motion-variant pipelines also run in passes without a vector attachment; VK_EXT_dynamic_rendering_unused_attachments makes that legal, the layer still reports it.
    // FidelityFX 1.1.4 declares its luma history storage image as Rgba8 in the shader and views an R16G16 image: a format the SDK mixes on purpose (same texel size).
    const bool known = std::strstr(message->pMessage, "this write is unused") != nullptr || std::strstr(message->pMessage, "rw_luma_history") != nullptr;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && !known) renderer.errors_.fetch_add(1);
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
    return VK_FALSE;
}
VulkanRenderer::~VulkanRenderer() {
    try { shutdown(); } catch (const std::exception& error) { std::fprintf(stderr, "Vulkan shutdown failed: %s\n", error.what()); }
}
void VulkanRenderer::initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders, double render_scale, double render_aspect) {
    if (instance_) throw std::logic_error("renderer already initialized");
    owner_ = std::this_thread::get_id(); window_ = window;
    render_aspect_=render_aspect;
    set_render_scale(render_scale);
    if (!window) throw std::invalid_argument("Vulkan requires an SDL window");
    uint32_t version = 0; vk_check(vkEnumerateInstanceVersion(&version), "vkEnumerateInstanceVersion");
    if (version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 loader required");
    Uint32 count = 0; const char* const* required = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!required) throw std::runtime_error(SDL_GetError());
    std::vector<const char*> extensions(required, required + count);
    uint32_t available_count = 0; vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr), "instance extension count");
    std::vector<VkExtensionProperties> available(available_count);
    vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data()), "instance extensions");
    auto has = [&](const char* name) { return std::any_of(available.begin(), available.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; }); };
    bool maintenance_khr = has("VK_KHR_surface_maintenance1"), maintenance_ext = has("VK_EXT_surface_maintenance1");
    if (!has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) || (!maintenance_khr && !maintenance_ext))
        throw std::runtime_error("surface maintenance1 required for verified presentation retirement");
    extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (maintenance_khr) extensions.push_back("VK_KHR_surface_maintenance1");
    if (maintenance_ext) extensions.push_back("VK_EXT_surface_maintenance1");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        uint32_t n = 0; vk_check(vkEnumerateInstanceLayerProperties(&n, nullptr), "layer count");
        std::vector<VkLayerProperties> layers(n); vk_check(vkEnumerateInstanceLayerProperties(&n, layers.data()), "layers");
        if (!std::any_of(layers.begin(), layers.end(), [&](const auto& item) { return std::strcmp(item.layerName, layer) == 0; }))
            throw std::runtime_error("VK_LAYER_KHRONOS_validation unavailable");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.pApplicationName = "Umbrella Chronicles"; application.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = debug; debug_info.pUserData = this;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = uint32_t(extensions.size()); instance_info.ppEnabledExtensionNames = extensions.data();
    if (validation) { instance_info.enabledLayerCount = 1; instance_info.ppEnabledLayerNames = &layer; instance_info.pNext = &debug_info; }
    {   // Optional backends must be ready before the instance, so live picks never need a new device.
        std::string reason;
        streamline_ = Streamline::load(shaders.parent_path().parent_path(), true, reason);
        if (!streamline_) {
            std::fprintf(stderr, "DLSS unavailable: %s\n", reason.c_str());
            if (upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        }
        else if (!validation && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            // Streamline's Vulkan backend creates a debug messenger through the loader trampoline, which crashes when the extension is off.
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            instance_info.enabledExtensionCount = uint32_t(extensions.size()); instance_info.ppEnabledExtensionNames = extensions.data();
        }
    }
    vk_check(streamline_ ? streamline_->create_instance(instance_info, instance_) : vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance");
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!create) throw std::runtime_error("debug-utils creation function unavailable");
        vk_check(create(instance_, &debug_info, nullptr, &messenger_), "debug messenger");
    }
    if (streamline_) {   // the surface is one of the calls Streamline intercepts
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd) throw std::runtime_error("window has no native handle");
        vk_check(streamline_->create_surface(instance_, hwnd, surface_), "Streamline surface");
    } else if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) throw std::runtime_error(SDL_GetError());
    const auto enumerate = [&](uint32_t* count, VkPhysicalDevice* devices) { return streamline_ ? streamline_->enumerate_physical_devices(instance_, count, devices) : vkEnumeratePhysicalDevices(instance_, count, devices); };
    uint32_t n = 0; vk_check(enumerate(&n, nullptr), "physical device count");
    std::vector<VkPhysicalDevice> physical(n); vk_check(enumerate(&n, physical.data()), "physical devices");
    const char* chosen_maintenance = nullptr; int score = -1;
    for (auto candidate : physical) {
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t extension_count = 0; vk_check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr), "device extension count");
        std::vector<VkExtensionProperties> device_extensions(extension_count);
        vk_check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, device_extensions.data()), "device extensions");
        auto device_has = [&](const char* name) { return std::any_of(device_extensions.begin(), device_extensions.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; }); };
        const char* maintenance = maintenance_khr && device_has("VK_KHR_swapchain_maintenance1") ? "VK_KHR_swapchain_maintenance1" : maintenance_ext && device_has("VK_EXT_swapchain_maintenance1") ? "VK_EXT_swapchain_maintenance1" : nullptr;
        if (!device_has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !maintenance) continue;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR retirement{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.pNext = &retirement;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!features13.dynamicRendering || !features13.synchronization2 || !retirement.swapchainMaintenance1) continue;
        uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (uint32_t family = 0; family < family_count; ++family) {
            VkBool32 present = VK_FALSE; vk_check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &present), "surface queue support");
            int candidate_score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (present && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && candidate_score > score) {
                physical_ = candidate; family_ = family; chosen_maintenance = maintenance; score = candidate_score;
            }
        }
    }
    if (!physical_) throw std::runtime_error("GPU requires Vulkan 1.3 graphics/present, dynamic rendering, synchronization2 and swapchain maintenance1");
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_, &properties);
    max_image_dimension_=properties.limits.maxImageDimension2D;
    std::fprintf(stderr, "Vulkan device: %s; presentation retirement: %s\n", properties.deviceName, chosen_maintenance);
    if (streamline_) {
        std::string reason;
        dlss_available_=streamline_->dlss_supported(physical_, reason);
        if (!dlss_available_) {
            std::fprintf(stderr, "DLSS unavailable: %s\n", reason.c_str());
            streamline_->shutdown();
            if (upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        } else {
            dlss_g_available_=streamline_->dlss_g_supported(physical_,reason);
            if (!dlss_g_available_ && upscaler_kind_==UpscalerKind::dlss) frame_generation_=false;
        }
    }
    bool through_streamline = streamline_ && streamline_->active();
    bool private_data = false;   // Streamline keeps swapchain data in a VkPrivateDataSlot
    if (through_streamline) {
        VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext = &supported13;
        vkGetPhysicalDeviceFeatures2(physical_, &query);
        private_data = supported13.privateData;
        if (!private_data) {
            std::fprintf(stderr,"DLSS unavailable: Streamline presentation requires Vulkan privateData\n");
            streamline_->shutdown(); through_streamline=false; dlss_available_=dlss_g_available_=false;
            if(upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        }
    }
    const float priorities[3] = {1, 1, 1};
    uint32_t queue_count = 1;
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 query12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query12.pNext=&supported12;
    vkGetPhysicalDeviceFeatures2(physical_,&query12);
    {   // Optional interpolation queues are provisioned once, even when generation starts disabled.
        uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count); vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
        if (families[family_].queueCount>=3 && supported12.timelineSemaphore) queue_count=3;
        else if (upscaler_kind_==UpscalerKind::fsr) frame_generation_=false;
    }
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue_info.queueFamilyIndex = family_; queue_info.queueCount = queue_count; queue_info.pQueuePriorities = priorities;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR retirement{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR}; retirement.swapchainMaintenance1 = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.pNext = &retirement; features13.dynamicRendering = VK_TRUE; features13.synchronization2 = VK_TRUE;
    features13.privateData = private_data;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = queue_count == 3;   // the interpolation swapchain's queue sync
    features12.shaderFloat16=supported12.shaderFloat16;
    if (features12.timelineSemaphore || features12.shaderFloat16) { features12.pNext = features13.pNext; features13.pNext = &features12; }
    std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME, chosen_maintenance};
    {   // FidelityFX resolves these entry points by their extension names, including after a live switch.
        uint32_t n = 0; vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> supported(n); vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, supported.data());
        for (const char* name : {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME})
            if (std::any_of(supported.begin(), supported.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; })) device_extensions.push_back(name);
        if (through_streamline && std::any_of(supported.begin(),supported.end(),[](const auto& extension) { return std::strcmp(extension.extensionName,VK_KHR_PRESENT_ID_EXTENSION_NAME)==0; }))
            device_extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);   // dependency of the low-latency extension Streamline enables
    }
    // Upscaling: pipelines with the motion attachment also draw passes that bind none (post chain, HUD), which needs unused attachments.
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unused{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext = &unused;
        vkGetPhysicalDeviceFeatures2(physical_, &query);
        VkPhysicalDeviceFeatures core_features{}; vkGetPhysicalDeviceFeatures(physical_, &core_features);
        if (unused.dynamicRenderingUnusedAttachments && core_features.independentBlend) { device_extensions.push_back(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME); unused.pNext = features13.pNext; features13.pNext = &unused; motion_outputs_ = true; }
        else { std::fprintf(stderr, "Upscaling unavailable: VK_EXT_dynamic_rendering_unused_attachments or independentBlend missing\n"); upscaler_kind_ = UpscalerKind::off; frame_generation_ = false; }
    }
    // Ray tracing (ray query in compute/fragment shaders): all of it or none, so a GPU without it still creates the device.
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR}; ray_query.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; acceleration.accelerationStructure = VK_TRUE;
    ray_tracing_ = motion_outputs_ && !ray_tracing_gap(physical_);
    if (ray_tracing_) {
        for (const char* name : {VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME}) device_extensions.push_back(name);
        if (!features12.timelineSemaphore && !features12.shaderFloat16) { features12.pNext = features13.pNext; features13.pNext = &features12; }
        features12.bufferDeviceAddress = features12.descriptorIndexing = features12.runtimeDescriptorArray = features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        acceleration.pNext = &ray_query; ray_query.pNext = features13.pNext; features13.pNext = &acceleration;
    }
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext = &features13;
    VkPhysicalDeviceFeatures core{}; vkGetPhysicalDeviceFeatures(physical_,&core);
    logic_ops_=core.logicOp;
    VkPhysicalDeviceFeatures enabled{}; enabled.logicOp=logic_ops_; enabled.independentBlend=motion_outputs_; enabled.shaderInt16=core.shaderInt16; device_info.pEnabledFeatures=&enabled;
    enabled.samplerAnisotropy=core.samplerAnisotropy; anisotropy_supported_=core.samplerAnisotropy;
    { VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_,&properties); anisotropy_limit_=properties.limits.maxSamplerAnisotropy; }
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;   // queue_info.queueCount: 3 with the interpolation swapchain
    device_info.enabledExtensionCount = uint32_t(device_extensions.size()); device_info.ppEnabledExtensionNames = device_extensions.data();
    vk_check(through_streamline ? streamline_->create_device(instance_, physical_, device_info, device) : vkCreateDevice(physical_, &device_info, nullptr, &device), "vkCreateDevice");
    if (through_streamline) base_present_=streamline_->present_functions(device);
    if (upscaler_kind_==UpscalerKind::dlss && frame_generation_ && dlss_g_available_) {
        dlss_presentation_=true; present_=base_present_;
    }
    vkGetDeviceQueue(device, family_, 0, &queue_); vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    if (queue_count == 3 && motion_outputs_) {
        vkGetDeviceQueue(device, family_, 1, &fg_present_queue_); vkGetDeviceQueue(device, family_, 2, &fg_acquire_queue_);
        try {
            fsr_g_ = std::make_unique<FsrFrameGeneration>(device, physical_, FsrFrameGeneration::Queues{queue_, fg_present_queue_, fg_acquire_queue_, family_}, shaders.parent_path().parent_path());
            if (upscaler_kind_==UpscalerKind::fsr && frame_generation_) { fsr_presentation_=true; present_=fsr_g_->present_functions(); }
        } catch (const std::exception& error) { std::fprintf(stderr, "FSR Frame Generation unavailable: %s\n", error.what()); if(upscaler_kind_==UpscalerKind::fsr)frame_generation_=false; }
    }
    VkPhysicalDeviceProperties limits{}; vkGetPhysicalDeviceProperties(physical_,&limits);
    const auto alignment=size_t(limits.limits.minUniformBufferOffsetAlignment);
    uniform_stride_=(kDrawUniformBytes+alignment-1)/alignment*alignment;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool_info.queueFamilyIndex = family_;
    vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &pool_), "command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool = pool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = uint32_t(slots_.size());
    VkCommandBuffer commands[kFrameSlots]; vk_check(vkAllocateCommandBuffers(device, &allocation, commands), "command buffers");
    VkQueryPoolCreateInfo timing{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; timing.queryType = VK_QUERY_TYPE_TIMESTAMP; timing.queryCount = uint32_t(slots_.size() * 2);
    vk_check(vkCreateQueryPool(device, &timing, nullptr, &timing_pool_), "timing query pool");
    timestamp_period_ = limits.limits.timestampPeriod;
    pass_timer_.initialize(device, timestamp_period_, uint32_t(slots_.size()));
    for (size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].command = commands[i];
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(device, &fence, nullptr, &slots_[i].fence), "frame fence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &slots_[i].acquired), "acquire semaphore");
    }
    shaders_=shaders; cache_dir_=shaders_.parent_path();
    const std::string variant=motion_outputs_ ? "smoke.motion." : "smoke.";
    if (motion_outputs_) { shadow_vertex_module_=shader(device, shaders_ / "smoke.shadow.vert.spv"); shadow_map_.initialize(device,memory_); }
    vertex_module_=shader(device, shaders_ / (variant+"vert.spv")); fragment_module_=shader(device, shaders_ / (variant+"frag.spv")); uber_module_=shader(device, shaders_ / (variant+"uber.spv"));
    load_pipeline_cache();
    if (motion_outputs_ && sharpening_) rcas_.initialize(device, memory_, shaders_);
    gx::DrawPacket initial; initial.primitive=0x90; pipeline_for(pipeline_key(initial));
    start_workers();
    int width, height; if (!SDL_GetWindowSizeInPixels(window, &width, &height)) throw std::runtime_error(SDL_GetError());
    resize(uint32_t(std::max(0, width)), uint32_t(std::max(0, height)));
    if (requested_.width && requested_.height) rebuild();
}
// Push constants of every draw: clip matrix plus where to find this draw's matrices (see smoke.slang).
constexpr uint32_t kNoBank=UINT32_MAX;
// bank: texture bank index, kNoBank = the texgen's own matrix. motion (kNoMotion = none), jitter (target pixels) and viewport (target pixels)
// are read by the upscaling variant only.
constexpr uint32_t kNoMotion=UINT32_MAX;
constexpr VkFormat kMotionFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGBufferFormat=VK_FORMAT_R16G16B16A16_UNORM;   // gbuffer.h
constexpr float kShadowDistance=30.f;   // how far from the camera the shadow map reaches, in view units (metres)
struct DrawPush { gx::Projection projection; uint32_t palette, current_row, indexed, alpha_pass, bank, motion=kNoMotion; float jitter[2]{}; float viewport[4]{}; uint32_t effects=0, record=kRtNoRecord, reserved[2]{}; };   // effects: kEffect* bits (graphics_effects.h); record: rt_material.h
static_assert(sizeof(DrawPush)==128 && offsetof(DrawPush,jitter)==88 && offsetof(DrawPush,viewport)==96 && offsetof(DrawPush,effects)==112 && offsetof(DrawPush,record)==116);
constexpr VkFormat kMaterialFormat=VK_FORMAT_R32_UINT, kAlbedoFormat=VK_FORMAT_R8G8B8A8_UNORM;   // the RT pair (rt_material.h)
struct MotionBlock { gx::Projection prev_rows; uint32_t prev_palette, kind, unused[2]; };   // smoke.slang MotionBlock (std430)
static_assert(sizeof(MotionBlock)==80);
uint32_t VulkanRenderer::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw std::runtime_error("required Vulkan memory type unavailable");
}
VulkanRenderer::Buffer VulkanRenderer::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool prefer_device_local) {
    Buffer buffer; buffer.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.size = size; create.usage = usage;
    vk_check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        VkMemoryAllocateFlagsInfo address{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; address.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) allocate.pNext=&address;   // rt_input_usage(): the ray tracing builder reads it by address
        // A small BAR heap can fill up even when ordinary host memory is still available.
        vk_check(allocate_host_buffer(memory_, requirements.memoryTypeBits, prefer_device_local, [&](uint32_t type) {
            allocate.memoryTypeIndex=type;
            return vkAllocateMemory(device, &allocate, nullptr, &buffer.memory);
        }), "buffer memory");
        vk_check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "bind buffer memory");
        vk_check(vkMapMemory(device, buffer.memory, 0, size, 0, &buffer.mapped), "map buffer memory");
    } catch (...) { destroy_buffer(buffer); throw; }
    return buffer;
}
void VulkanRenderer::destroy_buffer(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}
static size_t validate_texture_chain(const gx::TexturePixels* pixels) {
    if(pixels) pixels->wait();   // the background decode of a freshly created texture
    const gx::TextureLevel white{1,1,{255,255,255,255}};
    const bool gpu_only=pixels && pixels->gpu_only;
    if(pixels && (unsigned(pixels->channels)>unsigned(gx::TextureChannels::rgb565) ||
        (pixels->channels!=gx::TextureChannels::rgba && !gpu_only)))throw std::invalid_argument("invalid owned texture channels");
    if(pixels && (pixels->levels.empty() || pixels->levels.size()>11)) throw std::invalid_argument("invalid owned mip chain count");
    const auto& level=pixels ? pixels->levels[0] : white;
    if (!level.width || !level.height || level.width>1024 || level.height>1024 || (gpu_only ? !level.rgba.empty() : level.rgba.size()!=size_t(level.width)*level.height*4))
        throw std::invalid_argument("invalid owned RGBA texture");
    const auto mip_levels=pixels ? uint32_t(pixels->levels.size()) : 1;
    size_t upload_size=0; uint32_t width=level.width,height=level.height;
    for(uint32_t i=0;i<mip_levels;++i) {
        const auto& mip=pixels ? pixels->levels[i] : white;
        if(mip.width!=width || mip.height!=height || (gpu_only ? !mip.rgba.empty() : mip.rgba.size()!=size_t(width)*height*4) || (i && pixels->levels[i-1].width==1 && pixels->levels[i-1].height==1)) throw std::invalid_argument("invalid owned mip chain dimensions");
        upload_size+=mip.rgba.size(); width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    return upload_size;
}
VulkanRenderer::Texture VulkanRenderer::make_texture(const gx::TexturePixels* pixels, VkExtent2D physical_extent) {
    const auto upload_size=validate_texture_chain(pixels);
    const gx::TextureLevel white{1,1,{255,255,255,255}};
    const bool gpu_only=pixels && pixels->gpu_only;
    const auto& level=pixels ? pixels->levels[0] : white;
    Texture texture; texture.extent=physical_extent.width ? physical_extent : VkExtent2D{level.width,level.height};
    texture.channels=pixels ? pixels->channels : gx::TextureChannels::rgba;
    // shortcut: RGB565 uses Vulkan UNORM rounding; verify exact GX copy rounding before pixel-parity acceptance (TD-045).
    const VkFormat format=texture.channels==gx::TextureChannels::rgb565 ? VK_FORMAT_R5G6B5_UNORM_PACK16 : VK_FORMAT_R8G8B8A8_UNORM;
    if(texture.channels==gx::TextureChannels::rgb565) {
        VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical_,format,&properties);
        const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&required)!=required)throw std::runtime_error("GPU lacks native RGB565 copy/filter support");
    }
    if(texture.extent.width>max_image_dimension_ || texture.extent.height>max_image_dimension_) throw std::invalid_argument("texture extent exceeds device image limits");
    texture.mip_levels=pixels ? uint32_t(pixels->levels.size()) : 1;
    texture.mip_levels=std::min(texture.mip_levels,uint32_t(std::bit_width(std::max(texture.extent.width,texture.extent.height))));
    try {
        if(!gpu_only) texture.upload=make_buffer(upload_size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        size_t at=0;
        if(!gpu_only) for(uint32_t i=0;i<texture.mip_levels;++i) { const auto& mip=pixels ? pixels->levels[i] : white; std::memcpy(static_cast<std::byte*>(texture.upload.mapped)+at,mip.rgba.data(),mip.rgba.size()); at+=mip.rgba.size(); }
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType=VK_IMAGE_TYPE_2D; create.format=format;
        create.extent={texture.extent.width,texture.extent.height,1}; create.mipLevels=texture.mip_levels; create.arrayLayers=1; create.samples=VK_SAMPLE_COUNT_1_BIT;
        create.tiling=VK_IMAGE_TILING_OPTIMAL; create.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|(gpu_only ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        vk_check(vkCreateImage(device,&create,nullptr,&texture.image),"texture image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,texture.image,&requirements);
        texture.allocation_size=requirements.size;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
        allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device,&allocate,nullptr,&texture.memory),"texture memory");
        vk_check(vkBindImageMemory(device,texture.image,texture.memory,0),"bind texture memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=texture.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,texture.mip_levels,0,1};
        if(texture.channels==gx::TextureChannels::alpha_intensity)
            view.components={VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A};
        vk_check(vkCreateImageView(device,&view,nullptr,&texture.view),"texture view");
    } catch (...) {
        if (texture.view) vkDestroyImageView(device,texture.view,nullptr);
        if (texture.image) vkDestroyImage(device,texture.image,nullptr);
        if (texture.memory) vkFreeMemory(device,texture.memory,nullptr);
        destroy_buffer(texture.upload); throw;
    }
    ++texture_creations_;
    return texture;
}
VulkanRenderer::Texture VulkanRenderer::make_copy_texture(const std::shared_ptr<const gx::TexturePixels>& source) {
    if(!source || !source->gpu_only) throw std::invalid_argument("invalid owned GPU copy texture");
    validate_texture_chain(source.get());
    const auto& level=source->levels[0];
    const VkExtent2D physical{std::max(1u,uint32_t((uint64_t(level.width)*efb_extent_.width+320)/640)),std::max(1u,uint32_t((uint64_t(level.height)*efb_extent_.height+240)/480))};
    const auto mip_levels=std::min(uint32_t(source->levels.size()),uint32_t(std::bit_width(std::max(physical.width,physical.height))));
    auto found=std::find_if(recycled_copies_.begin(),recycled_copies_.end(),[&](const auto& texture) {
        return texture.extent.width==physical.width && texture.extent.height==physical.height && texture.mip_levels==mip_levels && texture.channels==source->channels;
    });
    Texture result;
    if(found==recycled_copies_.end()) result=make_texture(source.get(),physical);
    else {
        result=std::move(*found); recycled_copy_bytes_-=result.allocation_size;
        if(found!=recycled_copies_.end()-1) *found=std::move(recycled_copies_.back());
        recycled_copies_.pop_back();
    }
    result.source=source; result.initialized=false;
    return result;
}
void VulkanRenderer::record_texture_copy(VkCommandBuffer command, VkImage source, VkExtent2D extent, const std::array<uint32_t,4>& region, Texture& target, VkImageLayout before) {
    const auto [x,y,width,height]=region;
    const bool initialized=before!=VK_IMAGE_LAYOUT_UNDEFINED;
    barrier(command,target.image,before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,initialized ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
        initialized ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels);
    VkImageBlit copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.srcOffsets[0]={int32_t(uint64_t(x)*extent.width/640),int32_t(uint64_t(y)*extent.height/480),0};
    copy.srcOffsets[1]={std::max(copy.srcOffsets[0].x+1,int32_t(uint64_t(x+width)*extent.width/640)),std::max(copy.srcOffsets[0].y+1,int32_t(uint64_t(y+height)*extent.height/480)),1};
    copy.dstOffsets[1]={int32_t(target.extent.width),int32_t(target.extent.height),1};
    vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_LINEAR);
    for(uint32_t i=1;i<target.mip_levels;++i) {
        barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,i-1);
        VkImageBlit mip{}; mip.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i-1,0,1}; mip.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1};
        mip.srcOffsets[1]={int32_t(std::max(1u,target.extent.width>>(i-1))),int32_t(std::max(1u,target.extent.height>>(i-1))),1};
        mip.dstOffsets[1]={int32_t(std::max(1u,target.extent.width>>i)),int32_t(std::max(1u,target.extent.height>>i)),1};
        vkCmdBlitImage(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mip,VK_FILTER_LINEAR);
    }
    barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,target.mip_levels-1);
    barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels);
}
void VulkanRenderer::recycle_copy_texture(Texture& texture) {
    const double factor=std::max(efb_extent_.width/640.0,efb_extent_.height/480.0);
    const VkDeviceSize budget=VkDeviceSize(std::min(double(VkDeviceSize(512)<<20),double(VkDeviceSize(64)<<20)*factor*factor));
    if(recycled_copies_.size()==32 || recycled_copy_bytes_>=budget || texture.allocation_size>budget-recycled_copy_bytes_) { destroy_texture(texture); return; }
    texture.source.reset(); texture.initialized=false;
    try { recycled_copies_.push_back(std::move(texture)); }
    catch(...) { destroy_texture(texture); throw; }
    recycled_copy_bytes_+=recycled_copies_.back().allocation_size;
    texture={};
}
void VulkanRenderer::destroy_texture(Texture& texture) {
    if (texture.view) vkDestroyImageView(device,texture.view,nullptr);
    if (texture.image) vkDestroyImage(device,texture.image,nullptr);
    if (texture.memory) vkFreeMemory(device,texture.memory,nullptr);
    destroy_buffer(texture.upload);
    texture={};
}
void VulkanRenderer::retire_textures(Slot& slot) {
    if (slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
    slot.texture_pool=VK_NULL_HANDLE; slot.descriptor_capacity=0; slot.texture_sets.clear();
    for (auto& texture : slot.textures) {
        destroy_texture(texture);
    }
    slot.textures.clear();
    slot.copy_sources.clear();
}
PipelineKey VulkanRenderer::pipeline_key(const gx::DrawPacket& draw,bool alpha_pass) const {
    auto blend=draw.blend;
    if(alpha_pass) blend={0,1,0,3,false,true};
    else if(draw.uniforms->pixel[2]&256) blend.alpha_write=false;
    if (blend.mode>3 || blend.source>7 || blend.destination>7 || blend.logic>15) throw std::invalid_argument("invalid native blend state");
    if (blend.mode==2 && !logic_ops_) throw std::runtime_error("native GX logic operation requires GPU logicOp support");
    // Fields the pipeline ignores in this mode must not split the key space (factors only matter when blending, the logic op only in logic mode).
    if (blend.mode!=1) { blend.source=1; blend.destination=0; }
    if (blend.mode!=2) blend.logic=3;
    PipelineKey key{};
    switch(draw.primitive) {
    case 0x80: case 0x90: case 0x98: case 0xA0: key[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 0xA8: case 0xB0: key[69]=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    default:
        std::fprintf(stderr,"[unsupported-primitive] type %02X callsite %08X vertices %zu indices %zu\n",draw.primitive,draw.callsite,draw.vertex_view().size(),draw.index_view().size());
        throw std::invalid_argument("unsupported native draw topology");
    }
    if(draw.state->cull>3) throw std::invalid_argument("invalid native cull mode");
    key[0]=blend.mode|(blend.source<<2)|(blend.destination<<5)|(blend.logic<<8)|(uint32_t(blend.color_write)<<12)|(uint32_t(blend.alpha_write)<<13);
    key[0]|=uint32_t(draw.state->cull)<<14;
    const auto& material=draw.material;
    if (material.stages>16) throw std::invalid_argument("native TEV stage count exceeds16");
    key[1]=material.stages;
    key[67]=alpha_pass;
    key[68]=draw.uniforms->pixel[1]&31;
    if(alpha_pass) {
        if((key[68]&17)==17) key[68]=5; // compare equal to the first pass's depth
        else key[68]&=~16u;
    }
    for(size_t i=0;i<4;++i) key[2]|=uint32_t(material.swaps[i])<<(i*8);
    for(size_t i=0;i<material.stages;++i) {
        const auto& stage=material.stage[i];
        key[3+i*4]=stage.color; key[4+i*4]=stage.alpha; key[5+i*4]=stage.order; key[6+i*4]=stage.konst;
    }
    return key;
}
VulkanRenderer::PipelineChoice VulkanRenderer::pipeline_for(const PipelineKey& key) {
    const auto fixed=uber_key(key);
    PipelineChoice choice;
    bool fresh=false;
    {
        std::lock_guard lock(pipelines_mutex_);
        if (!uber_only_) {
            if (auto found=pipelines_.find(key); found!=pipelines_.end()) return {found->second,false};
            fresh=known_keys_.insert(key).second;
        }
        if (auto found=uber_pipelines_.find(fixed); found!=uber_pipelines_.end()) {
            choice={found->second,true};
            if (!uber_only_) queue_pipeline(key,false);
        }
    }
    if (fresh && !append_pipeline_key(cache_dir_/"pipeline_keys.bin",key)) std::fprintf(stderr,"[pipeline] cannot record key in %s\n",cache_dir_.string().c_str());
    if (choice.pipeline) {
        if (fresh) std::fprintf(stderr,"[pipeline] new key: ubershader until its pipeline is compiled\n");
        return choice;
    }
    // Neither pipeline exists yet: compile synchronously (the ubershader in uber-only mode, else the specialized one) and queue the ubershader for later misses.
    const bool uber=uber_only_;
    const PipelineKey& wanted=uber ? fixed : key;
    const auto start=std::chrono::steady_clock::now();
    auto pipeline=make_pipeline(wanted,uber);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::lock_guard lock(pipelines_mutex_);
    std::pair<std::map<PipelineKey,VkPipeline>::iterator,bool> placed;
    try { placed=(uber ? uber_pipelines_ : pipelines_).emplace(wanted,pipeline); } catch (...) { vkDestroyPipeline(device,pipeline,nullptr); throw; }
    if (!placed.second) { vkDestroyPipeline(device,pipeline,nullptr); return {placed.first->second,uber}; }   // a compile thread finished it first
    if (!uber) queue_pipeline(fixed,true);
    if (fresh || ms>=2) std::fprintf(stderr,"[pipeline] %s %s compiled on the render thread: %.1f ms\n",fresh ? "new" : "not yet ready",uber ? "ubershader" : "key",ms);
    return {pipeline,uber};
}
uint32_t VulkanRenderer::effective_anisotropy() const { return clamp_anisotropy(effects_.anisotropy,anisotropy_supported_,anisotropy_limit_); }
// One sampler per GX sampler mode (wrap s/t, mipmap mode, mag/min filter); anisotropic filtering applies to the ones that filter linearly across mips.
void VulkanRenderer::create_samplers() {
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkSamplerAddressMode modes[]={VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    const auto anisotropy=effective_anisotropy();
    for(uint32_t mode=0;mode<256;++mode) {
        const auto s=mode&3,t=(mode>>2)&3,mip=(mode>>5)&3;
        if(s>2 || t>2 || mip>2) continue;
        sampler.addressModeU=modes[s]; sampler.addressModeV=modes[t];
        sampler.magFilter=(mode&16) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.minFilter=(mode&128) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.mipmapMode=mip==2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.maxLod=mip ? VK_LOD_CLAMP_NONE : 0.f;
        sampler.anisotropyEnable=anisotropy>1 && (mode&128); sampler.maxAnisotropy=sampler.anisotropyEnable ? float(anisotropy) : 1.f;   // linear minification, with or without a mip filter: most of the game's textures have one level and sampler mode 0x9x
        vk_check(vkCreateSampler(device,&sampler,nullptr,&texture_samplers_[mode]),"texture sampler");
        if(motion_outputs_) {   // scene draws sample as if at output resolution (DLSS/FSR guides: log2(internal/output) - 1)
            auto biased=sampler; biased.mipLodBias=(upscaler_kind_==UpscalerKind::smaa || upscaler_kind_==UpscalerKind::off) ? 0.f : float(-std::log2(upscale_ratio(upscale_mode_))-1.0);   // SMAA and the pass-through: no upscaling, no jitter, no bias
            vk_check(vkCreateSampler(device,&biased,nullptr,&scene_samplers_[mode]),"scene texture sampler");
        }
    }
}
void VulkanRenderer::destroy_samplers() {
    for(auto& sampler:texture_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
    for(auto& sampler:scene_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
}
void VulkanRenderer::set_effects(const GraphicsEffects& effects) {
    owner();
    const bool anisotropy=effects.anisotropy!=effects_.anisotropy, path_before=scene_path_wanted(), rt_before=rt_material_active();
    chosen_effects_=effects;
    effects_=effective_effects(chosen_effects_,ray_tracing_supported());
    apply_scene_path(anisotropy,path_before);
    if (layout_ && rt_material_active()!=rt_before) {   // with an upscaler the scene targets stay: the RT pair comes and goes on its own
        vk_check(present_.wait_idle(device),"RT material targets");
        sync_rt_material_targets();
    }
}
void VulkanRenderer::force_scene_targets(bool on) {
    owner();
    const bool path_before=scene_path_wanted();
    force_scene_targets_=on;
    apply_scene_path(false,path_before);
}
// The samplers and the scene targets follow a change of the anisotropy level or of whether the scene-target path is needed (both only once the device exists).
void VulkanRenderer::apply_scene_path(bool anisotropy_changed, bool path_before) {
    const bool path_changed=scene_path_wanted()!=path_before;
    if(!layout_ || (!anisotropy_changed && !path_changed)) return;
    vk_check(present_.wait_idle(device),"effects change");   // through the presentation backend: frame generation drains its presents first
    destroy_samplers(); create_samplers();   // the scene samplers' mip bias depends on the path too
    if(path_changed && upscaler_kind_==UpscalerKind::off) {   // with an upscaler the scene targets exist anyway
        scene_phase_=false; reset_next_=true; scene_restore_copy_.reset();
        resized_=true; rebuild();
    }
}
// The vertex layout of every mesh draw (gx::GpuMeshVertex); the colour pipelines and the shadow pipeline share it.
static void mesh_vertex_input(VkVertexInputBindingDescription& binding, std::array<VkVertexInputAttributeDescription,14>& attributes) {
    binding={0, uint32_t(sizeof(gx::GpuMeshVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    attributes={{
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,position))},
        {1,0,VK_FORMAT_R8G8B8A8_UNORM,uint32_t(offsetof(gx::GpuMeshVertex,color))},
        {2,0,VK_FORMAT_R32_UINT,uint32_t(offsetof(gx::GpuMeshVertex,row))},
        {3,0,VK_FORMAT_R32G32_UINT,uint32_t(offsetof(gx::GpuMeshVertex,texture_matrix))},
        {14,0,VK_FORMAT_R8G8B8A8_UNORM,uint32_t(offsetof(gx::GpuMeshVertex,color1))},
        {15,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,object_normal))}}};
    for(uint32_t i=0;i<8;++i) attributes[6+i]={5+i,0,VK_FORMAT_R32G32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,texcoords)+i*8)};
}
// The shadow map's pipeline: the shadow vertex stage (no lighting) into a depth-only target, no fragment stage, no culling, a small depth bias.
VkPipeline VulkanRenderer::make_shadow_pipeline() {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage=VK_SHADER_STAGE_VERTEX_BIT; stage.module=shadow_vertex_module_; stage.pName="vertexMain";
    VkVertexInputBindingDescription binding; std::array<VkVertexInputAttributeDescription,14> attributes;
    mesh_vertex_input(binding,attributes);
    const std::array<VkVertexInputAttributeDescription,2> used{attributes[0],attributes[2]};   // the shadow vertex stage reads the position (0) and the matrix row (2) only
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount=1; input.pVertexBindingDescriptions=&binding; input.vertexAttributeDescriptionCount=uint32_t(used.size()); input.pVertexAttributeDescriptions=used.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.frontFace=VK_FRONT_FACE_CLOCKWISE; raster.lineWidth=1;
    raster.depthBiasEnable=VK_TRUE; raster.depthBiasConstantFactor=2.f; raster.depthBiasSlopeFactor=2.f;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable=VK_TRUE; depth.depthWriteEnable=VK_TRUE; depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; create.pNext=&rendering; create.stageCount=1; create.pStages=&stage; create.pVertexInputState=&input;
    create.pInputAssemblyState=&assembly; create.pViewportState=&viewport; create.pRasterizationState=&raster; create.pMultisampleState=&samples; create.pDepthStencilState=&depth;
    create.pColorBlendState=&blending; create.pDynamicState=&dynamic; create.layout=layout_;
    VkPipeline pipeline=VK_NULL_HANDLE;
    vk_check(vkCreateGraphicsPipelines(device,pipeline_cache_,1,&create,nullptr,&pipeline),"shadow pipeline");
    return pipeline;
}
VkPipeline VulkanRenderer::make_pipeline(const PipelineKey& key,bool uber) {
    gx::BlendState mode;
    mode.mode=key[0]&3; mode.source=(key[0]>>2)&7; mode.destination=(key[0]>>5)&7; mode.logic=(key[0]>>8)&15;
    mode.color_write=key[0]>>12&1; mode.alpha_write=key[0]>>13&1;
    const VkShaderModule vertex=vertex_module_, fragment=uber ? uber_module_ : fragment_module_;
    VkPipeline pipeline=VK_NULL_HANDLE;
    {
        if (!layout_) {
        VkPushConstantRange projection{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof(DrawPush))};
        VkDescriptorSetLayoutBinding texture_bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLER,8,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo texture_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; texture_layout.bindingCount=6; texture_layout.pBindings=texture_bindings;
        vk_check(vkCreateDescriptorSetLayout(device,&texture_layout,nullptr,&texture_layout_),"texture descriptor layout");
        create_samplers();
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.pushConstantRangeCount=1; layout_info.pPushConstantRanges=&projection;
        layout_info.setLayoutCount=1; layout_info.pSetLayouts=&texture_layout_;
        vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_), "pipeline layout");
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "vertexMain";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "fragmentMain";
        std::array<VkSpecializationMapEntry,67> spec_entries{};
        for(uint32_t i=0;i<spec_entries.size();++i) spec_entries[i]={i,i*uint32_t(sizeof(uint32_t)),sizeof(uint32_t)};
        VkSpecializationInfo specialization{uint32_t(spec_entries.size()),spec_entries.data(),67*sizeof(uint32_t),key.data()+1};
        if (!uber) stages[1].pSpecializationInfo=&specialization;
        static_assert(sizeof(gx::GpuMeshVertex)==108 && sizeof(gx::Projection)==64);
        VkVertexInputBindingDescription binding; std::array<VkVertexInputAttributeDescription,14> attributes;
        mesh_vertex_input(binding,attributes);
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding; input.vertexAttributeDescriptionCount = uint32_t(attributes.size()); input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VkPrimitiveTopology(key[69]);
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        constexpr VkCullModeFlags culls[]={VK_CULL_MODE_NONE,VK_CULL_MODE_BACK_BIT,VK_CULL_MODE_FRONT_BIT,VK_CULL_MODE_FRONT_AND_BACK};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = culls[(key[0]>>14)&3]; raster.frontFace = VK_FRONT_FACE_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask=(mode.color_write ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT : 0)|(mode.alpha_write ? VK_COLOR_COMPONENT_A_BIT : 0);
        const VkBlendFactor sources[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        const VkBlendFactor destinations[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        blend.blendEnable=mode.mode==1 || mode.mode==3;
        blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : sources[mode.source];
        blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : destinations[mode.destination];
        blend.colorBlendOp=blend.alphaBlendOp=mode.mode==3 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
        VkPipelineColorBlendAttachmentState blends[5]{blend,{},{},{},{}};
        blends[1].colorWriteMask=motion_write_mask(key);   // motion vectors: replaced, never blended
        blends[2].colorWriteMask=gbuffer_write_mask(key);  // G-buffer: replaced, never blended
        const bool rt_writes=rt_material_write(key);
        blends[3].colorWriteMask=rt_writes ? VK_COLOR_COMPONENT_R_BIT : 0;   // material record index
        blends[4].colorWriteMask=rt_writes ? gbuffer_write_mask(key) : 0;    // albedo
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount = scene_color_attachments(); blending.pAttachments = blends;
        blending.logicOpEnable=mode.mode==2; blending.logicOp=VkLogicOp(mode.logic);
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
        const VkFormat formats[5] = {VK_FORMAT_R8G8B8A8_UNORM, kMotionFormat, kGBufferFormat, kMaterialFormat, kAlbedoFormat};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = scene_color_attachments(); rendering.pColorAttachmentFormats = formats;
        rendering.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable=bool(key[68]&1); depth.depthWriteEnable=bool(key[68]&16);
        depth.depthCompareOp=VkCompareOp((key[68]>>1)&7);
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; create.pNext = &rendering; create.stageCount = 2; create.pStages = stages; create.pVertexInputState = &input;
        create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &raster; create.pMultisampleState = &samples;
        create.pColorBlendState = &blending; create.pDynamicState = &dynamic; create.layout = layout_;
        create.pDepthStencilState=&depth;
        vk_check(vkCreateGraphicsPipelines(device, pipeline_cache_, 1, &create, nullptr, &pipeline), "graphics pipeline");
    }
    return pipeline;
}
// The driver cache makes recompiling a known pipeline cheap on later runs; the key files let a loading screen compile everything before the game starts.
void VulkanRenderer::load_pipeline_cache() {
    std::vector<char> data;
    if (std::ifstream file(cache_dir_/"pipeline_cache.bin",std::ios::binary|std::ios::ate); file) {
        data.resize(size_t(file.tellg())); file.seekg(0); file.read(data.data(),std::streamsize(data.size()));
        if (!file) data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; info.initialDataSize=data.size(); info.pInitialData=data.data();
    vk_check(vkCreatePipelineCache(device,&info,nullptr,&pipeline_cache_),"pipeline cache");   // the driver ignores data from another driver or version
    pipeline_cache_loaded_=data.size();
    for (const char* name:{"pipeline_keys.seed","pipeline_keys.bin"})
        for (const auto& key:load_pipeline_keys(cache_dir_/name))
            if (valid_pipeline_key(key,logic_ops_) && known_keys_.insert(key).second) warm_keys_.push_back(key);
}
void VulkanRenderer::save_pipeline_cache() {
    static std::mutex saving;   // compile threads and shutdown may save concurrently
    std::lock_guard guard(saving);
    size_t size=0;
    if (vkGetPipelineCacheData(device,pipeline_cache_,&size,nullptr)!=VK_SUCCESS || size==0 || size==pipeline_cache_loaded_) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device,pipeline_cache_,&size,data.data())!=VK_SUCCESS) return;
    const auto path=cache_dir_/"pipeline_cache.bin", temporary=cache_dir_/"pipeline_cache.tmp";
    if (std::ofstream file(temporary,std::ios::binary|std::ios::trunc); !file.write(data.data(),std::streamsize(size))) { std::fprintf(stderr,"[pipeline] cannot write %s\n",temporary.string().c_str()); return; }
    std::error_code error; std::filesystem::rename(temporary,path,error);   // atomic replace: a crash never leaves a torn cache
    if (error) std::fprintf(stderr,"[pipeline] cannot replace %s: %s\n",path.string().c_str(),error.message().c_str());
    else pipeline_cache_loaded_=size;
}
// Pipelines missing at draw time are compiled by these threads, off the render thread (the ubershader draws meanwhile).
void VulkanRenderer::queue_pipeline(const PipelineKey& key,bool uber) {
    if ((uber ? uber_queued_ : queued_).insert(key).second) { compile_queue_.push_back({key,uber}); compile_cv_.notify_one(); }
}
void VulkanRenderer::start_workers() {
    const unsigned count=std::clamp(std::thread::hardware_concurrency()/2,2u,8u);
    for (unsigned t=0;t<count;++t) compilers_.emplace_back([this] { compile_worker(); });
}
void VulkanRenderer::compile_worker() {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);   // in game these never compete with the guest and render threads; on the loading screen nothing else runs
    std::unique_lock lock(pipelines_mutex_);
    for (;;) {
        compile_cv_.wait(lock,[this] { return compile_stop_ || !compile_queue_.empty(); });
        if (compile_stop_) return;
        const CompileJob job=compile_queue_.front(); compile_queue_.pop_front();
        auto& target=job.uber ? uber_pipelines_ : pipelines_;
        if (target.count(job.key)) { ++jobs_done_; continue; }
        ++compiling_;
        lock.unlock();
        VkPipeline pipeline=VK_NULL_HANDLE;
        const auto start=std::chrono::steady_clock::now();
        try { pipeline=make_pipeline(job.key,job.uber); }
        catch (const std::exception& error) { std::fprintf(stderr,"[pipeline] background compile failed: %s\n",error.what()); }
        lock.lock();
        --compiling_; ++jobs_done_;
        if (pipeline) {
            ++compiled_; (job.uber ? compiled_uber_ms_ : compiled_ms_)+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); compiled_ubers_+=job.uber;
            if (!target.emplace(job.key,pipeline).second) vkDestroyPipeline(device,pipeline,nullptr);
        }
        if (compile_queue_.empty() && !compiling_ && compiled_) {
            const size_t done=std::exchange(compiled_,0), ubers=std::exchange(compiled_ubers_,0);
            const double ms=std::exchange(compiled_ms_,0.0), uber_ms=std::exchange(compiled_uber_ms_,0.0);
            lock.unlock();
            save_pipeline_cache();
            std::fprintf(stderr,"[pipeline] background compile idle: %zu pipelines ready (%zu ubershaders: %.0f ms, others: %.0f ms)\n",done,ubers,uber_ms,ms);
            lock.lock();
        }
    }
}
void VulkanRenderer::warm_pipelines(const std::function<void(size_t,size_t)>& progress) {
    owner();
    const auto begin=std::chrono::steady_clock::now();
    std::unique_lock lock(pipelines_mutex_);
    for (const auto& key:warm_keys_) queue_pipeline(uber_key(key),true);   // the few ubershader variants first: they cover any key still missing
    for (const auto& key:warm_keys_) queue_pipeline(key,false);
    const size_t base=jobs_done_, total=compile_queue_.size()+compiling_;
    while (!compile_queue_.empty() || compiling_) {
        const size_t done=jobs_done_-base;
        lock.unlock();
        progress(done,total);
        lock.lock();
        compile_cv_.wait_for(lock,std::chrono::milliseconds(50),[this] { return compile_queue_.empty() && !compiling_; });
    }
    lock.unlock();
    progress(total,total);
    std::fprintf(stderr,"[pipeline] warm-up: %zu pipelines in %.0f ms\n",total,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
}
void VulkanRenderer::stop_workers() {
    { std::lock_guard lock(pipelines_mutex_); compile_stop_=true; }
    compile_cv_.notify_all();
    for (auto& thread:compilers_) thread.join();
    compilers_.clear();
}
// Persistent vertex/index buffers of a cached mesh. An entry whose owner expired is stale (the guest freed the mesh, its address may be reused):
// no submission can still hold it, because every slot keeps a strong reference until its fence completed.
VulkanRenderer::GpuMesh& VulkanRenderer::gpu_mesh(const std::shared_ptr<const gx::MeshBuffers>& source) {
    auto found=meshes_.find(source.get());
    if (found!=meshes_.end()) {
        if (!found->second.owner.expired()) return found->second;
        release_mesh(found->second);
        meshes_.erase(found);
    }
    const auto& vertices=source->vertices; const auto& indices=source->indices;
    for (uint32_t index : indices) if (index >= vertices.size()) throw std::invalid_argument("draw index exceeds owned vertices");
    if (vertices.empty() || indices.empty() || vertices.size() > size_t(INT32_MAX) || indices.size() > size_t(UINT32_MAX)) throw std::invalid_argument("invalid cached mesh");
    const auto created_at=std::chrono::steady_clock::now();
    const VkDeviceSize vertex_bytes=VkDeviceSize(vertices.size())*sizeof(gx::GpuMeshVertex), index_bytes=VkDeviceSize(indices.size())*sizeof(uint32_t);
    const VkDeviceSize total=(vertex_bytes+15)/16*16+(index_bytes+15)/16*16;
    if (mesh_slabs_.empty() || mesh_slabs_.back()->buffer.size-mesh_slabs_.back()->used<total) {
        auto slab=std::make_unique<MeshSlab>();
        slab->buffer=make_buffer(std::max<VkDeviceSize>(VkDeviceSize(32)<<20,total),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|rt_input_usage(),true);
        mesh_slabs_.push_back(std::move(slab));
    }
    MeshSlab& slab=*mesh_slabs_.back();
    GpuMesh mesh;
    mesh.slab=&slab; mesh.vertex_offset=slab.used; mesh.index_offset=slab.used+(vertex_bytes+15)/16*16;
    auto* target=reinterpret_cast<gx::GpuMeshVertex*>(static_cast<std::byte*>(slab.buffer.mapped)+mesh.vertex_offset);
    for (size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
    std::memcpy(static_cast<std::byte*>(slab.buffer.mapped)+mesh.index_offset,indices.data(),index_bytes);
    slab.used+=total; ++slab.live;
    mesh.index_count=uint32_t(indices.size()); mesh.rows=gx::used_rows(vertices); mesh.owner=source; mesh.id=next_mesh_id_++;
    if (const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-created_at).count(); ms>2) std::fprintf(stderr,"[gpu-mesh] %zu vertices %zu indices uploaded in %.2f ms (%zu meshes resident)\n",vertices.size(),indices.size(),ms,meshes_.size()+1);
    return meshes_.emplace(source.get(),std::move(mesh)).first->second;
}
// A slab other than the one being filled is freed with its last mesh (every mesh in it expired, so no submission can still read it).
void VulkanRenderer::release_mesh(GpuMesh& mesh) {
    if (rt_.initialized()) rt_.invalidate_mesh(mesh.id);   // its cached BLAS goes with it (the id is never reused, but the memory should)
    MeshSlab* slab=mesh.slab; mesh.slab=nullptr;
    if (!slab || --slab->live) return;
    if (mesh_slabs_.back().get()==slab) { slab->used=0; return; }   // empty current slab: reuse it from the start
    destroy_buffer(slab->buffer);
    std::erase_if(mesh_slabs_,[slab](const auto& held){ return held.get()==slab; });
}
void VulkanRenderer::sweep_meshes(bool all) {
    for (auto it=meshes_.begin(); it!=meshes_.end();) {
        if (all || it->second.owner.expired()) { release_mesh(it->second); it=meshes_.erase(it); }
        else ++it;
    }
    if (all) { for (auto& slab : mesh_slabs_) destroy_buffer(slab->buffer); mesh_slabs_.clear(); }
}
void VulkanRenderer::collect() {
    for (auto& slot : slots_) {
        if (!slot.submitted || slot.reported) continue;
        VkResult status = vkGetFenceStatus(device, slot.fence);
        if (status == VK_NOT_READY) continue;
        vk_check(status, "frame fence status");
        completed_.push_back(slot.serial); slot.reported = true;
        uint64_t stamps[2];
        if (slot.timed && vkGetQueryPoolResults(device, timing_pool_, uint32_t((&slot - slots_.data()) * 2), 2, sizeof stamps, stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] >= stamps[0])
        { const uint64_t ns = uint64_t(double(stamps[1] - stamps[0]) * timestamp_period_); gpu_busy_ns_ += ns; if (ns > gpu_window_max_ns_) gpu_window_max_ns_ = ns; }
        if (slot.timed) pass_timer_.collect(uint32_t(&slot - slots_.data()));
        slot.timed = false;
    }
}
std::vector<uint64_t> VulkanRenderer::poll_completions() {
    owner(); collect();
    std::sort(completed_.begin(), completed_.end());
    auto completed = std::move(completed_); completed_.clear(); return completed;
}
RenderPixels VulkanRenderer::capture(uint64_t serial, bool efb) {
    owner();
    if(efb && serial!=last_serial_) throw std::invalid_argument("EFB capture requires latest submitted serial");
    for (auto& slot : slots_) {
        if (slot.submitted && slot.serial == serial && (efb || slot.copied)) {
            vk_check(vkGetFenceStatus(device, slot.fence), "capture requires completed frame");
            collect();
            const auto size=efb ? efb_extent_ : extent_;
            const VkDeviceSize bytes=VkDeviceSize(size.width)*size.height*4;
            if(slot.pixels.size!=bytes) { destroy_buffer(slot.pixels); slot.pixels=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT); }
            vk_check(vkResetCommandBuffer(slot.command, 0), "reset capture command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin capture");
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {size.width, size.height, 1};
            vkCmdCopyImageToBuffer(slot.command, efb ? efb_ : slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.pixels.buffer, 1, &copy);
            VkBufferMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}; host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT; host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.buffer = slot.pixels.buffer; host_barrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; host_dependency.bufferMemoryBarrierCount = 1; host_dependency.pBufferMemoryBarriers = &host_barrier; vkCmdPipelineBarrier2(slot.command, &host_dependency);
            vk_check(vkEndCommandBuffer(slot.command), "end capture");
            vk_check(vkResetFences(device, 1, &slot.fence), "reset capture fence");
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = slot.command;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command;
            vk_check(vkQueueSubmit2(queue_, 1, &submit, slot.fence), "submit capture");
            vk_check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 1000000000ull), "capture fence");
            const auto* data = static_cast<const uint8_t*>(slot.pixels.mapped);
            return {serial, size.width, size.height, {data, data + size_t(bytes)}};
        }
    }
    throw std::runtime_error("capture serial no longer retained: " + std::to_string(serial));
}
bool VulkanRenderer::wait_for_pending(uint64_t timeout_ns) {
    owner();
    const Slot* oldest=nullptr;
    for(const auto& slot:slots_) if(slot.submitted && !slot.reported && (!oldest || slot.serial<oldest->serial)) oldest=&slot;
    if(!oldest) return false;
    const auto result=vkWaitForFences(device,1,&oldest->fence,VK_TRUE,timeout_ns);
    if(result!=VK_TIMEOUT) vk_check(result,"wait pending native frame");
    return true;
}
void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    owner(); requested_ = {width, height}; resized_ = true;
}
void VulkanRenderer::set_render_scale(double scale) {
    owner();
    manual_render_extent(scale,render_aspect_);
    if(configured_scale_!=scale) { configured_scale_=scale; resized_=true; }
}
void VulkanRenderer::configure_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier) {
    if (instance_) throw std::logic_error("upscaling is configured before initialize()");
    upscaler_kind_=kind; upscale_mode_=mode; sharpening_=sharpening; sharpness_=sharpness;
    frame_generation_=frame_generation && (kind==UpscalerKind::dlss || kind==UpscalerKind::fsr); frame_generation_multiplier_=multiplier;
}
bool VulkanRenderer::set_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier) {
    owner();
    if (!motion_outputs_ || (kind==UpscalerKind::dlss && !dlss_available_)) kind=UpscalerKind::off;
    frame_generation=frame_generation && ((kind==UpscalerKind::dlss && dlss_g_available_) || (kind==UpscalerKind::fsr && bool(fsr_g_)));
    sharpening_=sharpening; sharpness_=sharpness;
    if (motion_outputs_ && sharpening_) rcas_.initialize(device,memory_,shaders_);
    if (kind==upscaler_kind_ && mode==upscale_mode_ && frame_generation==frame_generation_ && multiplier==frame_generation_multiplier_) return true;
    vk_check(present_.wait_idle(device), "upscale mode switch");
    if (dlss_g_) dlss_g_->set_active(false,true);
    if (fsr_g_ && fsr_g_->has_context()) fsr_g_->begin_frame(false,{},frame_index_);
    const bool reconstruction_changed=kind!=upscaler_kind_ || mode!=upscale_mode_;
    const bool fsr_presentation=kind==UpscalerKind::fsr && (frame_generation || fsr_presentation_);
    const bool dlss_presentation=kind==UpscalerKind::dlss && (frame_generation || dlss_presentation_);
    const bool backend_changed=fsr_presentation!=fsr_presentation_ || dlss_presentation!=dlss_presentation_;
    if (backend_changed) {
        if (fsr_g_) fsr_g_->release_context();
        retire_images(true);   // every old chain is destroyed through its owning backend before routing changes
        fsr_presentation_=fsr_presentation;
        dlss_presentation_=dlss_presentation;
        present_=fsr_presentation_ ? fsr_g_->present_functions() : dlss_presentation_ ? base_present_ : PresentFunctions{};
    }
    if (reconstruction_changed) upscaler_.reset();
    if (multiplier!=frame_generation_multiplier_) { dlss_g_.reset(); fg_waited_value_=0; }
    upscaler_kind_=kind; upscale_mode_=mode; frame_generation_=frame_generation; frame_generation_multiplier_=multiplier;
    scene_phase_=false; reset_next_=true; scene_restore_copy_.reset();
    if (reconstruction_changed && layout_) { destroy_samplers(); create_samplers(); }   // the scene samplers' mip bias follows the reconstruction (the device is idle here)
    if (reconstruction_changed || backend_changed) { resized_=true; rebuild(); }
    else configure_generation();
    return true;
}
std::vector<const gx::TexturePixels*> VulkanRenderer::copy_keys() const {
    std::vector<const gx::TexturePixels*> keys;
    for (const auto& [key,texture] : copy_textures_) keys.push_back(key);
    keys.insert(keys.end(), lost_copies_.begin(), lost_copies_.end());
    return keys;
}
void VulkanRenderer::lost_copies(std::vector<const gx::TexturePixels*> keys) {
    lost_copies_.insert(keys.begin(), keys.end());
    if (!blank_) { auto blank=std::make_shared<gx::TexturePixels>(); blank->levels.push_back({1,1,{0,0,0,0}}); blank_=std::move(blank); }
}
void VulkanRenderer::set_menu(std::function<void()> build, std::function<void()> setup) {
    owner();
    menu_build_=std::move(build); menu_setup_=std::move(setup);
}
void VulkanRenderer::set_bloom(bool enabled) {
    owner();
    bloom_=enabled;
}
void VulkanRenderer::set_prompt_style(PromptStyle style) {
    owner();
    if(style!=PromptStyle::monochrome && style!=PromptStyle::sculpted)throw std::invalid_argument("invalid native prompt style");
    prompt_style_=style;
}
// Resize path: waits for the slots' GPU work only (no device idle, no presentation fences) and keeps buffers, textures and the EFB.
// The old swapchain becomes the new one's oldSwapchain and is freed by reap_retired() when its presentations are done.
void VulkanRenderer::retire_for_resize() {
    if (!device || !swapchain_) return;
    for (auto& slot : slots_)
        if (slot.submitted) vk_check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 1000000000ull), "resize frame fence");
    collect();
    old_swapchain_ = swapchain_;
    retired_.push_back({swapchain_, std::move(images_)});
    images_.clear(); swapchain_ = VK_NULL_HANDLE;
    for (auto& slot : slots_) {   // the output-sized images are the only per-slot resources that depend on the window
        if (slot.view) vkDestroyImageView(device, slot.view, nullptr);
        if (slot.image) vkDestroyImage(device, slot.image, nullptr);
        if (slot.memory) vkFreeMemory(device, slot.memory, nullptr);
        slot.view = VK_NULL_HANDLE; slot.image = VK_NULL_HANDLE; slot.memory = VK_NULL_HANDLE;
        slot.initialized = slot.copied = false;
    }
}
void VulkanRenderer::reap_retired(bool wait) {
    for (auto it = retired_.begin(); it != retired_.end();) {
        bool done = wait || it->chain != old_swapchain_;   // a chain still waiting for its successor stays
        for (const auto& image : it->images) {
            if (!done || !image.pending) continue;
            const VkResult status = wait ? vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull) : vkGetFenceStatus(device, image.fence);
            if (status == VK_NOT_READY || status == VK_TIMEOUT) done = false;
            else if (status != VK_ERROR_DEVICE_LOST) vk_check(status, "retired presentation fence");
        }
        if (!done) { ++it; continue; }
        for (auto& image : it->images) {
            if (image.ready) vkDestroySemaphore(device, image.ready, nullptr);
            if (image.fence) vkDestroyFence(device, image.fence, nullptr);
        }
        present_.destroy(device, it->chain, nullptr);
        it = retired_.erase(it);
    }
}
void VulkanRenderer::retire_images(bool keep_buffers) {
    if (!device) return;
    VkResult status = present_.wait_idle(device);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) vk_check(status, "retire device work");
    old_swapchain_ = VK_NULL_HANDLE;
    if (status == VK_SUCCESS) {
        collect();
        reap_retired(true);
        for (const auto& image : images_)
            if (image.pending) vk_check(vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull), "presentation retirement fence");
    }
    for (auto& image : images_) {
        if (image.ready) vkDestroySemaphore(device, image.ready, nullptr);
        if (image.fence) vkDestroyFence(device, image.fence, nullptr);
    }
    images_.clear();
    if (swapchain_) present_.destroy(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    for (auto& slot : slots_) {
        if (!keep_buffers) {
            retire_textures(slot);
            destroy_buffer(slot.vertices); destroy_buffer(slot.indices); destroy_buffer(slot.pixels); destroy_buffer(slot.uniforms); destroy_buffer(slot.palettes); destroy_buffer(slot.banks); destroy_buffer(slot.motions); slot.mesh_refs.clear();
        }
        if (slot.view) vkDestroyImageView(device, slot.view, nullptr);
        if (slot.image) vkDestroyImage(device, slot.image, nullptr);
        if (slot.memory) vkFreeMemory(device, slot.memory, nullptr);
        slot.view = VK_NULL_HANDLE; slot.image = VK_NULL_HANDLE; slot.memory = VK_NULL_HANDLE;
        slot.submitted = slot.reported = slot.initialized = slot.copied = false; slot.serial = 0;
    }
}
UpscaleImage VulkanRenderer::hudless_image() const {   // this frame's HUD-less copy (alternating: the interpolation of the previous frame may still read the other one)
    const auto slot=frame_index_&1;
    return {hudless_[slot],hudless_view_[slot],VK_FORMAT_R8G8B8A8_UNORM,hudless_extent_,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT};
}
void VulkanRenderer::record_upscale(VkCommandBuffer command, bool black_output) {
    tracker_.end_frame();   // every scene draw of the frame is recorded: camera cut and reprojection of this frame
    rt_key_direction_=tracker_.camera_cut() ? kRtOverheadDirection : rt_key_direction_after(rt_key_direction_,tracker_.view_to_prev_view());   // the key light stands still in the world
    if (effects_need_lights()) dominant_light_=gx::select_dominant(scene_lights_.lights(),dominant_light_);
    const auto now=std::chrono::steady_clock::now();
    UpscaleInputs in;
    in.command=command; in.targets_generation=targets_generation_;
    // Usage as make_attachment creates them with motion outputs on.
    const VkImageUsageFlags color_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags depth_usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    in.color={scene_color_,scene_color_view_,VK_FORMAT_R8G8B8A8_UNORM,scene_extent_,color_usage};
    in.depth={scene_depth_,scene_depth_view_,VK_FORMAT_D32_SFLOAT,scene_extent_,depth_usage};
    in.motion={scene_motion_,scene_motion_view_,kMotionFormat,scene_extent_,color_usage};
    in.output={efb_,efb_view_,VK_FORMAT_R8G8B8A8_UNORM,efb_extent_,color_usage};
    // The scene viewport (640x360 of the 640x480 logical EFB, top-left) in pixels of each target.
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const auto region=[&](VkExtent2D extent) {
        return VkExtent2D{std::clamp<uint32_t>(uint32_t(std::lround((viewport[0]+viewport[2])*extent.width/640)),1,extent.width),
                          std::clamp<uint32_t>(uint32_t(std::lround((viewport[1]+viewport[3])*extent.height/480)),1,extent.height)};
    };
    in.render_size=region(scene_extent_); in.output_size=region(efb_extent_);
    in.jitter=jitter_;
    in.reset=reset_next_ || tracker_.camera_cut() || black_output;
    in.sharpening=sharpening_; in.sharpness=float(sharpness_);
    in.frame_ms=last_upscale_==std::chrono::steady_clock::time_point{} ? 16.7f : std::clamp(std::chrono::duration<float,std::milli>(now-last_upscale_).count(),0.1f,100.f);
    in.frame=upscale_frame_;
    in.projection=scene_projection_; in.clip_to_prev_clip=tracker_.clip_to_prev_clip();
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_color_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
    barrier(command,scene_depth_,scene_depth_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(command,scene_motion_,scene_motion_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
    scene_depth_layout_=scene_motion_layout_=VK_IMAGE_LAYOUT_GENERAL;   // and they stay there until the next frame's first scene pass
    if (gbuffer_cleared_) { barrier(command,scene_gbuffer_,scene_gbuffer_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any); scene_gbuffer_layout_=VK_IMAGE_LAYOUT_GENERAL; }   // written this frame: readable by the effects
    if (rt_material_cleared_) {   // likewise the RT pair
        for (VkImage image:{scene_material_,scene_albedo_}) barrier(command,image,scene_rt_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
        scene_rt_layout_=VK_IMAGE_LAYOUT_GENERAL;
    }
    barrier(command,efb_,efb_initialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
    const auto timer_slot=uint32_t(next_slot_);
    if (shadow_cleared_) barrier(command,shadow_map_.image(),VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);   // drawn this frame: readable by the composite
    const std::array<float,6> scene_viewport{viewport[0]*scene_extent_.width/640,viewport[1]*scene_extent_.height/480,viewport[2]*scene_extent_.width/640,viewport[3]*scene_extent_.height/480,viewport[4],viewport[5]};
    bool rt_shadow_ran=false;
    if (rt_frame_ && (rt_as_open_ || rt_.stats().instances)) {   // the TLAS over this frame's BLASes (once: nothing after the boundary builds), then the ray-traced shadows
        if (!rt_as_open_) pass_timer_.begin(command,timer_slot,GpuPass::rt_as);
        rt_as_open_=false; rt_frame_=false;
        const bool tlas_built=rt_.build_tlas(command);
        pass_timer_.end(command,timer_slot,GpuPass::rt_as);
        if (tlas_built && rt_shadows_active() && gbuffer_cleared_ && rt_material_cleared_ && frame_projection_ && rt_record_count_ && scene_material_) {   // ray-traced shadows: trace, denoise, composite into the scene colour
            const auto idle=[this] { vk_check(present_.wait_idle(device),"rt shadows idle"); };
            rt_shadow_.initialize(device,memory_,shaders_,uint32_t(kFrameSlots),idle);
            rt_shadow_denoiser_.initialize(device,memory_,shaders_,idle);
            pass_timer_.begin(command,timer_slot,GpuPass::rt_shadows);
            RtShadowInputs shadow;
            shadow.command=command; shadow.tlas=rt_.tlas(); shadow.depth=scene_depth_view_; shadow.gbuffer=scene_gbuffer_view_; shadow.material=scene_material_view_; shadow.color=scene_color_view_;
            shadow.records=rt_records_[rt_record_slot_].buffer; shadow.lights=rt_lights_[rt_record_slot_].buffer;
            shadow.region=in.render_size; shadow.viewport=scene_viewport; shadow.view=view_params(*frame_projection_);
            shadow.sun=effects_.rt_shadows; shadow.local=effects_.rt_local_shadows; shadow.slot=timer_slot; shadow.level_light=effects_.rt_local_shadows!=EffectLevel::off && !rt_trace_lamps_; shadow.trace_lamps=rt_trace_lamps_; shadow.key_direction=rt_key_direction_;   // the test mode traces the lamps instead of the key light
            shadow.frame=uint32_t(upscale_frame_%8)+1;   // the noise pattern cycles on every path: the own denoiser's history (and a temporal upscaler's) averages it
            const VkImageView noisy=rt_shadow_.trace(shadow);
            rt_shadow_denoiser_.resize(rt_shadow_.signal_extent());
            if (in.reset || !rt_shadow_ran_last_) rt_shadow_denoiser_.reset();   // a cut, a black frame or a frame without the effect: the history is stale
            DenoiseInputs denoise;
            denoise.depth=scene_depth_view_; denoise.motion=scene_motion_view_; denoise.gbuffer=scene_gbuffer_view_; denoise.region=in.render_size;
            denoise.view=view_params(*frame_projection_); denoise.depth_range={scene_viewport[4],scene_viewport[5]}; denoise.targets_generation=targets_generation_;
            const bool high=int(effects_.rt_shadows)>=int(EffectLevel::high) || int(effects_.rt_local_shadows)>=int(EffectLevel::high);
            rt_shadow_.composite(shadow,rt_shadow_denoiser_.denoise(command,RtSignal::visibility,noisy,denoise,high ? 3 : 2));
            pass_timer_.end(command,timer_slot,GpuPass::rt_shadows);
            rt_shadow_traced_=rt_shadow_ran=true;
        }
    }
    rt_shadow_ran_last_=rt_shadow_ran;
    if ((effects_.ambient_occlusion!=EffectLevel::off || effects_.contact_shadows || (effects_.shadows!=EffectLevel::off && shadow_cleared_)) && gbuffer_cleared_ && frame_projection_) {   // the occlusion darkens the scene colour before it is reconstructed
        occlusion_.initialize(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"occlusion idle"); });
        pass_timer_.begin(command,timer_slot,GpuPass::occlusion);
        OcclusionInputs occlusion;
        occlusion.command=command; occlusion.targets_generation=targets_generation_; occlusion.color=in.color; occlusion.depth=in.depth;
        occlusion.gbuffer={scene_gbuffer_,scene_gbuffer_view_,kGBufferFormat,scene_extent_,color_usage};
        occlusion.region=in.render_size;
        occlusion.viewport=scene_viewport;
        occlusion.view=view_params(*frame_projection_);
        occlusion.ao=effects_.ambient_occlusion; occlusion.contact=effects_.contact_shadows; occlusion.light=dominant_light_; occlusion.slot=timer_slot;
        if (shadow_cleared_ && shadow_fit_ && dominant_light_) {
            const auto& p=dominant_light_->position; const float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            occlusion.shadow=ShadowInputs{shadow_map_.view(),shadow_map_.generation(),*shadow_fit_,shadow_preset(effects_.shadows),{p[0]/length,p[1]/length,p[2]/length}};
        }
        occlusion.noise_frame=(upscaler_kind_==UpscalerKind::dlss || upscaler_kind_==UpscalerKind::fsr) ? uint32_t(upscale_frame_%8)+1 : 0;   // the upscaler's history averages the pattern; otherwise it stays fixed
        occlusion_.record(occlusion);
        pass_timer_.end(command,timer_slot,GpuPass::occlusion);
    }
    const auto timed_pass=upscaler_kind_==UpscalerKind::smaa ? GpuPass::smaa : GpuPass::upscale;
    pass_timer_.begin(command,timer_slot,timed_pass);
    upscaler_->evaluate(in);
    if (fsr_generation_enabled()) fsr_g_->prepare(in,frame_index_);
    if (sharpening_ && upscaler_kind_!=UpscalerKind::off && !upscaler_->sharpens()) rcas_.record(command,efb_,efb_view_,in.output_size,rcas_constant(sharpness_));   // the pass-through (off) never sharpens
    pass_timer_.end(command,timer_slot,timed_pass);
    if (effects_.motion_blur!=EffectLevel::off && !in.reset) {   // a cut, a black frame or the first frame after a rebuild has no valid vectors
        motion_blur_.initialize(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"motion blur idle"); });
        pass_timer_.begin(command,timer_slot,GpuPass::motion_blur);
        MotionBlurInputs blur;
        blur.command=command; blur.targets_generation=targets_generation_; blur.color=in.output; blur.motion=in.motion;
        blur.output_size=in.output_size; blur.render_size=in.render_size; blur.level=effects_.motion_blur;
        motion_blur_.record(blur);
        pass_timer_.end(command,timer_slot,GpuPass::motion_blur);
    }
    barrier(command,efb_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    barrier(command,scene_color_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    if (scene_restore_copy_) {   // restore reconstructed pixels after bloom, rather than the raw jittered scene saved before this boundary
        slots_[next_slot_].copy_sources.push_back(scene_restore_copy_->target);   // the refresh submission owns this GPU use too
        auto& texture=copy_textures_.at(scene_restore_copy_->target.get());
        record_texture_copy(command,efb_,efb_extent_,scene_restore_copy_->source,texture,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene_restore_copy_.reset();
    }
    // Draws after the boundary (particles in front of the HUD, 3D markers) test against the scene depth.
    barrier(command,depth_,efb_initialized_ ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
    blit.srcOffsets[1]={int32_t(scene_extent_.width),int32_t(scene_extent_.height),1}; blit.dstOffsets[1]={int32_t(efb_extent_.width),int32_t(efb_extent_.height),1};
    vkCmdBlitImage(command,scene_depth_,VK_IMAGE_LAYOUT_GENERAL,depth_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    barrier(command,depth_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    efb_initialized_=true;
    reset_next_=false; scene_phase_=false; ++upscale_frame_; last_upscale_=now;
}
void VulkanRenderer::run_commands(const std::function<void(VkCommandBuffer)>& record) {
    owner();
    vk_check(present_.wait_idle(device),"run commands idle");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"run commands buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin run commands");
    try { record(command); } catch (...) { vkEndCommandBuffer(command); vkFreeCommandBuffers(device,pool_,1,&command); throw; }
    vk_check(vkEndCommandBuffer(command),"end run commands");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit run commands");
    vk_check(vkQueueWaitIdle(queue_),"run commands wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
}
VulkanRenderer::MotionPixels VulkanRenderer::capture_motion() {
    owner();
    if (!scene_motion_ || !motion_initialized_) throw std::logic_error("no motion target to capture");
    vk_check(present_.wait_idle(device),"motion capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*8;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"motion capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin motion capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_motion_,scene_motion_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_motion_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
    barrier(command,scene_motion_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_motion_layout_,all,any,all,any);
    vk_check(vkEndCommandBuffer(command),"end motion capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit motion capture");
    vk_check(vkQueueWaitIdle(queue_),"motion capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    MotionPixels pixels{scene_extent_.width,scene_extent_.height,{}};
    pixels.rg.reserve(size_t(scene_extent_.width)*scene_extent_.height*2);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    const auto half=[](uint16_t h) {   // IEEE half to float
        const uint32_t sign=uint32_t(h>>15)<<31, exponent=(h>>10)&31, mantissa=h&1023;
        if (!exponent) return std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24);
        if (exponent==31) return std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13));
        return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    };
    for (size_t i=0;i<size_t(scene_extent_.width)*scene_extent_.height;++i) { pixels.rg.push_back(half(halves[i*4])); pixels.rg.push_back(half(halves[i*4+1])); }
    destroy_buffer(buffer);
    return pixels;
}
static float half_to_float(uint16_t h) {   // IEEE half to float
    const uint32_t sign=uint32_t(h>>15)<<31, exponent=(h>>10)&31, mantissa=h&1023;
    return !exponent ? std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24) : exponent==31 ? std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13)) : std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
}
VulkanRenderer::RtShadowPixels VulkanRenderer::capture_rt_shadow_signal() {
    owner();
    const VkExtent2D extent=rt_shadow_.signal_extent();
    if (!rt_shadow_traced_ || !rt_shadow_.signal_image()) throw std::logic_error("no ray-traced shadow signal to capture");
    vk_check(present_.wait_idle(device),"rt shadow capture idle");
    const VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*2;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    run_commands([&](VkCommandBuffer command) {
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(command,rt_shadow_.signal_image(),VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);
    });
    RtShadowPixels pixels{extent.width,extent.height,{}};
    pixels.factor.reserve(size_t(extent.width)*extent.height);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    for (size_t i=0;i<size_t(extent.width)*extent.height;++i) pixels.factor.push_back(half_to_float(halves[i]));
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::RtShadowPixels VulkanRenderer::capture_rt_shadow_filtered() {
    owner();
    const VkExtent2D extent=rt_shadow_.signal_extent();
    if (!rt_shadow_traced_ || !rt_shadow_denoiser_.output_image()) throw std::logic_error("no denoised ray-traced shadow signal to capture");
    vk_check(present_.wait_idle(device),"rt shadow capture idle");
    const VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*8;   // RGBA16F: the factor is the red channel
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    run_commands([&](VkCommandBuffer command) {
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(command,rt_shadow_denoiser_.output_image(),VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);
    });
    RtShadowPixels pixels{extent.width,extent.height,{}};
    pixels.factor.reserve(size_t(extent.width)*extent.height);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    for (size_t i=0;i<size_t(extent.width)*extent.height;++i) pixels.factor.push_back(half_to_float(halves[i*4]));
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::GBufferPixels VulkanRenderer::capture_gbuffer() {
    owner();
    if (!scene_gbuffer_ || !gbuffer_initialized_) throw std::logic_error("no G-buffer to capture");
    vk_check(present_.wait_idle(device),"G-buffer capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*8;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"G-buffer capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin G-buffer capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_gbuffer_,scene_gbuffer_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_gbuffer_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
    barrier(command,scene_gbuffer_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_gbuffer_layout_,all,any,all,any);
    vk_check(vkEndCommandBuffer(command),"end G-buffer capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit G-buffer capture");
    vk_check(vkQueueWaitIdle(queue_),"G-buffer capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* data=static_cast<const uint16_t*>(buffer.mapped);
    GBufferPixels pixels{scene_extent_.width,scene_extent_.height,{data,data+size_t(bytes/2)}};
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::RtMaterialPixels VulkanRenderer::capture_rt_material() {
    owner();
    if (!scene_material_ || !rt_material_initialized_) throw std::logic_error("no RT material targets to capture");
    vk_check(present_.wait_idle(device),"RT material capture idle");
    const VkDeviceSize texels=VkDeviceSize(scene_extent_.width)*scene_extent_.height;
    Buffer buffer=make_buffer(texels*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);   // records, then albedo (4 bytes each)
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"RT material capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin RT material capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDeviceSize offset=0;
    for (VkImage image:{scene_material_,scene_albedo_}) {
        barrier(command,image,scene_rt_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
        VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
        vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
        barrier(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_rt_layout_,all,any,all,any);
        offset+=texels*4;
    }
    vk_check(vkEndCommandBuffer(command),"end RT material capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit RT material capture");
    vk_check(vkQueueWaitIdle(queue_),"RT material capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* records=static_cast<const uint32_t*>(buffer.mapped);
    const auto* albedo=static_cast<const uint8_t*>(buffer.mapped)+texels*4;
    RtMaterialPixels pixels{scene_extent_.width,scene_extent_.height,{records,records+texels},{albedo,albedo+texels*4}};
    destroy_buffer(buffer);
    return pixels;
}
std::vector<RtMaterialRecord> VulkanRenderer::rt_material_records() const {
    owner();
    const auto& buffer=rt_records_[rt_record_slot_];
    if (!buffer.mapped) return {};
    const auto* records=static_cast<const RtMaterialRecord*>(buffer.mapped);
    return {records,records+rt_record_count_};
}
RenderPixels VulkanRenderer::capture_scene() {
    owner();
    if (!scene_color_ || !scene_initialized_) throw std::logic_error("no scene target to capture");
    vk_check(present_.wait_idle(device),"scene capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*4;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"scene capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin scene capture");
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_color_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);   // between passes the color is TRANSFER_SRC
    vk_check(vkEndCommandBuffer(command),"end scene capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit scene capture");
    vk_check(vkQueueWaitIdle(queue_),"scene capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* data=static_cast<const uint8_t*>(buffer.mapped);
    RenderPixels pixels{last_serial_,scene_extent_.width,scene_extent_.height,{data,data+size_t(bytes)}};
    destroy_buffer(buffer);
    return pixels;
}
void VulkanRenderer::destroy_scene_targets() {
    ++targets_generation_;   // views of the images made next may reuse these handles: effect passes must rebind
    scene_restore_copy_.reset();
    for (auto [view,image,memory]:{std::tuple{&scene_color_view_,&scene_color_,&scene_color_memory_},{&scene_depth_view_,&scene_depth_,&scene_depth_memory_},{&scene_motion_view_,&scene_motion_,&scene_motion_memory_},{&scene_gbuffer_view_,&scene_gbuffer_,&scene_gbuffer_memory_}}) {
        if (*view) vkDestroyImageView(device,*view,nullptr);
        if (*image) vkDestroyImage(device,*image,nullptr);
        if (*memory) vkFreeMemory(device,*memory,nullptr);
        *view=VK_NULL_HANDLE; *image=VK_NULL_HANDLE; *memory=VK_NULL_HANDLE;
    }
    scene_extent_={}; scene_initialized_=motion_initialized_=gbuffer_initialized_=gbuffer_cleared_=false;
    scene_depth_layout_=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; scene_motion_layout_=scene_gbuffer_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    destroy_rt_material_targets();
}
void VulkanRenderer::destroy_rt_material_targets() {
    for (auto [view,image,memory]:{std::tuple{&scene_material_view_,&scene_material_,&scene_material_memory_},{&scene_albedo_view_,&scene_albedo_,&scene_albedo_memory_}}) {
        if (*view) vkDestroyImageView(device,*view,nullptr);
        if (*image) vkDestroyImage(device,*image,nullptr);
        if (*memory) vkFreeMemory(device,*memory,nullptr);
        *view=VK_NULL_HANDLE; *image=VK_NULL_HANDLE; *memory=VK_NULL_HANDLE;
    }
    rt_material_initialized_=rt_material_cleared_=false; scene_rt_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
// The RT pair exists exactly while an RT effect is active and the scene targets exist (same extent).
void VulkanRenderer::sync_rt_material_targets() {
    const bool wanted=rt_material_active() && scene_color_;
    if (!wanted) { destroy_rt_material_targets(); return; }
    if (scene_material_) return;
    make_attachment(scene_material_,scene_material_memory_,scene_material_view_,scene_extent_,false,kMaterialFormat);
    make_attachment(scene_albedo_,scene_albedo_memory_,scene_albedo_view_,scene_extent_,false,kAlbedoFormat);
}
void VulkanRenderer::make_attachment(VkImage& target,VkDeviceMemory& memory,VkImageView& target_view,VkExtent2D extent,bool depth,VkFormat format) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = format!=VK_FORMAT_UNDEFINED ? format : depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {extent.width, extent.height, 1}; image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (motion_outputs_) image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | (depth ? 0 : VK_IMAGE_USAGE_STORAGE_BIT);   // upscaler inputs and outputs
    vk_check(vkCreateImage(device, &image, nullptr, &target), "native attachment image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, target, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device, &allocate, nullptr, &memory), "attachment memory"); vk_check(vkBindImageMemory(device, target, memory, 0), "bind attachment");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = target; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format; view.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    vk_check(vkCreateImageView(device, &view, nullptr, &target_view), "attachment view");
}
void VulkanRenderer::rebuild() {
    if (fsr_presentation_) fsr_g_->release_context();
    if (swapchain_ && !fsr_presentation_ && !streamline_swapchain()) retire_for_resize(); else retire_images(true);
    VkSurfaceCapabilitiesKHR capabilities; vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities), "surface capabilities");
    VkImageUsageFlags swapchain_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags & swapchain_usage) != swapchain_usage) throw std::runtime_error("swapchain lacks transfer/color attachment support");
    extent_ = capabilities.currentExtent;
    if (extent_.width == UINT32_MAX) extent_ = {std::clamp(requested_.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width), std::clamp(requested_.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
    if (!extent_.width || !extent_.height) { resized_ = true; return; }
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_, &properties);
    if (extent_.width > properties.limits.maxImageDimension2D || extent_.height > properties.limits.maxImageDimension2D) throw std::runtime_error("render extent exceeds device image limits");
    uint32_t count = 0; vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "surface format count");
    std::vector<VkSurfaceFormatKHR> formats(count); vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "surface formats");
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& format : formats) {
        if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (format.format == VK_FORMAT_UNDEFINED) { chosen = {VK_FORMAT_B8G8R8A8_UNORM, format.colorSpace}; break; }
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = format; break; }
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) chosen = format;
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) throw std::runtime_error("8-bit SDR swapchain format unavailable");
    VkFormatProperties source_properties, target_properties;
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R8G8B8A8_UNORM, &source_properties); vkGetPhysicalDeviceFormatProperties(physical_, chosen.format, &target_properties);
    VkFormatFeatureFlags source_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatFeatureFlags target_features = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if ((source_properties.optimalTilingFeatures & source_features) != source_features || (target_properties.optimalTilingFeatures & target_features) != target_features)
        throw std::runtime_error("RGBA8 attachment/readback or swapchain blit format unsupported");
    uint32_t images = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount) images = std::min(images, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; create.surface = surface_; create.minImageCount = images;
    create.imageFormat = chosen.format; create.imageColorSpace = chosen.colorSpace; create.imageExtent = extent_; create.imageArrayLayers = 1;
    create.imageUsage = swapchain_usage; create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = VkCompositeAlphaFlagBitsKHR(capabilities.supportedCompositeAlpha & (0u - capabilities.supportedCompositeAlpha));
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
    {   // mailbox by default: with fifo the compositor held a swapchain image for up to two extra refreshes now and then, and the guest stalled in GXWaitDrawDone for 9-12 ms (the micro-freezes). UC_PRESENT_MODE=fifo|mailbox|immediate overrides; fifo is also the fallback
        const char* env = std::getenv("UC_PRESENT_MODE");
        const std::string_view wanted = env ? env : "mailbox";
        const VkPresentModeKHR mode = wanted == "immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR : wanted == "mailbox" ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
        uint32_t mode_count = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count); vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
        if (std::find(modes.begin(), modes.end(), mode) != modes.end()) create.presentMode = mode;
    }
    create.oldSwapchain = old_swapchain_;
    swapchain_format_ = chosen.format;
    vk_check(present_.create(device, &create, nullptr, &swapchain_), "swapchain");
    old_swapchain_ = VK_NULL_HANDLE;   // retired by the call above; reap_retired() frees it when its presentations are done
    vk_check(present_.images(device, swapchain_, &count, nullptr), "swapchain image count");
    std::vector<VkImage> swapchain_images(count); vk_check(present_.images(device, swapchain_, &count, swapchain_images.data()), "swapchain images");
    images_.resize(count);
    for (size_t i = 0; i < images_.size(); ++i) {
        images_[i].image = swapchain_images[i];
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &images_[i].ready), "per-image present semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk_check(vkCreateFence(device, &fence, nullptr, &images_[i].fence), "per-image present fence");
    }
    const auto scale=device_render_scale(configured_scale_,max_image_dimension_,render_aspect_);
    const auto visible=manual_render_extent(scale,render_aspect_);
    const VkExtent2D internal{visible.width,uint32_t(std::llround(960*scale))};
    if(internal.width>max_image_dimension_ || internal.height>max_image_dimension_) throw std::runtime_error("internal render extent exceeds device image limits");
    if(efb_extent_.width!=internal.width || efb_extent_.height!=internal.height) {
        if(depth_view_) vkDestroyImageView(device,depth_view_,nullptr);
        if(depth_) vkDestroyImage(device,depth_,nullptr);
        if(depth_memory_) vkFreeMemory(device,depth_memory_,nullptr);
        depth_view_=VK_NULL_HANDLE; depth_=VK_NULL_HANDLE; depth_memory_=VK_NULL_HANDLE;
        if(efb_view_) vkDestroyImageView(device,efb_view_,nullptr);
        if(efb_) vkDestroyImage(device,efb_,nullptr);
        if(efb_memory_) vkFreeMemory(device,efb_memory_,nullptr);
        efb_view_=VK_NULL_HANDLE; efb_=VK_NULL_HANDLE; efb_memory_=VK_NULL_HANDLE; efb_initialized_=false; ++targets_generation_;
        sweep_meshes(true);
        for(auto& texture:recycled_copies_) destroy_texture(texture);
        recycled_copies_.clear(); recycled_copy_bytes_=0;
        efb_extent_=internal;
        std::fprintf(stderr,"Native render: %ux%u EFB,720p multiplier %.4gx (requested %.4gx), scene %ux%u, output %ux%u\n",internal.width,internal.height,scale,configured_scale_,visible.width,visible.height,extent_.width,extent_.height);
    }
    if (!efb_) make_attachment(efb_,efb_memory_,efb_view_,efb_extent_);
    if (!depth_) make_attachment(depth_,depth_memory_,depth_view_,efb_extent_,true);
    if (upscaler_ && upscaler_kind_==UpscalerKind::off && !scene_path_wanted()) { upscaler_.reset(); reset_next_=true; }   // the last effect that needed the scene targets went away
    if (motion_outputs_ && (upscaler_kind_!=UpscalerKind::off || scene_path_wanted()) && !upscaler_) {
        if (upscaler_kind_==UpscalerKind::off || upscaler_kind_==UpscalerKind::blit) upscaler_=make_blit_upscaler();   // off: the effects' pass-through at ratio 1
        else if (upscaler_kind_==UpscalerKind::smaa) upscaler_=make_smaa_upscaler(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"SMAA idle"); });
        else if (upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) upscaler_=make_dlss_upscaler(*streamline_,upscale_mode_);
        else if (upscaler_kind_==UpscalerKind::fsr) {
            try { upscaler_=make_fsr_upscaler(device,physical_,shaders_.parent_path().parent_path()); }
            catch (const std::exception& error) { std::fprintf(stderr,"FSR unavailable: %s\n",error.what()); }
        }
        else std::fprintf(stderr, "Upscaling: %s is not available in this build\n", upscaler_kind_==UpscalerKind::dlss ? "DLSS" : "FSR");
    }
    if (!upscaler_ && scene_color_) destroy_scene_targets();   // no reconstruction any more: the internal targets go too
    if (upscaler_) {
        const auto internal=internal_extent({efb_extent_.width,efb_extent_.height},effective_upscale_mode(),upscaler_->minimum_internal({efb_extent_.width,efb_extent_.height}));
        if (scene_extent_.width!=internal.width || scene_extent_.height!=internal.height) {
            destroy_scene_targets();
            scene_extent_={internal.width,internal.height};
            make_attachment(scene_color_,scene_color_memory_,scene_color_view_,scene_extent_);
            make_attachment(scene_depth_,scene_depth_memory_,scene_depth_view_,scene_extent_,true);
            make_attachment(scene_motion_,scene_motion_memory_,scene_motion_view_,scene_extent_,false,kMotionFormat);
            make_attachment(scene_gbuffer_,scene_gbuffer_memory_,scene_gbuffer_view_,scene_extent_,false,kGBufferFormat);
            reset_next_=true;
            std::fprintf(stderr,"Upscaling: scene %ux%u -> %ux%u EFB (%s)\n",scene_extent_.width,scene_extent_.height,efb_extent_.width,efb_extent_.height,
                upscaler_kind_==UpscalerKind::dlss ? "DLSS" : upscaler_kind_==UpscalerKind::fsr ? "FSR" : upscaler_kind_==UpscalerKind::smaa ? "SMAA" : "blit");
        }
    }
    sync_rt_material_targets();
    configure_generation();
    for (auto& slot : slots_) make_attachment(slot.image,slot.memory,slot.view,extent_);
    resized_ = false;
}
void VulkanRenderer::configure_generation() {
    if (upscaler_ && frame_generation_ && upscaler_kind_==UpscalerKind::dlss && dlss_g_available_ && !dlss_g_)
        dlss_g_=std::make_unique<DlssFrameGeneration>(*streamline_,frame_generation_multiplier_);
    if (frame_generation_active() || (frame_generation_ && fsr_presentation_)) {
        if (hudless_extent_.width!=extent_.width || hudless_extent_.height!=extent_.height) {
            for (int i=0;i<2;++i) {
                if (hudless_view_[i]) vkDestroyImageView(device,hudless_view_[i],nullptr);
                if (hudless_[i]) vkDestroyImage(device,hudless_[i],nullptr);
                if (hudless_memory_[i]) vkFreeMemory(device,hudless_memory_[i],nullptr);
                make_attachment(hudless_[i],hudless_memory_[i],hudless_view_[i],extent_);
            }
            hudless_extent_=extent_;
        }
        if (upscaler_ && fsr_presentation_ && !fsr_g_->has_context()) {
            try { fsr_g_->create_context(extent_,scene_extent_,swapchain_format_); }
            catch (const std::exception& error) { std::fprintf(stderr,"FSR Frame Generation unavailable: %s\n",error.what()); }
        }
    }
}
bool VulkanRenderer::render(const gx::FramePacket& packet) {
    owner();
    if (!device) throw std::logic_error("renderer not initialized");
    if (!requested_.width || !requested_.height) return false;
    if (!packet.serial || packet.serial <= last_serial_) throw std::invalid_argument("frame serial must increase");
    if(packet.clear_depth>0xFFFFFF) throw std::invalid_argument("invalid native EFB clear depth");
    for(const auto& prompt:packet.prompts) {
        for(float value:prompt.efb_corners)if(!std::isfinite(value))throw std::invalid_argument("nonfinite native prompt geometry");
        for(float value:prompt.alpha)if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("invalid native prompt alpha");
        for(const auto& tint:prompt.tint)for(float value:tint)if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("invalid native prompt tint");
        for(float value:prompt.clip)if(!std::isfinite(value))throw std::invalid_argument("nonfinite native prompt clip");
        if(prompt.clip[2]<prompt.clip[0] || prompt.clip[3]<prompt.clip[1])throw std::invalid_argument("invalid native prompt clip");
    }
    const auto& display=packet.display_source;
    if(packet.present && (!display[2] || !display[3] || display[0]>=640 || display[2]>640-display[0] || display[1]>=480 || display[3]>480-display[1])) throw std::invalid_argument("invalid native display source rectangle");
    if (const auto now = std::chrono::steady_clock::now(); uc::rebuild_due(resized_, swapchain_ != VK_NULL_HANDLE, now, last_rebuild_)) {
        last_rebuild_ = now; rebuild();        if (resized_) return false;
    }
    if (!swapchain_) return false;   // while a resize storm postpones the rebuild the old swapchain keeps presenting
    reap_retired(false);
    Slot& slot = slots_[next_slot_];
    VkResult fence_status = vkGetFenceStatus(device, slot.fence);
    if (fence_status == VK_NOT_READY) return false;
    vk_check(fence_status, "frame slot fence");
    collect();   // after the check: a fence that finishes between an earlier collect() and the check would be reused unreported and its packet would never complete
    slot.copy_sources.clear();
    slot.mesh_refs.clear();   // the submission that used these meshes completed (fence checked above)
    if ((++renders_ & 255)==0) sweep_meshes(false);
    for(auto it=copy_textures_.begin();it!=copy_textures_.end();) {
        if(it->second.source.use_count()==1) {
            auto retired=std::move(it->second); it=copy_textures_.erase(it);
            recycle_copy_texture(retired);
        }
        else ++it;
    }
    Texture* copy_target=nullptr;
    if(packet.texture_copy) {
        const auto& request=*packet.texture_copy;
        const auto [x,y,width,height]=request.source;
        if(!width || !height || x>=640 || width>640-x || y>=480 || height>480-y || !request.target || !request.target->gpu_only) throw std::invalid_argument("invalid native EFB texture copy");
        auto found=copy_textures_.find(request.target.get());
        if(found==copy_textures_.end()) {
            auto texture=make_copy_texture(request.target);
            try { found=copy_textures_.emplace(request.target.get(),texture).first; } catch(...) { destroy_texture(texture); throw; }
        }
        if(found->second.initialized) throw std::invalid_argument("native EFB copy version cannot be overwritten");
        copy_target=&found->second; slot.copy_sources.push_back(request.target);
    }
    // Upscaling: a frame whose predecessor had a 3D scene draws into the internal scene targets until the scene boundary (the game's
    // copy of the finished scene, gx::SceneBoundary), is upscaled into efb_ there, and draws the rest (post chain, HUD) into efb_.
    const struct { gx::SceneBoundary boundary; bool frame_start, scene_phase, scene_mode, motion_cleared, gbuffer_cleared, reset_next; std::array<float,2> jitter; uint64_t whole_frame_upscales; size_t tracker; std::optional<gx::Projection> frame_projection; std::optional<ShadowFit> shadow_fit; bool shadow_cleared, rt_material_cleared; uint32_t rt_record_slot, rt_record_count; }
        upscale_before{boundary_,frame_start_,scene_phase_,scene_mode_,motion_cleared_,gbuffer_cleared_,reset_next_,jitter_,whole_frame_upscales_,tracker_.checkpoint(),frame_projection_,shadow_fit_,shadow_cleared_,rt_material_cleared_,rt_record_slot_,rt_record_count_};
    gx::PacketSummary summary;
    size_t scene_draws=0;          // draws [0, scene_draws) of this packet go to the scene targets
    bool upscale_here=false;       // the upscale runs after those draws, before the rest and before the packet's copy/present
    if (upscaler_) {
        if (frame_start_) {
            const bool scene=boundary_.frame_had_scene();
            if (scene!=scene_mode_) reset_next_=true;
            scene_mode_=scene_phase_=scene; frame_start_=false; motion_cleared_=gbuffer_cleared_=false; hudless_taken_=false;
            scene_lights_.clear(); frame_projection_.reset(); shadow_fit_.reset(); shadow_cleared_=false;
            rt_material_cleared_=false; rt_record_slot_=frame_index_%kFrameSlots; rt_record_count_=0;   // the slot's previous frame has completed (see rt_ below)
            rt_frame_=rt_scene_active();
            if (rt_frame_) {   // frame index modulo the in-flight slots: a frame has at least one submission, so the slot's previous frame has completed
                rt_.initialize(device,physical_,memory_,shaders_,uint32_t(kFrameSlots));
                rt_.begin_frame(frame_index_);
            }
            if (upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) {
                streamline_->begin_frame(frame_index_);
                streamline_->marker(Streamline::Marker::simulation_start);
                streamline_->marker(Streamline::Marker::simulation_end);
                streamline_->marker(Streamline::Marker::render_submit_start);
            }
            if (fsr_presentation_ && fsr_g_->has_context()) fsr_g_->begin_frame(fsr_generation_enabled() && scene && !packet.black_output,hudless_image(),frame_index_);
            if (dlss_g_) dlss_g_->set_active(dlss_generation_enabled() && scene && !packet.black_output,reset_next_);
            if (scene_phase_) {
                tracker_.begin_frame();
                jitter_=uses_jitter(upscaler_kind_) ? jitter_offset(upscale_frame_,jitter_phase_count(scene_extent_.width,efb_extent_.width)) : std::array<float,2>{};
            }
        }
        summary=gx::summarize(packet);
        const auto cut=boundary_.on_packet(summary);
        if (scene_phase_) {
            scene_draws=cut.value_or(packet.draws.size());
            upscale_here=cut.has_value() || packet.present;
            if (!cut && packet.present) ++whole_frame_upscales_;   // no scene copy: the HUD goes through the upscaler too
        }
    }
    std::vector<MotionBlock> motion_blocks;
    std::vector<uint32_t> draw_motion(scene_draws,kNoMotion);
    // RT material records (rt_material.h) of this packet's depth-writing scene draws, appended to the frame's at rt_record_count_.
    const bool rt_records=rt_material_active() && scene_material_;
    const uint32_t first_record=rt_record_count_;
    std::vector<uint32_t> draw_record(rt_records ? scene_draws : 0,kRtNoRecord);   // empty with RT off: read only when rt_records
    std::vector<RtMaterialRecord> records;
    std::vector<const gx::DrawUniform*> record_uniforms;
    std::unordered_set<const gx::TexturePixels*> used_textures;
    for(const auto& draw:packet.draws) for(size_t map=0;map<8;++map) used_textures.insert(draw_texture(draw,map).get());
    std::unordered_map<const gx::TexturePixels*,size_t> renderer_references;
    for(const auto& cached_slot:slots_) for(const auto& texture:cached_slot.textures) ++renderer_references[texture.source.get()];
    // CPU-cache/queued-packet owners retain textures across different passes; this slot's fence is retired.
    for (auto it=slot.textures.begin();it!=slot.textures.end();) {
        const bool owned=it->source && size_t(it->source.use_count())>renderer_references.at(it->source.get());
        const bool used=!it->source || used_textures.contains(it->source.get());
        if (!used && !owned) { destroy_texture(*it); it=slot.textures.erase(it); }
        else { if (it->initialized) destroy_buffer(it->upload); ++it; }
    }
    // Per draw: where its vertices live (a persistent mesh buffer, or this frame's dynamic buffer for non-mesh draws) and which palette it reads.
    struct DrawGpu { PipelineKey key{}; PipelineChoice choice{}; const Buffer* vertices=nullptr; const Buffer* indices=nullptr; VkDeviceSize vertex_offset=0, index_offset=0; uint32_t index_count=0, first_index=0; int32_t first_vertex=0; uint32_t palette=0, current=0, indexed=0, bank=kNoBank; uint64_t mesh_id=0; };   // mesh_id: GpuMesh::id of a cached mesh draw (0 for an inline draw)
    std::vector<DrawGpu> gpu_draws; gpu_draws.reserve(packet.draws.size());
    std::unordered_map<const gx::TextureBank*,uint32_t> bank_of;   // one bank per distinct texture matrix set
    std::vector<const gx::TextureBank*> banks;
    std::unordered_map<const gx::Transforms*,uint32_t> palette_of;   // one palette per distinct transform block
    std::vector<const gx::Transforms*> palette_blocks;
    size_t vertex_count = 0, index_count = 0;   // dynamic (non-mesh) draws only
    for (const auto& draw : packet.draws) {
        if(draw.state->viewport) {
            const auto& v=*draw.state->viewport;
            for(auto value:v) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native draw viewport");
            if(std::abs(v[0])>2048 || std::abs(v[1])>2048 || v[2]<=0 || v[3]<=0 || v[2]>1024 || v[3]>1024 || v[4]<0 || v[4]>1 || v[5]<0 || v[5]>1) throw std::invalid_argument("invalid native draw viewport");
        }
        if(draw.state->scissor && ((*draw.state->scissor)[2]<0 || (*draw.state->scissor)[3]<0)) throw std::invalid_argument("negative native draw scissor extent");
        // shortcut: indirect texturing/depth/material parity remains TD-036.
        const size_t group=(draw.primitive==0xA8 || draw.primitive==0xB0) ? 2 : 3;
        const bool indexed=draw.state->descriptors[0]!=gx::Mode::none;
        DrawGpu info; info.indexed=indexed; info.current=draw.transforms->current;
        info.key=pipeline_key(draw); info.choice=pipeline_for(info.key);
        if (draw.mesh) {
            GpuMesh& mesh=gpu_mesh(draw.mesh);   // uploaded and validated on first use
            if (mesh.index_count % group != 0) throw std::invalid_argument("index count does not match native draw topology");
            gx::validate_draw_rows(*draw.transforms,indexed,mesh.rows,draw.callsite);
            if (slot.mesh_refs.empty() || slot.mesh_refs.back()!=draw.mesh) slot.mesh_refs.push_back(draw.mesh);
            info.vertices=info.indices=&mesh.slab->buffer; info.vertex_offset=mesh.vertex_offset; info.index_offset=mesh.index_offset; info.index_count=mesh.index_count; info.mesh_id=mesh.id;
        } else {
            const auto vertices = draw.vertex_view(); const auto indices = draw.index_view();
            if (indices.size() % group != 0) throw std::invalid_argument("index count does not match native draw topology");
            for (uint32_t index : indices) if (index >= vertices.size()) throw std::invalid_argument("draw index exceeds owned vertices");
            if (vertices.size() > size_t(INT32_MAX) - vertex_count || indices.size() > size_t(UINT32_MAX) - index_count) throw std::invalid_argument("frame exceeds indexed draw limits");
            gx::validate_draw_rows(*draw.transforms,indexed,gx::used_rows(vertices),draw.callsite);
            info.index_count=uint32_t(indices.size()); info.first_index=uint32_t(index_count); info.first_vertex=int32_t(vertex_count);
            vertex_count += vertices.size(); index_count += indices.size();
        }
        const auto [palette,inserted]=palette_of.try_emplace(draw.transforms.block().get(),uint32_t(palette_blocks.size()));
        if (inserted) palette_blocks.push_back(palette->first);
        info.palette=palette->second;
        static const bool no_banks=std::getenv("UC_NO_TMI")!=nullptr;   // debug: ignore per-vertex texture matrix ids (A/B against the old behaviour)
        if (!no_banks && gx::has_texture_matrix_index(*draw.state)) {
            if (!draw.texture_bank) throw std::invalid_argument("native draw has per-vertex texture matrix indices but no texture bank");
            const auto [bank,added]=bank_of.try_emplace(draw.texture_bank.get(),uint32_t(banks.size()));
            if (added) banks.push_back(bank->first);
            info.bank=bank->second;
        }
        if (const size_t d=gpu_draws.size(); d<scene_draws) {   // scene draw: last frame's matrices of the same draw for its motion vectors
            const auto key=tracker_.key(draw);
            if(motion_write_mask(info.key)) {
                MotionBlock block{};
                if (const auto* previous=tracker_.previous(key)) {
                    block.kind=1; block.prev_rows=previous->transforms->projection;
                    const auto [palette,inserted]=palette_of.try_emplace(previous->transforms.get(),uint32_t(palette_blocks.size()));
                    if (inserted) palette_blocks.push_back(palette->first);
                    block.prev_palette=palette->second;
                } else { block.kind=2; block.prev_rows=tracker_.clip_to_prev_clip(); }
                draw_motion[d]=uint32_t(motion_blocks.size()); motion_blocks.push_back(block);
            }
            if (rt_records && rt_material_write(info.key)) {
                const auto& uniforms=*draw.uniforms;
                const auto lighting=gx::channel_lighting(uniforms.channels[0]);
                const uint32_t index=first_record+uint32_t(records.size());
                records.push_back({uniforms.materials[0],uniforms.ambient[0],lighting.mask,index*8,rt_record_flags(lighting,light_model_valid(draw.material,lighting,gx::channel_lighting(uniforms.channels[1]))),0.f});
                record_uniforms.push_back(&uniforms);
                draw_record[d]=index;
            }
            tracker_.record(key,draw.transforms.block(),summary.draws[d].z_write);
            if (summary.draws[d].perspective) scene_projection_=draw.transforms->projection;
            if (summary.draws[d].perspective && summary.draws[d].z_write && !frame_projection_) {   // the world's projection: the first depth-writing perspective draw (the weapon comes later with its own)
                frame_projection_=draw.transforms->projection;
                // The map uses last frame's dominant light (stable by its hysteresis), so the casters can be drawn as the scene packets arrive.
                if (effects_.shadows!=EffectLevel::off && dominant_light_ && dominant_light_->kind==gx::LightKind::directional)
                    shadow_fit_=fit_shadow(*frame_projection_,*dominant_light_,kShadowDistance,shadow_preset(effects_.shadows).resolution);
            }
        }
        gpu_draws.push_back(info);
    }
    if (vertex_count && index_count) {
        if (slot.vertices.size < VkDeviceSize(vertex_count) * sizeof(gx::GpuMeshVertex)) {
            destroy_buffer(slot.vertices); slot.vertices = make_buffer(VkDeviceSize(vertex_count) * sizeof(gx::GpuMeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        }
        if (slot.indices.size < VkDeviceSize(index_count) * sizeof(uint32_t)) {
            destroy_buffer(slot.indices); slot.indices = make_buffer(VkDeviceSize(index_count) * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        }
        for (size_t d=0; d<packet.draws.size(); ++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            if (info.vertices) continue;   // persistent mesh buffer
            const auto vertices = draw.vertex_view(); const auto indices = draw.index_view();
            auto* target=static_cast<gx::GpuMeshVertex*>(slot.vertices.mapped)+info.first_vertex;
            for (size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
            if (!indices.empty()) std::memcpy(static_cast<uint32_t*>(slot.indices.mapped) + info.first_index, indices.data(), indices.size() * sizeof(uint32_t));
        }
    }
    if (!palette_blocks.empty()) {
        const VkDeviceSize palette_bytes=VkDeviceSize(palette_blocks.size())*sizeof(gx::PaletteBlock);
        if (slot.palettes.size < palette_bytes) { destroy_buffer(slot.palettes); slot.palettes = make_buffer(palette_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|rt_input_usage()); }
        auto* palettes=static_cast<gx::PaletteBlock*>(slot.palettes.mapped);
        for (size_t i=0;i<palette_blocks.size();++i) gx::fill_palette(palettes[i], *palette_blocks[i]);
    }
    {   // the descriptor always has a bank bound; a frame without per-vertex texture matrices binds one unused entry
        const VkDeviceSize bank_bytes=VkDeviceSize(std::max<size_t>(banks.size(),1))*sizeof(gx::TextureBank);
        if (slot.banks.size < bank_bytes) { destroy_buffer(slot.banks); slot.banks = make_buffer(bank_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        for (size_t i=0;i<banks.size();++i) std::memcpy(static_cast<gx::TextureBank*>(slot.banks.mapped)+i,banks[i],sizeof(gx::TextureBank));
    }
    {   // bound even when no draw reads it
        const VkDeviceSize motion_bytes=VkDeviceSize(std::max<size_t>(motion_blocks.size(),1))*sizeof(MotionBlock);
        if (slot.motions.size < motion_bytes) { destroy_buffer(slot.motions); slot.motions = make_buffer(motion_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        if (!motion_blocks.empty()) std::memcpy(slot.motions.mapped, motion_blocks.data(), motion_blocks.size()*sizeof(MotionBlock));
    }
    if (!records.empty()) {   // the frame's record and light arrays: grow-only per frame-in-flight slot
        constexpr VkDeviceSize kLightsBytes=8*sizeof(gx::LightUniform);
        const auto grow=[&](Buffer& buffer, VkDeviceSize used, VkDeviceSize needed) {
            if (buffer.size>=needed) return;
            Buffer bigger=make_buffer(std::max(needed,buffer.size*2),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (used) std::memcpy(bigger.mapped,buffer.mapped,size_t(used));   // the frame's earlier packets (no GPU work reads these buffers before the boundary)
            destroy_buffer(buffer); buffer=bigger;
        };
        Buffer& record_buffer=rt_records_[rt_record_slot_]; Buffer& light_buffer=rt_lights_[rt_record_slot_];
        const VkDeviceSize total=first_record+records.size();
        grow(record_buffer,first_record*sizeof(RtMaterialRecord),total*sizeof(RtMaterialRecord));
        grow(light_buffer,first_record*kLightsBytes,total*kLightsBytes);
        std::memcpy(static_cast<RtMaterialRecord*>(record_buffer.mapped)+first_record,records.data(),records.size()*sizeof(RtMaterialRecord));
        auto* lights=static_cast<std::byte*>(light_buffer.mapped)+first_record*kLightsBytes;
        for (size_t i=0;i<record_uniforms.size();++i) std::memcpy(lights+i*kLightsBytes,record_uniforms[i]->lights.data(),kLightsBytes);
        rt_record_count_=uint32_t(total);
    }
    if (!packet.draws.empty()) {
        if (packet.draws.size()>UINT32_MAX) throw std::invalid_argument("too many native texture descriptors");
        if(packet.draws.size()>UINT32_MAX/8) throw std::invalid_argument("native texture descriptor count overflows");
        const uint32_t count=uint32_t(packet.draws.size());
        if(slot.uniforms.size<VkDeviceSize(count)*uniform_stride_) { destroy_buffer(slot.uniforms); slot.uniforms=make_buffer(VkDeviceSize(count)*uniform_stride_,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); }
        if(count>slot.descriptor_capacity) {
            if(slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
            slot.texture_pool=VK_NULL_HANDLE; slot.descriptor_capacity=0; slot.texture_sets.clear();
            VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count*8},{VK_DESCRIPTOR_TYPE_SAMPLER,count*8},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count*3}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=count; pool.poolSizeCount=4; pool.pPoolSizes=sizes;
            vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.texture_pool),"texture descriptor pool");
            std::vector<VkDescriptorSetLayout> layouts(count,texture_layout_);
            slot.texture_sets.resize(count);
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.texture_pool; allocate.descriptorSetCount=count; allocate.pSetLayouts=layouts.data();
            vk_check(vkAllocateDescriptorSets(device,&allocate,slot.texture_sets.data()),"texture descriptor sets");
            slot.descriptor_capacity=count; ++descriptor_pool_creations_;
        }
        // The slot fence has retired; rewrite its retained sets before their next submission.
        slot.textures.reserve(count);
        std::unordered_map<const gx::TexturePixels*,size_t> texture_indices;
        for(size_t i=0;i<slot.textures.size();++i) texture_indices.emplace(slot.textures[i].source.get(),i);
        VkDeviceSize uniform_offset=0;
        for (size_t i=0;i<count;++i) {
            std::array<VkDescriptorImageInfo,8> image{};
            for(size_t map=0;map<8;++map) {
                const auto* chosen=&draw_texture(packet.draws[i],map);
                if(*chosen && (*chosen)->gpu_only) {
                    auto copied=copy_textures_.find(chosen->get());
                    if(copied!=copy_textures_.end() && copied->second.initialized) {
                        slot.copy_sources.push_back(*chosen);
                        image[map]={VK_NULL_HANDLE,copied->second.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        continue;
                    }
                    if(!lost_copies_.contains(chosen->get())) throw std::invalid_argument("native EFB texture used before its copy");
                    chosen=&blank_;
                }
                const auto& source=*chosen;
                auto found=texture_indices.find(source.get());
                if (found==texture_indices.end()) {
                    auto texture=make_texture(source.get()); texture.source=source;
                    try { slot.textures.push_back(texture); } catch (...) { destroy_texture(texture); throw; }
                    found=texture_indices.emplace(source.get(),slot.textures.size()-1).first;
                }
                image[map]={VK_NULL_HANDLE,slot.textures[found->second].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            }
            if(i==0 || packet.draws[i].uniforms.block()!=packet.draws[i-1].uniforms.block() ||
               !std::equal(gpu_draws[i].key.begin()+1,gpu_draws[i].key.begin()+68,gpu_draws[i-1].key.begin()+1)) {
                uniform_offset=i*uniform_stride_;
                auto* uniform_at=static_cast<std::byte*>(slot.uniforms.mapped)+uniform_offset;
                std::memcpy(uniform_at,&*packet.draws[i].uniforms,sizeof(gx::DrawUniform));
                std::array<uint32_t,kTevWords> tev{}; std::copy(gpu_draws[i].key.begin()+1,gpu_draws[i].key.begin()+68,tev.begin());
                std::memcpy(uniform_at+sizeof(gx::DrawUniform),tev.data(),sizeof tev);
            }
            VkDescriptorBufferInfo uniform{slot.uniforms.buffer,uniform_offset,kDrawUniformBytes};
            std::array<VkDescriptorImageInfo,8> samplers{};
            for(size_t map=0;map<8;++map) {
                const auto wrap=packet.draws[i].texture_wrap[map];
                if(!texture_samplers_[wrap]) throw std::invalid_argument("invalid native texture sampler mode");
                samplers[map]={i<scene_draws ? scene_samplers_[wrap] : texture_samplers_[wrap],VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
            }
            VkDescriptorBufferInfo palette{slot.palettes.buffer,0,VK_WHOLE_SIZE};
            VkDescriptorBufferInfo bank{slot.banks.buffer,0,VK_WHOLE_SIZE};
            VkDescriptorBufferInfo motion{slot.motions.buffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[6]{};
            for (uint32_t binding=0;binding<6;++binding) { writes[binding].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[binding].dstSet=slot.texture_sets[i]; writes[binding].dstBinding=binding; writes[binding].descriptorCount=1; }
            writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].descriptorCount=8; writes[0].pImageInfo=image.data();
            writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].descriptorCount=8; writes[1].pImageInfo=samplers.data();
            writes[2].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[2].pBufferInfo=&uniform;
            writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo=&palette;
            writes[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo=&bank;
            writes[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo=&motion;
            vkUpdateDescriptorSets(device,6,writes,0,nullptr);
        }
    }
    uint32_t image_index = 0;
    bool suboptimal=false;
    PresentImage* present_image=nullptr;
    if (packet.present) {
        const auto acquire_start = std::chrono::steady_clock::now();
        VkResult acquired = present_.acquire(device, swapchain_, 1000000ull, slot.acquired, VK_NULL_HANDLE, &image_index);
        timings_.acquire_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquire_start).count();
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {   // retried later: the packet must look unseen to the per-frame upscaling state
            boundary_=upscale_before.boundary; frame_start_=upscale_before.frame_start; scene_phase_=upscale_before.scene_phase; scene_mode_=upscale_before.scene_mode;
            motion_cleared_=upscale_before.motion_cleared; gbuffer_cleared_=upscale_before.gbuffer_cleared; reset_next_=upscale_before.reset_next; jitter_=upscale_before.jitter; whole_frame_upscales_=upscale_before.whole_frame_upscales; frame_projection_=upscale_before.frame_projection; shadow_fit_=upscale_before.shadow_fit; shadow_cleared_=upscale_before.shadow_cleared;
            rt_material_cleared_=upscale_before.rt_material_cleared; rt_record_slot_=upscale_before.rt_record_slot; rt_record_count_=upscale_before.rt_record_count;
            tracker_.rollback(upscale_before.tracker);
            return false;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) resized_ = true;   // stale swapchain while the window is resized: the frame is rendered but not shown, the guest must not wait for the rebuild
        else {
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) vk_check(acquired, "acquire image");
            suboptimal = acquired == VK_SUBOPTIMAL_KHR;
            present_image = &images_.at(image_index);
            if (present_image->pending) {
                vk_check(vkWaitForFences(device, 1, &present_image->fence, VK_TRUE, 1000000000ull), "reuse presentation fence");
                present_image->pending = false;
            }
            vk_check(vkResetFences(device, 1, &present_image->fence), "reset present fence");
        }
    }
    vk_check(vkResetCommandBuffer(slot.command, 0), "reset command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin frame");
    const uint32_t first_query = uint32_t((&slot - slots_.data()) * 2);
    vkCmdResetQueryPool(slot.command, timing_pool_, first_query, 2);
    vkCmdWriteTimestamp2(slot.command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing_pool_, first_query);
    pass_timer_.reset(slot.command, first_query / 2);
    slot.timed = true;
    for (const auto& texture : slot.textures) {
        if (texture.initialized) continue;
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,texture.mip_levels);
        std::vector<VkBufferImageCopy> copies(texture.mip_levels); VkDeviceSize at=0;
        for(uint32_t i=0;i<texture.mip_levels;++i) {
            const auto width=std::max(1u,texture.extent.width>>i),height=std::max(1u,texture.extent.height>>i);
            copies[i].bufferOffset=at; copies[i].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1}; copies[i].imageExtent={width,height,1}; at+=VkDeviceSize(width)*height*4;
        }
        vkCmdCopyBufferToImage(slot.command,texture.upload.buffer,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,texture.mip_levels);
    }
    // A render target of the frame: efb_ (output resolution) or the internal scene targets. Between passes color is TRANSFER_SRC, depth
    // DEPTH_ATTACHMENT and motion COLOR_ATTACHMENT; *initialized false means never written (cleared on first use).
    struct PassTarget { VkImage color, depth; VkImageView color_view, depth_view, motion_view, gbuffer_view; VkExtent2D extent; bool* initialized; };
    const PassTarget efb_target{efb_,depth_,efb_view_,depth_view_,VK_NULL_HANDLE,VK_NULL_HANDLE,efb_extent_,&efb_initialized_};
    const PassTarget scene_target{scene_color_,scene_depth_,scene_color_view_,scene_depth_view_,scene_motion_view_,scene_gbuffer_view_,scene_extent_,&scene_initialized_};
    const auto depth_stages=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    // Debug: UC_SKIP_CALLSITES=<hex>[,<hex>...] leaves the draws of those guest callsites out (finding what a callsite draws).
    static const std::vector<uint32_t> skipped_callsites=[] {
        std::vector<uint32_t> sites;
        if(const char* text=std::getenv("UC_SKIP_CALLSITES")) for(const char* at=text;*at;) {
            char* end=nullptr; const auto value=std::strtoul(at,&end,16);
            if(end==at) break;
            sites.push_back(uint32_t(value)); at=*end==',' ? end+1 : end;
        }
        return sites;
    }();
    auto record_pass=[&](const PassTarget& target, size_t first, size_t last, bool scene) {
    const bool initialized=*target.initialized;
    barrier(slot.command, target.color, initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE, initialized ? VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT : 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView = target.color_view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = initialized ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (size_t i=0;i<4;++i) color.clearValue.color.float32[i]=packet.clear_color[i]/255.f;
    VkRenderingAttachmentInfo colors[5]{color,{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};   // [1]: motion, [2]: G-buffer, [3], [4]: RT material and albedo; none bound outside the scene pass
    const bool bind_motion=scene && (!motion_cleared_ || std::any_of(draw_motion.begin()+first,draw_motion.begin()+last,[](uint32_t index) { return index!=kNoMotion; }));
    if (bind_motion) {   // clear once, then bind only for passes whose depth-writing draws produce vectors
        barrier(slot.command,scene_motion_,motion_initialized_ && motion_cleared_ ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colors[1].imageView=target.motion_view; colors[1].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp=motion_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; colors[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        motion_cleared_=motion_initialized_=true;
        scene_motion_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (scene && effects_need_gbuffer()) {   // cleared by the first scene pass of a frame (no surface), then loaded; bound only while an effect reads it
        barrier(slot.command,scene_gbuffer_,gbuffer_initialized_ && gbuffer_cleared_ ? scene_gbuffer_layout_ : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colors[2].imageView=target.gbuffer_view; colors[2].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[2].loadOp=gbuffer_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; colors[2].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        gbuffer_cleared_=gbuffer_initialized_=true;
        scene_gbuffer_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (scene && rt_records) {   // the RT pair the same way: cleared once per frame (no record, no albedo), then loaded
        for (VkImage image:{scene_material_,scene_albedo_})
            barrier(slot.command,image,rt_material_initialized_ && rt_material_cleared_ ? scene_rt_layout_ : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        for (auto [attachment,view]:{std::pair{&colors[3],scene_material_view_},std::pair{&colors[4],scene_albedo_view_}}) {
            attachment->imageView=view; attachment->imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment->loadOp=rt_material_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; attachment->storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        }
        colors[3].clearValue.color.uint32[0]=kRtNoRecord;
        rt_material_cleared_=rt_material_initialized_=true;
        scene_rt_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea = {{0, 0}, target.extent}; rendering.layerCount = 1; rendering.colorAttachmentCount = scene_color_attachments(); rendering.pColorAttachments = colors;
    const VkImageLayout depth_before=!initialized ? VK_IMAGE_LAYOUT_UNDEFINED : scene ? scene_depth_layout_ : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier(slot.command,target.depth,depth_before,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            initialized ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,initialized ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
            depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    if (scene) scene_depth_layout_=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth.imageView=target.depth_view; depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp=initialized ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil={packet.clear_depth/float(0xFFFFFF),0};
    rendering.pDepthAttachment=&depth;
    vkCmdBeginRendering(slot.command, &rendering);
    if (scene && scene_clear_.pending) {   // the last present's clear, which GX applied to the EFB this scene renders into
        const auto& r=scene_clear_.region;
        const uint32_t x=uint64_t(r[0])*target.extent.width/640, y=uint64_t(r[1])*target.extent.height/480;
        const uint32_t right=std::max(x+1,uint32_t(uint64_t(r[0]+r[2])*target.extent.width/640)), bottom=std::max(y+1,uint32_t(uint64_t(r[1]+r[3])*target.extent.height/480));
        VkClearRect rectangle{{{int32_t(x),int32_t(y)},{right-x,bottom-y}},0,1};
        VkClearAttachment clears[2]{}; clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        for (size_t i=0;i<4;++i) clears[0].clearValue.color.float32[i]=scene_clear_.color[i]/255.f;
        clears[1].clearValue.depthStencil={scene_clear_.depth/float(0xFFFFFF),0};
        vkCmdClearAttachments(slot.command,2,clears,1,&rectangle);
        scene_clear_.pending=false;
    }
    VkViewport viewport{0, 0, float(target.extent.width), float(target.extent.height), 0, 1}; VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(slot.command, 0, 1, &viewport); vkCmdSetScissor(slot.command, 0, 1, &scissor);
    if (!gpu_draws.empty()) {
        const Buffer* bound_vertices=nullptr; const Buffer* bound_indices=nullptr; VkDeviceSize bound_vertex_offset=0, bound_index_offset=0;
        for (size_t d=first; d<last; ++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            const size_t set_index=d;
            if (!info.index_count) continue;
            if (!bloom_ && gx::bloom_composite(draw)) continue;
            if (scene && effects_need_lights()) scene_lights_.add(*draw.uniforms);
            if (std::find(skipped_callsites.begin(),skipped_callsites.end(),draw.callsite)!=skipped_callsites.end()) continue;
            const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
            const auto area=uc::efb_viewport(v,target.extent.width,target.extent.height);
            VkViewport draw_viewport{area.x,area.y,area.width,area.height,v[4],v[5]};
            auto draw_scissor=scissor;
            if(draw.state->scissor) {
                const auto& s=*draw.state->scissor;
                const auto x=std::clamp<int64_t>(int64_t(s[0])*target.extent.width/640,0,target.extent.width);
                const auto y=std::clamp<int64_t>(int64_t(s[1])*target.extent.height/480,0,target.extent.height);
                const auto right=std::clamp<int64_t>((int64_t(s[0])+s[2])*target.extent.width/640,x,target.extent.width);
                const auto bottom=std::clamp<int64_t>((int64_t(s[1])+s[3])*target.extent.height/480,y,target.extent.height);
                draw_scissor={{int32_t(x),int32_t(y)},{uint32_t(right-x),uint32_t(bottom-y)}};
            }
            if (draw.late_aim && aim_source_) {
                const auto latest=aim_source_();
                const float dx=(latest[0]-packet.aim_consumed[0])*gx::kAimToNdcX*0.5f*area.width, dy=(latest[1]-packet.aim_consumed[1])*gx::kAimToNdcY*0.5f*area.height;
                if (dx!=0 || dy!=0) { draw_viewport.x+=dx; draw_viewport.y+=dy; draw_scissor=scissor; ++late_aim_draws_; }
            }
            vkCmdSetViewport(slot.command,0,1,&draw_viewport); vkCmdSetScissor(slot.command,0,1,&draw_scissor);
            const Buffer& vertex_buffer=info.vertices ? *info.vertices : slot.vertices;
            const Buffer& index_buffer=info.indices ? *info.indices : slot.indices;
            if (&vertex_buffer!=bound_vertices || info.vertex_offset!=bound_vertex_offset) { VkDeviceSize offset=info.vertex_offset; vkCmdBindVertexBuffers(slot.command,0,1,&vertex_buffer.buffer,&offset); bound_vertices=&vertex_buffer; bound_vertex_offset=info.vertex_offset; }
            if (&index_buffer!=bound_indices || info.index_offset!=bound_index_offset) { vkCmdBindIndexBuffer(slot.command,index_buffer.buffer,info.index_offset,VK_INDEX_TYPE_UINT32); bound_indices=&index_buffer; bound_index_offset=info.index_offset; }
            vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,info.choice.pipeline);
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[set_index],0,nullptr);
            DrawPush push{draw.transforms->projection,info.palette,info.current,info.indexed,0,info.bank};
            push.viewport[0]=draw_viewport.x; push.viewport[1]=draw_viewport.y; push.viewport[2]=draw_viewport.width; push.viewport[3]=draw_viewport.height;
            if (effects_.per_pixel_lighting && motion_outputs_) push.effects|=kEffectPerPixelLighting;   // only lit draws change: nothing else has a lighting channel on
            if (scene) {
                push.motion=draw_motion[d];
                if (summary.draws[d].perspective) { push.jitter[0]=jitter_[0]; push.jitter[1]=jitter_[1]; }   // post-style quads stay unjittered
                if (rt_records && draw_record[d]!=kRtNoRecord) {
                    push.record=draw_record[d];
                    push.effects|=kEffectRtMaterial|(records[draw_record[d]-first_record].flags&kRtModelValid ? kEffectRtModelValid : 0u);
                }
            }
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            vkCmdDrawIndexed(slot.command, info.index_count, 1, info.first_index, info.first_vertex, 0);
            const auto depth=draw.uniforms->pixel[1]&31;
            if((draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !(depth&1 && ((depth>>1)&7)==0)) {
                // GX destination alpha replaces stored alpha after RGB blending.
                const auto alpha=pipeline_for(pipeline_key(draw,true));
                vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,alpha.pipeline);
                if (alpha.uber) { push.alpha_pass=1; vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push); }
                vkCmdDrawIndexed(slot.command,info.index_count,1,info.first_index,info.first_vertex,0);
            }
        }
    }
    vkCmdEndRendering(slot.command);
    barrier(slot.command, target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    *target.initialized=true;
    };
    // The shadow map: this packet's casters (mesh draws that write depth, in the scene, without an alpha cut-out) drawn from the light, in the packet's own command buffer.
    auto record_shadow=[&](size_t first, size_t last) {
        if (!shadow_fit_ || effects_.shadows==EffectLevel::off || !shadow_vertex_module_) return;
        std::vector<size_t> casters;
        for (size_t d=first;d<last;++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            // the ray tracing instance rule; alpha cut-outs (foliage, fences) are not opaque and cast no shadow here (a limit)
            if (rt_instance_opaque(draw.mesh && info.vertices,info.index_count!=0,summary.draws[d].perspective,summary.draws[d].z_write,draw.uniforms->pixel[0],VkPrimitiveTopology(info.key[69]))==true) casters.push_back(d);
        }
        if (casters.empty()) return;
        const uint32_t resolution=shadow_preset(effects_.shadows).resolution;
        if (shadow_map_.resolution()!=resolution) {   // a preset change: the map is recreated and this frame's earlier casters are lost
            vk_check(present_.wait_idle(device),"shadow map resize");
            shadow_map_.ensure(resolution); shadow_cleared_=false;
        }
        const auto timer_slot=uint32_t(next_slot_);
        pass_timer_.begin(slot.command,timer_slot,GpuPass::shadow);
        barrier(slot.command,shadow_map_.image(),shadow_cleared_ ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth.imageView=shadow_map_.view(); depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp=shadow_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil={1.f,0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},{resolution,resolution}}; rendering.layerCount=1; rendering.pDepthAttachment=&depth;
        vkCmdBeginRendering(slot.command,&rendering);
        if (!shadow_pipeline_) shadow_pipeline_=make_shadow_pipeline();
        vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadow_pipeline_);
        const VkViewport viewport{0,0,float(resolution),float(resolution),0,1}; const VkRect2D scissor{{0,0},{resolution,resolution}};
        vkCmdSetViewport(slot.command,0,1,&viewport); vkCmdSetScissor(slot.command,0,1,&scissor);
        for (const size_t d:casters) {
            const auto& info=gpu_draws[d];
            VkDeviceSize vertex_offset=info.vertex_offset;
            vkCmdBindVertexBuffers(slot.command,0,1,&info.vertices->buffer,&vertex_offset);
            vkCmdBindIndexBuffer(slot.command,info.indices->buffer,info.index_offset,VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[d],0,nullptr);
            DrawPush push{shadow_fit_->view_to_light,info.palette,info.current,info.indexed,0,info.bank};   // the light's matrix takes the place of the projection; motion stays "none"
            push.viewport[2]=push.viewport[3]=float(resolution);   // the vertex stage divides its jitter by the viewport (zero jitter here)
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            vkCmdDrawIndexed(slot.command,info.index_count,1,info.first_index,info.first_vertex,0);
        }
        vkCmdEndRendering(slot.command);
        shadow_cleared_=true;
        pass_timer_.end(slot.command,timer_slot,GpuPass::shadow);
    };
    // Ray tracing: this packet's instances (the caster rule, cut-outs included as non-opaque) into one BLAS, in the packet's own command buffer.
    auto record_rt=[&](size_t first, size_t last) {
        if (!rt_frame_) return;
        const auto address=[&](const Buffer& buffer) { VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer=buffer.buffer; return vkGetBufferDeviceAddress(device,&info); };
        std::vector<RtGeometry> geometries;
        for (size_t d=first;d<last;++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            const auto opaque=rt_instance_opaque(draw.mesh && info.vertices,info.index_count!=0,summary.draws[d].perspective,summary.draws[d].z_write,draw.uniforms->pixel[0],VkPrimitiveTopology(info.key[69]));
            if (!opaque) continue;
            RtGeometry geometry;
            geometry.vertices=address(*info.vertices)+info.vertex_offset; geometry.indices=address(*info.indices)+info.index_offset;
            geometry.vertex_count=uint32_t(draw.mesh->vertices.size()); geometry.index_count=info.index_count; geometry.first_index=info.first_index; geometry.first_vertex=info.first_vertex;
            geometry.palette=info.palette; geometry.current=info.current; geometry.indexed=info.indexed; geometry.opaque=*opaque; geometry.palettes=address(slot.palettes);
            geometry.skinned=info.indexed!=0;   // characters and enemies: a rig light's rays see only these (the instance mask), a lamp's rays see the whole scene
            if (!geometry.skinned) {   // a static draw: its mesh's BLAS is cached in object space, the instance carries the draw's object-to-view rows (current row and the two after it)
                geometry.mesh_id=info.mesh_id;
                for (uint32_t row=0;row<3;++row) geometry.transform[row]=draw.transforms->position_rows[info.current+row];
            }
            geometries.push_back(geometry);
        }
        if (geometries.empty()) return;
        const auto timer_slot=uint32_t(next_slot_);
        pass_timer_.begin(slot.command,timer_slot,GpuPass::rt_as);
        rt_.record_packet(slot.command,geometries);
        if (upscale_here) rt_as_open_=true;   // the boundary's TLAS build follows in this submission: one timed span (a query is written once per submission)
        else pass_timer_.end(slot.command,timer_slot,GpuPass::rt_as);
    };
    const bool scene_only=scene_phase_ && !upscale_here;   // the whole packet belongs to the scene: copy and clear act on the scene targets
    const bool writes_scene=scene_phase_ && (scene_draws || !scene_initialized_);   // overwrites the depth and vectors frame generation may still read
    if (writes_scene) { record_pass(scene_target,0,scene_draws,true); record_shadow(0,scene_draws); record_rt(0,scene_draws); }
    if (upscale_here) {
        if (scene_draws || packet.present) scene_restore_copy_.reset();   // another draw changed the scene since the saved copy
        record_upscale(slot.command,packet.black_output);
    }
    // DLSS-G's HUD-less colour: the frame after its post chain, before the HUD packet (the first packet after the scene boundary that draws
    // without copying), at back-buffer size; at the present when no such packet came (it then includes the HUD).
    const bool hud_packet=!packet.texture_copy && !packet.draws.empty();
    auto take_hudless=[&] {
        const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
        const auto hudless=hudless_image();
        barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        blit.srcOffsets[0]={int32_t(viewport[0]*efb_extent_.width/640),int32_t(viewport[1]*efb_extent_.height/480),0};
        blit.srcOffsets[1]={int32_t((viewport[0]+viewport[2])*efb_extent_.width/640),int32_t((viewport[1]+viewport[3])*efb_extent_.height/480),1};
        blit.dstOffsets[1]={int32_t(hudless_extent_.width),int32_t(hudless_extent_.height),1};
        vkCmdBlitImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudless.image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);   // as the present copies
        barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
        if (dlss_generation_enabled()) dlss_g_->tag_hudless(slot.command,hudless);
        hudless_taken_=true;
    };
    const bool hudless_wanted=(dlss_generation_enabled() || (fsr_generation_enabled() && fsr_g_->active())) && hudless_[0] && scene_mode_ && !scene_phase_ && !hudless_taken_;
    if (hudless_wanted && efb_initialized_ && hud_packet) take_hudless();
    if (!scene_only && (!efb_initialized_ || scene_draws<packet.draws.size())) record_pass(efb_target,scene_draws,packet.draws.size(),false);
    if (hudless_wanted && !hudless_taken_ && packet.present) take_hudless();
    const PassTarget& active=scene_only ? scene_target : efb_target;
    if(copy_target) record_texture_copy(slot.command,active.color,active.extent,packet.texture_copy->source,*copy_target,VK_IMAGE_LAYOUT_UNDEFINED);
    if (scene_only) {
        if (copy_target && !packet.clear_after_copy) scene_restore_copy_=packet.texture_copy;
        else if (!packet.draws.empty() || (copy_target && packet.clear_after_copy)) scene_restore_copy_.reset();
    }
    if (packet.present) {
        const bool prompts=!packet.prompts.empty() && !packet.black_output, menu=bool(menu_build_);
        if(prompts)prompt_overlay_.initialize(instance_,physical_,device,queue_,family_,uint32_t(slots_.size()));
        if(menu)menu_overlay_.initialize(window_,instance_,physical_,device,queue_,family_,uint32_t(slots_.size()),menu_setup_);
        barrier(slot.command,slot.image,slot.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                slot.initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,slot.initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.srcOffsets[0]={int32_t(uint64_t(display[0])*efb_extent_.width/640),int32_t(uint64_t(display[1])*efb_extent_.height/480),0};
        copy.srcOffsets[1]={std::max(copy.srcOffsets[0].x+1,int32_t(uint64_t(display[0]+display[2])*efb_extent_.width/640)),std::max(copy.srcOffsets[0].y+1,int32_t(uint64_t(display[1]+display[3])*efb_extent_.height/480)),1};
        copy.dstOffsets[1]={int32_t(extent_.width),int32_t(extent_.height),1};
        if(packet.black_output) {
            const VkClearColorValue black{}; const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        } else vkCmdBlitImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_NEAREST);
        if(prompts || menu) {
            barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=slot.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ui{VK_STRUCTURE_TYPE_RENDERING_INFO};ui.renderArea.extent=extent_;ui.layerCount=1;ui.colorAttachmentCount=1;ui.pColorAttachments=&color;
            vkCmdBeginRendering(slot.command,&ui);
            if(prompts)prompt_overlay_.record(slot.command,packet.prompts,extent_,display,prompt_style_);
            if(menu)menu_overlay_.record(slot.command,extent_,menu_build_);
            vkCmdEndRendering(slot.command);
            barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        } else
        barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    auto clear_region=[&](const PassTarget& active, bool with_motion) {   // after a copy: GX clears the copied EFB region when asked
            barrier(slot.command,active.depth,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
            barrier(slot.command,active.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo colors[5]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}},depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            const bool clear_motion=with_motion && active.motion_view && motion_cleared_;   // the scene's vectors in the cleared region go with it
            const bool clear_gbuffer=with_motion && active.gbuffer_view && gbuffer_cleared_ && scene_gbuffer_layout_==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // and so do its surfaces
            const bool clear_rt=with_motion && active.gbuffer_view && scene_material_ && rt_material_cleared_ && scene_rt_layout_==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // and their records
            for (auto [attachment,view]:{std::pair{&colors[0],active.color_view},std::pair{&colors[1],clear_motion ? active.motion_view : VK_NULL_HANDLE},std::pair{&colors[2],clear_gbuffer ? active.gbuffer_view : VK_NULL_HANDLE},
                                         std::pair{&colors[3],clear_rt ? scene_material_view_ : VK_NULL_HANDLE},std::pair{&colors[4],clear_rt ? scene_albedo_view_ : VK_NULL_HANDLE}}) {
                attachment->imageView=view; attachment->imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; attachment->loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; attachment->storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            }
            depth.imageView=active.depth_view; depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo clear_render{VK_STRUCTURE_TYPE_RENDERING_INFO}; clear_render.renderArea.extent=active.extent; clear_render.layerCount=1; clear_render.colorAttachmentCount=clear_rt ? 5 : clear_gbuffer ? 3 : clear_motion ? 2 : 1; clear_render.pColorAttachments=colors; clear_render.pDepthAttachment=&depth;
            const auto& source=copy_target ? packet.texture_copy->source : packet.display_source;
            const uint32_t x=uint64_t(source[0])*active.extent.width/640,y=uint64_t(source[1])*active.extent.height/480;
            const uint32_t right=std::max(x+1,uint32_t(uint64_t(source[0]+source[2])*active.extent.width/640)),bottom=std::max(y+1,uint32_t(uint64_t(source[1]+source[3])*active.extent.height/480));
            VkClearRect rectangle{{{int32_t(x),int32_t(y)},{right-x,bottom-y}},0,1};
            VkClearAttachment clears[6]{}; clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            uint32_t clear_count=2;
            if (clear_motion) { clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=1; }
            if (clear_gbuffer) { clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=2; }   // cleared to zero: no surface
            if (clear_rt) {   // no record, no albedo
                clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count].clearValue.color.uint32[0]=kRtNoRecord; clears[clear_count++].colorAttachment=3;
                clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=4;
            }
            for(size_t i=0;i<4;++i) clears[0].clearValue.color.float32[i]=packet.clear_color[i]/255.f;
            clears[1].clearValue.depthStencil={packet.clear_depth/float(0xFFFFFF),0};
            vkCmdBeginRendering(slot.command,&clear_render);
            vkCmdClearAttachments(slot.command,clear_count,clears,1,&rectangle);
            vkCmdEndRendering(slot.command);
            barrier(slot.command,active.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    };
    if (packet.clear_after_copy && (packet.present || copy_target)) {
        clear_region(active,true);
        // The scene targets keep this frame until it is presented (frame generation reads depth and vectors there); the next frame's first
        // scene pass applies this clear to them.
        if (packet.present && upscaler_ && !scene_only) scene_clear_={true,packet.display_source,packet.clear_color,packet.clear_depth};
    }
    if(present_image) {
        barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
        vkCmdBlitImage(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        const auto present_layout=fsr_presentation_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : streamline_swapchain() ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,present_layout,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
    }
    vkCmdWriteTimestamp2(slot.command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing_pool_, first_query + 1);
    vk_check(vkEndCommandBuffer(slot.command), "end frame"); vk_check(vkResetFences(device, 1, &slot.fence), "reset frame fence");
    VkSemaphoreSubmitInfo waits[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}}; uint32_t wait_count = 0;
    if (present_image) { waits[wait_count].semaphore = slot.acquired; waits[wait_count].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; ++wait_count; }
    if (writes_scene && dlss_g_ && dlss_g_->inputs_fence() && dlss_g_->inputs_fence_value() > fg_waited_value_) {   // DLSS-G guide §15.1
        waits[wait_count].semaphore = dlss_g_->inputs_fence(); waits[wait_count].value = dlss_g_->inputs_fence_value(); waits[wait_count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        fg_waited_value_ = waits[wait_count].value; ++wait_count;
    }
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore = present_image ? present_image->ready : VK_NULL_HANDLE; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = slot.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.waitSemaphoreInfoCount = wait_count; submit.signalSemaphoreInfoCount = present_image ? 1 : 0; submit.commandBufferInfoCount = 1;
    submit.pWaitSemaphoreInfos = waits; submit.pSignalSemaphoreInfos = &signal; submit.pCommandBufferInfos = &command;
    if (packet.present && upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) streamline_->marker(Streamline::Marker::render_submit_end);
    const auto submit_start = std::chrono::steady_clock::now();
    vk_check(vkQueueSubmit2(queue_, 1, &submit, slot.fence), "submit frame");
    timings_.submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();
    if(copy_target) copy_target->initialized=true;
    for (auto& texture : slot.textures) texture.initialized=true;
    if (packet.present) { frame_start_=true; ++frame_index_; scene_restore_copy_.reset(); }
    slot.serial = packet.serial; slot.submitted = true; slot.initialized |= packet.present; slot.copied=packet.present; slot.reported = false; last_serial_ = packet.serial; next_slot_ = (next_slot_ + 1) % slots_.size();   // efb_initialized_/scene_initialized_ are set by the passes that wrote them
    if (!present_image) return true;
    VkSwapchainPresentFenceInfoKHR present_fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR}; present_fence.swapchainCount = 1; present_fence.pFences = &present_image->fence;
    // With DLSS-G Streamline presents (and paces) the frames itself and never signals a present fence: its swapchain images are not tracked here.
    const bool fenced = !streamline_swapchain() && !fsr_presentation_;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.pNext = fenced ? &present_fence : nullptr; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &present_image->ready;
    present.swapchainCount = 1; present.pSwapchains = &swapchain_; present.pImageIndices = &image_index;
    const auto present_start = std::chrono::steady_clock::now();
    const bool marked = upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active();
    if (marked) streamline_->marker(Streamline::Marker::present_start);
    VkResult result = present_.present(queue_, &present);   // with DLSS-G Streamline presents the generated frames here too
    if (marked) streamline_->marker(Streamline::Marker::present_end);
    if (dlss_generation_enabled()) dlss_g_->after_present();
    if (fsr_presentation_) fsr_g_->after_present();
    timings_.present_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_start).count();
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) { present_image->pending = fenced; ++presentations_; }
    else if (result != VK_ERROR_OUT_OF_DATE_KHR) vk_check(result, "present frame");
    if (suboptimal || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) resized_ = true;
    return true;
}
void VulkanRenderer::shutdown() {
    if (!instance_ && !device) return;
    owner();
    if (device) {
        retire_images();
        sweep_meshes(true);
        prompt_overlay_.shutdown();
        menu_overlay_.shutdown();
        for(auto& [source,texture]:copy_textures_) destroy_texture(texture);
        copy_textures_.clear();
        for(auto& texture:recycled_copies_) destroy_texture(texture);
        recycled_copies_.clear(); recycled_copy_bytes_=0;
        if(depth_view_) vkDestroyImageView(device,depth_view_,nullptr);
        if(depth_) vkDestroyImage(device,depth_,nullptr);
        if(depth_memory_) vkFreeMemory(device,depth_memory_,nullptr);
        depth_view_=VK_NULL_HANDLE; depth_=VK_NULL_HANDLE; depth_memory_=VK_NULL_HANDLE;
        if (efb_view_) vkDestroyImageView(device,efb_view_,nullptr);
        if (efb_) vkDestroyImage(device,efb_,nullptr);
        if (efb_memory_) vkFreeMemory(device,efb_memory_,nullptr);
        efb_view_=VK_NULL_HANDLE; efb_=VK_NULL_HANDLE; efb_memory_=VK_NULL_HANDLE; efb_initialized_=false; efb_extent_={};
        dlss_g_.reset();
        fsr_g_.reset();   // its swapchain context was emptied by retire_images; the contexts go before the device
        for (int i=0;i<2;++i) {
            if (hudless_view_[i]) vkDestroyImageView(device,hudless_view_[i],nullptr);
            if (hudless_[i]) vkDestroyImage(device,hudless_[i],nullptr);
            if (hudless_memory_[i]) vkFreeMemory(device,hudless_memory_[i],nullptr);
            hudless_view_[i]=VK_NULL_HANDLE; hudless_[i]=VK_NULL_HANDLE; hudless_memory_[i]=VK_NULL_HANDLE;
        }
        hudless_extent_={};
        upscaler_.reset();
        rcas_.shutdown(); occlusion_.shutdown(); motion_blur_.shutdown(); rt_shadow_.shutdown(); rt_shadow_denoiser_.shutdown(); rt_.shutdown();
        destroy_scene_targets();
        for (auto& buffer:rt_records_) destroy_buffer(buffer);
        for (auto& buffer:rt_lights_) destroy_buffer(buffer);
        rt_record_count_=0;
        for(auto& sampler:scene_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
        stop_workers();
        for (auto [key,pipeline]:pipelines_) vkDestroyPipeline(device,pipeline,nullptr);
        for (auto [key,pipeline]:uber_pipelines_) vkDestroyPipeline(device,pipeline,nullptr);
        pipelines_.clear(); uber_pipelines_.clear();
        if (pipeline_cache_) { save_pipeline_cache(); vkDestroyPipelineCache(device,pipeline_cache_,nullptr); pipeline_cache_=VK_NULL_HANDLE; }
        if (shadow_pipeline_) vkDestroyPipeline(device,shadow_pipeline_,nullptr);
        shadow_pipeline_=VK_NULL_HANDLE; shadow_map_.shutdown();
        for (auto module:{vertex_module_,fragment_module_,uber_module_,shadow_vertex_module_}) if (module) vkDestroyShaderModule(device,module,nullptr);
        shadow_vertex_module_=VK_NULL_HANDLE;
        vertex_module_=fragment_module_=uber_module_=VK_NULL_HANDLE;
        if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
        if (texture_layout_) vkDestroyDescriptorSetLayout(device,texture_layout_,nullptr);
        for(auto& sampler:texture_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
        texture_layout_=VK_NULL_HANDLE;
        for (auto& slot : slots_) {
            if (slot.fence) vkDestroyFence(device, slot.fence, nullptr);
            if (slot.acquired) vkDestroySemaphore(device, slot.acquired, nullptr);
            slot = {};
        }
        pass_timer_.shutdown();
        if (timing_pool_) vkDestroyQueryPool(device, timing_pool_, nullptr);
        if (pool_) vkDestroyCommandPool(device, pool_, nullptr);
        if (streamline_ && streamline_->active()) {   // slShutdown before the device and instance go; the surface it created first
            if (surface_) { streamline_->destroy_surface(instance_, surface_); surface_ = VK_NULL_HANDLE; }
            streamline_->shutdown();
        }
        vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    messenger_ = VK_NULL_HANDLE;
    vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE;
}
} // namespace uc
