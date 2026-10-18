#pragma once
#include "present_functions.h"
#include "upscaler.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace uc {
// FSR 3.1 frame generation through the FFX API: the frame interpolation swapchain replaces the game's swapchain (its functions come back
// through present_functions()), a frame generation context turns the depth and vectors of each scene frame plus the HUD-less colour into
// the extra frame, and the swapchain's own threads pace the presentation of both.
class FsrFrameGeneration {
public:
    struct Queues { VkQueue game=VK_NULL_HANDLE, present=VK_NULL_HANDLE, acquire=VK_NULL_HANDLE; uint32_t family=0; };   // three distinct queues of one family
    FsrFrameGeneration(VkDevice device, VkPhysicalDevice physical, const Queues& queues, const std::filesystem::path& directory);   // throws when the DLL is unusable
    ~FsrFrameGeneration();
    FsrFrameGeneration(const FsrFrameGeneration&)=delete;
    FsrFrameGeneration& operator=(const FsrFrameGeneration&)=delete;
    PresentFunctions present_functions();
    // The frame generation context for a swapchain of `display` pixels in `backbuffer` format and a scene of up to `max_render` pixels.
    void create_context(VkExtent2D display, VkExtent2D max_render, VkFormat backbuffer);
    void release_context();   // before the swapchain goes: waits for the queued presentations
    bool has_context() const;
    // Per guest frame, before its present: on for frames with a 3D scene (with the frame's HUD-less colour), off for menus, movies and black frames.
    void begin_frame(bool active, const UpscaleImage& hudless, uint64_t frame);
    bool active() const;
    // After the upscale of a scene frame: the depth and vectors the interpolation reads; `reset` keeps it from blending across a camera cut.
    void prepare(const UpscaleInputs& inputs, uint64_t frame);
    void after_present();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
