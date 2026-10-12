#pragma once
#include "gx_packets.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h> // Load Clang's intrinsic guards before SDL 3.2.0's compatibility definitions.
#endif
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <filesystem>
#include <thread>

namespace uc {
struct RenderPixels { uint64_t serial = 0; uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };
void save_png(const RenderPixels& pixels,const std::filesystem::path& path);
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    void initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders);
    bool render(const gx::FramePacket& packet);
    void resize(uint32_t width, uint32_t height);
    std::vector<uint64_t> poll_completions();
    RenderPixels capture(uint64_t serial);
    void shutdown();
    uint32_t validation_errors() const { return errors_.load(); }
    uint64_t presentation_count() const { return presentations_.load(); }
    VkDevice device = VK_NULL_HANDLE;
private:
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };
    struct Texture {
        VkImage image=VK_NULL_HANDLE;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        Buffer upload;
        VkExtent2D extent{};
    };
    struct Slot {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        Buffer vertices, indices, pixels;
        std::vector<Texture> textures;
        std::vector<VkDescriptorSet> texture_sets;
        VkDescriptorPool texture_pool=VK_NULL_HANDLE;
        uint64_t serial = 0;
        bool submitted = false, reported = false, initialized = false, copied = false;
    };
    struct PresentImage {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore ready = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
    };
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user);
    void owner() const;
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& buffer);
    Texture make_texture(const gx::TexturePixels* pixels);
    void retire_textures(Slot& slot);
    void rebuild();
    void retire_images();
    void make_pipeline(const std::filesystem::path& shaders);
    void make_attachment(VkImage& image,VkDeviceMemory& memory,VkImageView& view,VkExtent2D extent);
    void collect();
    SDL_Window* window_ = nullptr;
    std::thread::id owner_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkExtent2D requested_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texture_layout_=VK_NULL_HANDLE;
    VkSampler texture_sampler_=VK_NULL_HANDLE;
    VkImage efb_ = VK_NULL_HANDLE;
    VkExtent2D efb_extent_{};
    VkDeviceMemory efb_memory_ = VK_NULL_HANDLE;
    VkImageView efb_view_ = VK_NULL_HANDLE;
    bool efb_initialized_ = false;
    std::array<Slot, 2> slots_{};
    std::vector<PresentImage> images_;
    std::vector<uint64_t> completed_;
    size_t next_slot_ = 0;
    uint64_t last_serial_ = 0;
    bool resized_ = true;
    std::atomic<uint32_t> errors_{0};
    std::atomic<uint64_t> presentations_{0};
};
}
