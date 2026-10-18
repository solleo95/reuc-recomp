#pragma once
#include "upscaler.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace uc {
class Streamline;
// DLSS Frame Generation through Streamline (docs/ProgrammingGuideDLSS_G.md). Streamline generates the extra frames inside its vkQueuePresentKHR
// proxy from the depth and vectors DLSS SR tagged for the frame plus the HUD-less colour tagged here; Reflex runs in low-latency mode.
class DlssFrameGeneration {
public:
    DlssFrameGeneration(const Streamline& streamline, uint32_t multiplier);   // presented frames per rendered frame, clamped to what the GPU allows
    ~DlssFrameGeneration();
    DlssFrameGeneration(const DlssFrameGeneration&)=delete;
    DlssFrameGeneration& operator=(const DlssFrameGeneration&)=delete;
    uint32_t multiplier() const { return multiplier_; }
    // Per guest frame, before its present: on for frames with a 3D scene, off for menus, movies and black frames (no smeared frames there).
    void set_active(bool active, bool reset);
    // The scene before the HUD at back-buffer size; VK_IMAGE_LAYOUT_GENERAL until the present.
    void tag_hudless(VkCommandBuffer command, const UpscaleImage& hudless);
    // After the present: the fence the next frame waits on before it overwrites the tagged depth and vectors (guide §15.1).
    void after_present();
    VkSemaphore inputs_fence() const { return fence_; }
    uint64_t inputs_fence_value() const { return fence_value_; }
private:
    const Streamline& streamline_;
    uint32_t multiplier_=2;
    bool active_=false, configured_=false;
    VkSemaphore fence_=VK_NULL_HANDLE;
    uint64_t fence_value_=0;
    uint64_t presented_=0, rendered_=0, reported_presented_=0;
    uint32_t last_status_=0;
};
}
