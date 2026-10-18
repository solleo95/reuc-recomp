#pragma once
#include "gx_packets.h"
#include "pipeline_keys.h"
#include "prompt_overlay.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h> // Load Clang's intrinsic guards before SDL 3.2.0's compatibility definitions.
#endif
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

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
    // Freshest pointer in aim units: reticle draws are re-aimed to it when their commands are recorded (late_aim.h).
    void set_aim_source(std::function<std::array<float,2>()> source) { aim_source_ = std::move(source); }
    uint64_t late_aim_draws() const { return late_aim_draws_.load(); }
    struct FrameTimings { double acquire_ms = 0, submit_ms = 0, present_ms = 0; };   // inside the last render(): the calls a layer or the driver can stall
    FrameTimings last_timings() const { return timings_; }
    void resize(uint32_t width, uint32_t height);
    // Compiles every known pipeline (seed and recorded keys) before the game starts; progress(done,total) is called on this thread.
    void warm_pipelines(const std::function<void(size_t,size_t)>& progress);
    void set_uber_only(bool enabled) { owner(); uber_only_=enabled; }   // tests: draw everything with the ubershader
    void set_render_scale(double scale); // Multiplier of720p; output resize preserves it.
    void set_prompt_style(PromptStyle);
    void set_bloom(bool);   // false skips the bloom composite draw (glow around lights)
    std::vector<uint64_t> poll_completions();
    bool wait_for_pending(uint64_t timeout_ns); // True when an unfinished batch was waited on, including timeout.
    RenderPixels capture(uint64_t serial, bool efb = false);
    void shutdown();
    uint32_t validation_errors() const { return errors_.load(); }
    uint64_t presentation_count() const { return presentations_.load(); }
    double take_gpu_window_ms() { return gpu_window_max_ns_.exchange(0) / 1e6; }   // longest single GPU submission retired since the last call (frame log)
    uint64_t gpu_busy_ns() const { return gpu_busy_ns_.load(); }   // summed GPU execution time of retired frame submissions
    uint64_t texture_creations() const { owner(); return texture_creations_; }
    uint64_t descriptor_pool_creations() const { owner(); return descriptor_pool_creations_; }
    VkDevice device = VK_NULL_HANDLE;
