#include "vulkan_renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace uc {
namespace {
void vk_check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
             VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkImageMemoryBarrier2 image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image_barrier.srcStageMask = source; image_barrier.srcAccessMask = source_access;
    image_barrier.dstStageMask = destination; image_barrier.dstAccessMask = destination_access;
    image_barrier.oldLayout = before; image_barrier.newLayout = after;
    image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image; image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) renderer.errors_.fetch_add(1);
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
    return VK_FALSE;
}
VulkanRenderer::~VulkanRenderer() {
    try { shutdown(); } catch (const std::exception& error) { std::fprintf(stderr, "Vulkan shutdown failed: %s\n", error.what()); }
}
void VulkanRenderer::initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders) {
    if (instance_) throw std::logic_error("renderer already initialized");
    owner_ = std::this_thread::get_id(); window_ = window;
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
    vk_check(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance");
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!create) throw std::runtime_error("debug-utils creation function unavailable");
        vk_check(create(instance_, &debug_info, nullptr, &messenger_), "debug messenger");
    }
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) throw std::runtime_error(SDL_GetError());
    uint32_t n = 0; vk_check(vkEnumeratePhysicalDevices(instance_, &n, nullptr), "physical device count");
    std::vector<VkPhysicalDevice> physical(n); vk_check(vkEnumeratePhysicalDevices(instance_, &n, physical.data()), "physical devices");
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
    std::fprintf(stderr, "Vulkan device: %s; presentation retirement: %s\n", properties.deviceName, chosen_maintenance);
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue_info.queueFamilyIndex = family_; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR retirement{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR}; retirement.swapchainMaintenance1 = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.pNext = &retirement; features13.dynamicRendering = VK_TRUE; features13.synchronization2 = VK_TRUE;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, chosen_maintenance};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext = &features13;
    VkPhysicalDeviceFeatures core{}; vkGetPhysicalDeviceFeatures(physical_,&core);
    logic_ops_=core.logicOp;
    VkPhysicalDeviceFeatures enabled{}; enabled.logicOp=logic_ops_; device_info.pEnabledFeatures=&enabled;
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2; device_info.ppEnabledExtensionNames = device_extensions;
    vk_check(vkCreateDevice(physical_, &device_info, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, family_, 0, &queue_); vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool_info.queueFamilyIndex = family_;
    vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &pool_), "command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool = pool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 2;
    VkCommandBuffer commands[2]; vk_check(vkAllocateCommandBuffers(device, &allocation, commands), "command buffers");
    for (size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].command = commands[i];
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(device, &fence, nullptr, &slots_[i].fence), "frame fence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &slots_[i].acquired), "acquire semaphore");
    }
    shaders_=shaders; pipeline_for({});
    int width, height; if (!SDL_GetWindowSizeInPixels(window, &width, &height)) throw std::runtime_error(SDL_GetError());
    resize(uint32_t(std::max(0, width)), uint32_t(std::max(0, height)));
    if (requested_.width && requested_.height) rebuild();
}
uint32_t VulkanRenderer::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw std::runtime_error("required Vulkan memory type unavailable");
}
VulkanRenderer::Buffer VulkanRenderer::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer buffer; buffer.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.size = size; create.usage = usage;
    vk_check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vk_check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "buffer memory");
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
VulkanRenderer::Texture VulkanRenderer::make_texture(const gx::TexturePixels* pixels) {
    const gx::TextureLevel white{1,1,{255,255,255,255}};
    if (pixels && pixels->levels.size()!=1) throw std::invalid_argument("native texture GPU path currently requires one mip level (TD-036)");
    const auto& level=pixels ? pixels->levels[0] : white;
    if (!level.width || !level.height || level.width>1024 || level.height>1024 || level.rgba.size()!=size_t(level.width)*level.height*4)
        throw std::invalid_argument("invalid owned RGBA texture");
    Texture texture; texture.extent={level.width,level.height};
    try {
        texture.upload=make_buffer(level.rgba.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(texture.upload.mapped,level.rgba.data(),level.rgba.size());
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType=VK_IMAGE_TYPE_2D; create.format=VK_FORMAT_R8G8B8A8_UNORM;
        create.extent={level.width,level.height,1}; create.mipLevels=create.arrayLayers=1; create.samples=VK_SAMPLE_COUNT_1_BIT;
        create.tiling=VK_IMAGE_TILING_OPTIMAL; create.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        vk_check(vkCreateImage(device,&create,nullptr,&texture.image),"texture image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,texture.image,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
        allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device,&allocate,nullptr,&texture.memory),"texture memory");
        vk_check(vkBindImageMemory(device,texture.image,texture.memory,0),"bind texture memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=texture.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=VK_FORMAT_R8G8B8A8_UNORM; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
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
void VulkanRenderer::destroy_texture(Texture& texture) {
    if (texture.view) vkDestroyImageView(device,texture.view,nullptr);
    if (texture.image) vkDestroyImage(device,texture.image,nullptr);
    if (texture.memory) vkFreeMemory(device,texture.memory,nullptr);
    destroy_buffer(texture.upload);
    texture={};
}
void VulkanRenderer::retire_textures(Slot& slot) {
    if (slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
    slot.texture_pool=VK_NULL_HANDLE; slot.texture_sets.clear();
    for (auto& texture : slot.textures) {
        destroy_texture(texture);
    }
    slot.textures.clear();
}
VkPipeline VulkanRenderer::pipeline_for(const gx::BlendState& blend) {
    if (blend.mode>3 || blend.source>7 || blend.destination>7 || blend.logic>15) throw std::invalid_argument("invalid native blend state");
    if (blend.mode==2 && !logic_ops_) throw std::runtime_error("native GX logic operation requires GPU logicOp support");
    const uint32_t key=blend.mode|(blend.source<<2)|(blend.destination<<5)|(blend.logic<<8)|(uint32_t(blend.color_write)<<12)|(uint32_t(blend.alpha_write)<<13);
    auto found=pipelines_.find(key);
    if (found!=pipelines_.end()) return found->second;
    auto pipeline=make_pipeline(blend);
    try { pipelines_.emplace(key,pipeline); } catch (...) { vkDestroyPipeline(device,pipeline,nullptr); throw; }
    return pipeline;
}
VkPipeline VulkanRenderer::make_pipeline(const gx::BlendState& mode) {
    VkShaderModule vertex = shader(device, shaders_ / "smoke.vert.spv"), fragment = VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    try {
        fragment = shader(device, shaders_ / "smoke.frag.spv");
        if (!layout_) {
        VkPushConstantRange projection{VK_SHADER_STAGE_VERTEX_BIT,0,uint32_t(sizeof(gx::Projection))};
        VkDescriptorSetLayoutBinding texture_bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo texture_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; texture_layout.bindingCount=2; texture_layout.pBindings=texture_bindings;
        vk_check(vkCreateDescriptorSetLayout(device,&texture_layout,nullptr,&texture_layout_),"texture descriptor layout");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device,&sampler,nullptr,&texture_sampler_),"texture sampler");
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.pushConstantRangeCount=1; layout_info.pPushConstantRanges=&projection;
        layout_info.setLayoutCount=1; layout_info.pSetLayouts=&texture_layout_;
        vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_), "pipeline layout");
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "vertexMain";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "fragmentMain";
        static_assert(sizeof(gx::GpuVertex)==72 && sizeof(gx::Projection)==64);
        VkVertexInputBindingDescription binding{0, uint32_t(sizeof(gx::GpuVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[] = {
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(gx::GpuVertex,position))},
            {1,0,VK_FORMAT_R8G8B8A8_UNORM,uint32_t(offsetof(gx::GpuVertex,color))},
            {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,uint32_t(offsetof(gx::GpuVertex,position_rows))},
            {3,0,VK_FORMAT_R32G32B32A32_SFLOAT,uint32_t(offsetof(gx::GpuVertex,position_rows)+16)},
            {4,0,VK_FORMAT_R32G32B32A32_SFLOAT,uint32_t(offsetof(gx::GpuVertex,position_rows)+32)},
            {5,0,VK_FORMAT_R32G32_SFLOAT,uint32_t(offsetof(gx::GpuVertex,texcoord))}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding; input.vertexAttributeDescriptionCount = 6; input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask=(mode.color_write ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT : 0)|(mode.alpha_write ? VK_COLOR_COMPONENT_A_BIT : 0);
        const VkBlendFactor sources[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        const VkBlendFactor destinations[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        blend.blendEnable=mode.mode==1 || mode.mode==3;
        blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : sources[mode.source];
        blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : destinations[mode.destination];
        blend.colorBlendOp=blend.alphaBlendOp=mode.mode==3 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount = 1; blending.pAttachments = &blend;
        blending.logicOpEnable=mode.mode==2; blending.logicOp=VkLogicOp(mode.logic);
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; create.pNext = &rendering; create.stageCount = 2; create.pStages = stages; create.pVertexInputState = &input;
        create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &raster; create.pMultisampleState = &samples;
        create.pColorBlendState = &blending; create.pDynamicState = &dynamic; create.layout = layout_;
        vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline), "graphics pipeline");
    } catch (...) { if (fragment) vkDestroyShaderModule(device, fragment, nullptr); vkDestroyShaderModule(device, vertex, nullptr); throw; }
    vkDestroyShaderModule(device, fragment, nullptr); vkDestroyShaderModule(device, vertex, nullptr);
    return pipeline;
}
void VulkanRenderer::collect() {
    for (auto& slot : slots_) {
        if (!slot.submitted || slot.reported) continue;
        VkResult status = vkGetFenceStatus(device, slot.fence);
        if (status == VK_NOT_READY) continue;
        vk_check(status, "frame fence status");
        completed_.push_back(slot.serial); slot.reported = true;
    }
}
std::vector<uint64_t> VulkanRenderer::poll_completions() {
    owner(); collect();
    std::sort(completed_.begin(), completed_.end());
    auto completed = std::move(completed_); completed_.clear(); return completed;
}
RenderPixels VulkanRenderer::capture(uint64_t serial) {
    owner();
    for (auto& slot : slots_) {
        if (slot.submitted && slot.serial == serial && slot.copied) {
            vk_check(vkGetFenceStatus(device, slot.fence), "capture requires completed frame");
            collect();
            if (!slot.pixels.buffer) slot.pixels = make_buffer(VkDeviceSize(extent_.width) * extent_.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            vk_check(vkResetCommandBuffer(slot.command, 0), "reset capture command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin capture");
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyImageToBuffer(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.pixels.buffer, 1, &copy);
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
            return {serial, extent_.width, extent_.height, {data, data + size_t(slot.pixels.size)}};
        }
    }
    throw std::runtime_error("capture serial no longer retained: " + std::to_string(serial));
}
void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    owner(); requested_ = {width, height}; resized_ = true;
}
void VulkanRenderer::retire_images() {
    if (!device) return;
    VkResult status = vkDeviceWaitIdle(device);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) vk_check(status, "retire device work");
    if (status == VK_SUCCESS) {
        collect();
        for (const auto& image : images_)
            if (image.pending) vk_check(vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull), "presentation retirement fence");
    }
    for (auto& image : images_) {
        if (image.ready) vkDestroySemaphore(device, image.ready, nullptr);
        if (image.fence) vkDestroyFence(device, image.fence, nullptr);
    }
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    for (auto& slot : slots_) {
        retire_textures(slot);
        destroy_buffer(slot.vertices); destroy_buffer(slot.indices); destroy_buffer(slot.pixels);
        if (slot.view) vkDestroyImageView(device, slot.view, nullptr);
        if (slot.image) vkDestroyImage(device, slot.image, nullptr);
        if (slot.memory) vkFreeMemory(device, slot.memory, nullptr);
        slot.view = VK_NULL_HANDLE; slot.image = VK_NULL_HANDLE; slot.memory = VK_NULL_HANDLE;
        slot.submitted = slot.reported = slot.initialized = slot.copied = false; slot.serial = 0;
    }
}
void VulkanRenderer::make_attachment(VkImage& target,VkDeviceMemory& memory,VkImageView& target_view,VkExtent2D extent) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {extent.width, extent.height, 1}; image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vk_check(vkCreateImage(device, &image, nullptr, &target), "native attachment image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, target, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device, &allocate, nullptr, &memory), "attachment memory"); vk_check(vkBindImageMemory(device, target, memory, 0), "bind attachment");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = target; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = VK_FORMAT_R8G8B8A8_UNORM; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device, &view, nullptr, &target_view), "attachment view");
}
void VulkanRenderer::rebuild() {
    retire_images();
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
    VkFormatFeatureFlags source_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
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
    vk_check(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain_), "swapchain");
    vk_check(vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr), "swapchain image count");
    std::vector<VkImage> swapchain_images(count); vk_check(vkGetSwapchainImagesKHR(device, swapchain_, &count, swapchain_images.data()), "swapchain images");
    images_.resize(count);
    for (size_t i = 0; i < images_.size(); ++i) {
        images_[i].image = swapchain_images[i];
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &images_[i].ready), "per-image present semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk_check(vkCreateFence(device, &fence, nullptr, &images_[i].fence), "per-image present fence");
    }
    if (!efb_) { efb_extent_=extent_; make_attachment(efb_,efb_memory_,efb_view_,efb_extent_); }
    for (auto& slot : slots_) make_attachment(slot.image,slot.memory,slot.view,extent_);
    resized_ = false;
}
bool VulkanRenderer::render(const gx::FramePacket& packet) {
    owner();
    if (!device) throw std::logic_error("renderer not initialized");
    if (!requested_.width || !requested_.height) return false;
    if (!packet.serial || packet.serial <= last_serial_) throw std::invalid_argument("frame serial must increase");
    if (resized_) rebuild();
    if (resized_ || !swapchain_) return false;
    collect();
    Slot& slot = slots_[next_slot_];
    VkResult fence_status = vkGetFenceStatus(device, slot.fence);
    if (fence_status == VK_NOT_READY) return false;
    vk_check(fence_status, "frame slot fence");
    if (slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
    slot.texture_pool=VK_NULL_HANDLE; slot.texture_sets.clear();
    // The slot fence owns all prior reads; retain only this packet's immutable versions.
    for (auto it=slot.textures.begin();it!=slot.textures.end();) {
        const bool used=std::any_of(packet.draws.begin(),packet.draws.end(),[&](const auto& draw){return draw.texture==it->source;});
        if (!used) { destroy_texture(*it); it=slot.textures.erase(it); }
        else { if (it->initialized) destroy_buffer(it->upload); ++it; }
    }
    size_t vertex_count = 0, index_count = 0;
    for (const auto& draw : packet.draws) {
        pipeline_for(draw.blend);
        // shortcut: native transforms/map0 sampling active; TEV/depth/material parity remains TD-036.
        if (draw.primitive != 0x80 && draw.primitive != 0x90 && draw.primitive != 0x98 && draw.primitive != 0xA0) throw std::invalid_argument("Vulkan smoke requires triangle geometry");
        if (draw.indices.size() % 3 != 0) throw std::invalid_argument("triangle index count is not divisible by three");
        for (uint32_t index : draw.indices) if (index >= draw.vertices.size()) throw std::invalid_argument("draw index exceeds owned vertices");
        if (draw.vertices.size() > size_t(INT32_MAX) - vertex_count || draw.indices.size() > size_t(UINT32_MAX) - index_count) throw std::invalid_argument("frame exceeds indexed draw limits");
        vertex_count += draw.vertices.size(); index_count += draw.indices.size();
    }
    if (vertex_count && index_count) {
        if (slot.vertices.size < VkDeviceSize(vertex_count) * sizeof(gx::GpuVertex)) {
            destroy_buffer(slot.vertices); slot.vertices = make_buffer(VkDeviceSize(vertex_count) * sizeof(gx::GpuVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        }
        if (slot.indices.size < VkDeviceSize(index_count) * sizeof(uint32_t)) {
            destroy_buffer(slot.indices); slot.indices = make_buffer(VkDeviceSize(index_count) * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        }
        size_t vertex_offset = 0, index_offset = 0;
        for (const auto& draw : packet.draws) {
            for (size_t i=0;i<draw.vertices.size();++i)
                static_cast<gx::GpuVertex*>(slot.vertices.mapped)[vertex_offset+i]=gx::gpu_vertex(draw.vertices[i],draw.transforms,draw.state.descriptors[0]!=gx::Mode::none,draw.callsite);
            if (!draw.indices.empty()) std::memcpy(static_cast<uint32_t*>(slot.indices.mapped) + index_offset, draw.indices.data(), draw.indices.size() * sizeof(uint32_t));
            vertex_offset += draw.vertices.size(); index_offset += draw.indices.size();
        }
    }
    if (!packet.draws.empty()) {
        if (packet.draws.size()>UINT32_MAX) throw std::invalid_argument("too many native texture descriptors");
        const uint32_t count=uint32_t(packet.draws.size());
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count},{VK_DESCRIPTOR_TYPE_SAMPLER,count}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=count; pool.poolSizeCount=2; pool.pPoolSizes=sizes;
        vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.texture_pool),"texture descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(count,texture_layout_);
        slot.texture_sets.resize(count); slot.textures.reserve(count);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.texture_pool; allocate.descriptorSetCount=count; allocate.pSetLayouts=layouts.data();
        vk_check(vkAllocateDescriptorSets(device,&allocate,slot.texture_sets.data()),"texture descriptor sets");
        for (size_t i=0;i<count;++i) {
            const auto& source=packet.draws[i].texture;
            auto found=std::find_if(slot.textures.begin(),slot.textures.end(),[&](const auto& texture){return texture.source==source;});
            if (found==slot.textures.end()) {
                auto texture=make_texture(source.get()); texture.source=source;
                try { slot.textures.push_back(texture); } catch (...) { destroy_texture(texture); throw; }
                found=std::prev(slot.textures.end());
            }
            VkDescriptorImageInfo image{VK_NULL_HANDLE,found->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo sampler{texture_sampler_,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t binding=0;binding<2;++binding) { writes[binding].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[binding].dstSet=slot.texture_sets[i]; writes[binding].dstBinding=binding; writes[binding].descriptorCount=1; }
            writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo=&image;
            writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].pImageInfo=&sampler;
            vkUpdateDescriptorSets(device,2,writes,0,nullptr);
        }
    }
    uint32_t image_index = 0;
    bool suboptimal=false;
    PresentImage* present_image=nullptr;
    if (packet.present) {
        VkResult acquired = vkAcquireNextImageKHR(device, swapchain_, 1000000ull, slot.acquired, VK_NULL_HANDLE, &image_index);
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return false;
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { resized_ = true; return false; }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) vk_check(acquired, "acquire image");
        suboptimal = acquired == VK_SUBOPTIMAL_KHR;
        present_image = &images_.at(image_index);
        if (present_image->pending) {
            vk_check(vkWaitForFences(device, 1, &present_image->fence, VK_TRUE, 1000000000ull), "reuse presentation fence");
            present_image->pending = false;
        }
        vk_check(vkResetFences(device, 1, &present_image->fence), "reset present fence");
    }
    vk_check(vkResetCommandBuffer(slot.command, 0), "reset command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin frame");
    for (const auto& texture : slot.textures) {
        if (texture.initialized) continue;
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={texture.extent.width,texture.extent.height,1};
        vkCmdCopyBufferToImage(slot.command,texture.upload.buffer,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barrier(slot.command, efb_, efb_initialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            efb_initialized_ ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE, efb_initialized_ ? VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT : 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView = efb_view_; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = efb_initialized_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (size_t i=0;i<4;++i) color.clearValue.color.float32[i]=packet.clear_color[i]/255.f;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea = {{0, 0}, efb_extent_}; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
    vkCmdBeginRendering(slot.command, &rendering);
    VkViewport viewport{0, 0, float(efb_extent_.width), float(efb_extent_.height), 0, 1}; VkRect2D scissor{{0, 0}, efb_extent_};
    vkCmdSetViewport(slot.command, 0, 1, &viewport); vkCmdSetScissor(slot.command, 0, 1, &scissor);
    if (vertex_count && index_count) {
        VkDeviceSize offset = 0; vkCmdBindVertexBuffers(slot.command, 0, 1, &slot.vertices.buffer, &offset); vkCmdBindIndexBuffer(slot.command, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        uint32_t first_index = 0; int32_t first_vertex = 0;
        size_t texture_index=0;
        for (const auto& draw : packet.draws) {
            vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_for(draw.blend));
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[texture_index++],0,nullptr);
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT,0,uint32_t(sizeof(gx::Projection)),draw.transforms.projection.data());
            vkCmdDrawIndexed(slot.command, uint32_t(draw.indices.size()), 1, first_index, first_vertex, 0); first_index += uint32_t(draw.indices.size()); first_vertex += int32_t(draw.vertices.size());
        }
    }
    vkCmdEndRendering(slot.command);
    barrier(slot.command, efb_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (packet.present) {
        barrier(slot.command,slot.image,slot.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                slot.initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,slot.initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.srcOffsets[1]={int32_t(efb_extent_.width),int32_t(efb_extent_.height),1};
        copy.dstOffsets[1]={int32_t(extent_.width),int32_t(extent_.height),1};
        vkCmdBlitImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_NEAREST);
        barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        if (packet.clear_after_copy) {
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue clear{};
            for (size_t i=0;i<4;++i) clear.float32[i]=packet.clear_color[i]/255.f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
        vkCmdBlitImage(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    vk_check(vkEndCommandBuffer(slot.command), "end frame"); vk_check(vkResetFences(device, 1, &slot.fence), "reset frame fence");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; wait.semaphore = slot.acquired; wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore = present_image ? present_image->ready : VK_NULL_HANDLE; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = slot.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.waitSemaphoreInfoCount = submit.signalSemaphoreInfoCount = packet.present ? 1 : 0; submit.commandBufferInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait; submit.pSignalSemaphoreInfos = &signal; submit.pCommandBufferInfos = &command;
    vk_check(vkQueueSubmit2(queue_, 1, &submit, slot.fence), "submit frame");
    for (auto& texture : slot.textures) texture.initialized=true;
    slot.serial = packet.serial; slot.submitted = true; slot.initialized |= packet.present; slot.copied=packet.present; efb_initialized_=true; slot.reported = false; last_serial_ = packet.serial; next_slot_ = (next_slot_ + 1) % slots_.size();
    if (!packet.present) return true;
    VkSwapchainPresentFenceInfoKHR present_fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR}; present_fence.swapchainCount = 1; present_fence.pFences = &present_image->fence;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.pNext = &present_fence; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &present_image->ready;
    present.swapchainCount = 1; present.pSwapchains = &swapchain_; present.pImageIndices = &image_index;
    VkResult result = vkQueuePresentKHR(queue_, &present);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) { present_image->pending = true; ++presentations_; }
    else if (result != VK_ERROR_OUT_OF_DATE_KHR) vk_check(result, "present frame");
    if (suboptimal || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) resized_ = true;
    return true;
}
void VulkanRenderer::shutdown() {
    if (!instance_ && !device) return;
    owner();
    if (device) {
        retire_images();
        if (efb_view_) vkDestroyImageView(device,efb_view_,nullptr);
        if (efb_) vkDestroyImage(device,efb_,nullptr);
        if (efb_memory_) vkFreeMemory(device,efb_memory_,nullptr);
        efb_view_=VK_NULL_HANDLE; efb_=VK_NULL_HANDLE; efb_memory_=VK_NULL_HANDLE; efb_initialized_=false; efb_extent_={};
        for (auto [key,pipeline]:pipelines_) vkDestroyPipeline(device,pipeline,nullptr);
        pipelines_.clear();
        if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
        if (texture_layout_) vkDestroyDescriptorSetLayout(device,texture_layout_,nullptr);
        if (texture_sampler_) vkDestroySampler(device,texture_sampler_,nullptr);
        texture_layout_=VK_NULL_HANDLE; texture_sampler_=VK_NULL_HANDLE;
        for (auto& slot : slots_) {
            if (slot.fence) vkDestroyFence(device, slot.fence, nullptr);
            if (slot.acquired) vkDestroySemaphore(device, slot.acquired, nullptr);
            slot = {};
        }
        if (pool_) vkDestroyCommandPool(device, pool_, nullptr);
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
