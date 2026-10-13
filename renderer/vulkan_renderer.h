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
    void initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders, double render_scale = 0.5, double render_aspect = 16.0/9);
    bool render(const gx::FramePacket& packet);
    void resize(uint32_t width, uint32_t height);
    void set_render_scale(double scale); // Multiplier of720p; output resize preserves it.
    std::vector<uint64_t> poll_completions();
    bool wait_for_pending(uint64_t timeout_ns); // True when an unfinished batch was waited on, including timeout.
    RenderPixels capture(uint64_t serial, bool efb = false);
    void shutdown();
    uint32_t validation_errors() const { return errors_.load(); }
    uint64_t presentation_count() const { return presentations_.load(); }
    uint64_t texture_creations() const { owner(); return texture_creations_; }
    uint64_t descriptor_pool_creations() const { owner(); return descriptor_pool_creations_; }
    VkDevice device = VK_NULL_HANDLE;
private:
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };
    struct Texture {
        VkImage image=VK_NULL_HANDLE;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        Buffer upload;
        VkExtent2D extent{};
        uint32_t mip_levels=1;
        VkDeviceSize allocation_size=0;
        std::shared_ptr<const gx::TexturePixels> source;
        bool initialized=false;
    };
    struct Slot {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        Buffer vertices, indices, pixels, uniforms;
        std::vector<Texture> textures;
        std::vector<std::shared_ptr<const gx::TexturePixels>> copy_sources;
        std::vector<VkDescriptorSet> texture_sets;
        VkDescriptorPool texture_pool=VK_NULL_HANDLE;
        uint32_t descriptor_capacity=0;
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
    Texture make_texture(const gx::TexturePixels* pixels, VkExtent2D physical_extent = {});
    Texture make_copy_texture(const std::shared_ptr<const gx::TexturePixels>& source);
    void recycle_copy_texture(Texture& texture);
    void destroy_texture(Texture& texture);
    void retire_textures(Slot& slot);
    void rebuild();
    void retire_images();
    using PipelineKey=std::array<uint32_t,69>;
    VkPipeline pipeline_for(const gx::DrawPacket& draw,bool alpha_pass=false);
    VkPipeline make_pipeline(const gx::BlendState& blend,const PipelineKey& key);
    void make_attachment(VkImage& image,VkDeviceMemory& memory,VkImageView& view,VkExtent2D extent,bool depth=false);
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
    double configured_scale_=0.5, render_aspect_=16.0/9;
    uint32_t max_image_dimension_=0;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::map<PipelineKey,VkPipeline> pipelines_;
    std::map<const gx::TexturePixels*,Texture> copy_textures_;
    std::vector<Texture> recycled_copies_;
    VkDeviceSize recycled_copy_bytes_=0;
    size_t uniform_stride_=sizeof(gx::DrawUniform);
    std::filesystem::path shaders_;
    bool logic_ops_=false;
    VkDescriptorSetLayout texture_layout_=VK_NULL_HANDLE;
    std::array<VkSampler,256> texture_samplers_{};
    VkImage efb_ = VK_NULL_HANDLE;
    VkExtent2D efb_extent_{};
    VkDeviceMemory efb_memory_ = VK_NULL_HANDLE;
    VkImageView efb_view_ = VK_NULL_HANDLE;
    bool efb_initialized_ = false;
    VkImage depth_=VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_=VK_NULL_HANDLE;
    VkImageView depth_view_=VK_NULL_HANDLE;
    std::array<Slot, 2> slots_{};
    std::vector<PresentImage> images_;
    std::vector<uint64_t> completed_;
    size_t next_slot_ = 0;
    uint64_t last_serial_ = 0;
    bool resized_ = true;
    std::atomic<uint32_t> errors_{0};
    std::atomic<uint64_t> presentations_{0};
    uint64_t texture_creations_=0;
    uint64_t descriptor_pool_creations_=0;
};
}