private:
    FrameTimings timings_;
    std::function<std::array<float,2>()> aim_source_;
    std::atomic<uint64_t> late_aim_draws_{0};
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };
    struct Texture {
        VkImage image=VK_NULL_HANDLE;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        Buffer upload;
        VkExtent2D extent{};
        uint32_t mip_levels=1;
        gx::TextureChannels channels=gx::TextureChannels::rgba;
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
        Buffer vertices, indices, pixels, uniforms, palettes, banks;   // vertices/indices: this frame's non-mesh draws
        std::vector<std::shared_ptr<const gx::MeshBuffers>> mesh_refs;   // keeps the meshes this submission draws alive until its fence completed
        std::vector<Texture> textures;
        std::vector<std::shared_ptr<const gx::TexturePixels>> copy_sources;
        std::vector<VkDescriptorSet> texture_sets;
        VkDescriptorPool texture_pool=VK_NULL_HANDLE;
        uint32_t descriptor_capacity=0;
        uint64_t serial = 0;
        bool submitted = false, reported = false, initialized = false, copied = false, timed = false;
    };
    // A mesh uploaded once and drawn from every frame; freed once the guest dropped it (owner expired) and no submission still holds it.
    // Meshes live in a few large buffers (one vkAllocateMemory per slab, not per mesh): vertices then indices at the given offsets.
    struct MeshSlab { Buffer buffer; VkDeviceSize used=0; uint32_t live=0; };
    struct GpuMesh { MeshSlab* slab=nullptr; VkDeviceSize vertex_offset=0, index_offset=0; uint32_t index_count=0; uint64_t rows=0; std::weak_ptr<const gx::MeshBuffers> owner; };
    struct PresentImage {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore ready = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
    };
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user);
    void owner() const;
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool prefer_device_local=false);
    GpuMesh& gpu_mesh(const std::shared_ptr<const gx::MeshBuffers>& source);
    void sweep_meshes(bool all);
    void release_mesh(GpuMesh& mesh);
    void destroy_buffer(Buffer& buffer);
    Texture make_texture(const gx::TexturePixels* pixels, VkExtent2D physical_extent = {});
    Texture make_copy_texture(const std::shared_ptr<const gx::TexturePixels>& source);
    void recycle_copy_texture(Texture& texture);
    void destroy_texture(Texture& texture);
    void retire_textures(Slot& slot);
    void rebuild();
    void retire_images();
    void retire_for_resize();
    void reap_retired(bool wait);
    // A pipeline for a draw: the specialized one when compiled, else the ubershader variant of the same fixed state (see queue_pipeline).
    struct PipelineChoice { VkPipeline pipeline=VK_NULL_HANDLE; bool uber=false; };
    PipelineKey pipeline_key(const gx::DrawPacket& draw,bool alpha_pass=false) const;
    PipelineChoice pipeline_for(const PipelineKey& key);
    VkPipeline make_pipeline(const PipelineKey& key,bool uber);
    void queue_pipeline(const PipelineKey& key,bool uber);   // pipelines_mutex_ held
    void load_pipeline_cache();
    void save_pipeline_cache();
    void start_workers();
    void stop_workers();
    void compile_worker();
    void make_attachment(VkImage& image,VkDeviceMemory& memory,VkImageView& view,VkExtent2D extent,bool depth=false);
    void collect();
    SDL_Window* window_ = nullptr;
    PromptOverlay prompt_overlay_;
    PromptStyle prompt_style_=PromptStyle::monochrome;
    bool bloom_=true;
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
    std::map<PipelineKey,VkPipeline> pipelines_, uber_pipelines_;   // guarded by pipelines_mutex_ with the members below: compile threads insert too
    std::mutex pipelines_mutex_;
    struct CompileJob { PipelineKey key; bool uber; };
    std::deque<CompileJob> compile_queue_;
    std::set<PipelineKey> queued_, uber_queued_;
    std::condition_variable compile_cv_;
    size_t compiling_ = 0, compiled_ = 0, compiled_ubers_ = 0, jobs_done_ = 0;
    double compiled_ms_ = 0, compiled_uber_ms_ = 0;   // compile time since the last idle report
    bool compile_stop_ = false;
    bool uber_only_ = false;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    size_t pipeline_cache_loaded_ = 0;
    VkShaderModule vertex_module_ = VK_NULL_HANDLE, fragment_module_ = VK_NULL_HANDLE, uber_module_ = VK_NULL_HANDLE;
    std::filesystem::path cache_dir_;
    std::set<PipelineKey> known_keys_;        // keys already in the seed or recorded file
    std::vector<PipelineKey> warm_keys_;
    std::vector<std::thread> compilers_;
    std::map<const gx::TexturePixels*,Texture> copy_textures_;
    std::unordered_map<const gx::MeshBuffers*,GpuMesh> meshes_;
    std::vector<std::unique_ptr<MeshSlab>> mesh_slabs_;   // the last one is the one being filled
    uint64_t renders_=0;
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
    static constexpr size_t kFrameSlots = 6;   // GPU submissions in flight; the guest's queue window is larger (gx::kMaxOutstandingPackets)
    std::array<Slot, kFrameSlots> slots_{};
    std::vector<PresentImage> images_;
    struct RetiredSwapchain { VkSwapchainKHR chain; std::vector<PresentImage> images; };
    std::vector<RetiredSwapchain> retired_;   // swapchains replaced by a resize, freed once their queued presentations finished
    VkSwapchainKHR old_swapchain_ = VK_NULL_HANDLE;   // the newest retired chain, passed as oldSwapchain to its successor
    std::vector<uint64_t> completed_;
    size_t next_slot_ = 0;
    uint64_t last_serial_ = 0;
    bool resized_ = true;
    std::chrono::steady_clock::time_point last_rebuild_{};
    std::atomic<uint32_t> errors_{0};
    std::atomic<uint64_t> presentations_{0}, gpu_busy_ns_{0}, gpu_window_max_ns_{0};
    VkQueryPool timing_pool_ = VK_NULL_HANDLE;   // two timestamps per slot
    double timestamp_period_ = 0;
    uint64_t texture_creations_=0;
    uint64_t descriptor_pool_creations_=0;
};
}
