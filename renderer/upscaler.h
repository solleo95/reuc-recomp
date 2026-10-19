#pragma once
#include "gx_transforms.h"
#include "render_scale.h"
#include "upscale_mode.h"
#include <vulkan/vulkan.h>
#include <array>
#include <filesystem>
#include <memory>

namespace uc {
struct UpscaleImage { VkImage image=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkFormat format=VK_FORMAT_UNDEFINED; VkExtent2D extent{}; VkImageUsageFlags usage=0; };
// One reconstruction. Every image is in VK_IMAGE_LAYOUT_GENERAL on entry and must be left there; the renderer puts barriers around the call.
struct UpscaleInputs {
    VkCommandBuffer command=VK_NULL_HANDLE;
    UpscaleImage color, depth, motion, output;   // color/depth/motion: the internal scene targets; output: the output-resolution EFB
    VkExtent2D render_size{}, output_size{};     // top-left sub-rectangles holding the scene (the 640x360 viewport of the 640x480 EFB)
    std::array<float,2> jitter{};                // internal pixels, as added to the projection
    bool reset=false;                            // history is invalid: camera cut, scene-mode change, first frame
    bool sharpening=false;
    float sharpness=0.5f;
    float frame_ms=16.7f;
    uint64_t frame=0;
    gx::Projection projection{}, clip_to_prev_clip{};   // the scene's projection and the camera reprojection (MotionTracker)
};
// Motion vectors in `motion` (rg) are previous minus current position in internal pixels, unjittered.
class Upscaler {
public:
    virtual ~Upscaler()=default;
    virtual void evaluate(const UpscaleInputs& inputs)=0;
    virtual bool sharpens() const=0;   // true: the upscaler applied the RCAS sharpening itself (FSR)
    virtual RenderExtent minimum_internal(RenderExtent output) const=0;
};
std::unique_ptr<Upscaler> make_blit_upscaler();   // test-only: linear blit of the scene into the output
class Streamline;
// DLSS 4.5 Super Resolution through Streamline; presets K (DLAA, Quality, Balanced), M (Performance), L (Ultra Performance).
std::unique_ptr<Upscaler> make_dlss_upscaler(const Streamline& streamline, UpscaleMode mode);
// FSR 3.1 through the FFX API (amd_fidelityfx_vk.dll in `directory`); it sharpens with its own RCAS when `sharpening` is on. Throws when the DLL is unusable.
std::unique_ptr<Upscaler> make_fsr_upscaler(VkDevice device, VkPhysicalDevice physical, const std::filesystem::path& directory);
}
