#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <vector>

namespace uc {
// GPU passes of the graphics effects that get their own timing (frame log columns, --profile). Later effects append to the list.
enum class GpuPass : uint32_t { upscale, smaa, occlusion, shadow, motion_blur, rt_as, rt_probe, rt_denoise, count };   // rt_as: a packet's BLAS (and the TLAS in the boundary packet); rt_denoise: the own denoiser (rt_denoiser.h), every signal
constexpr size_t kGpuPassCount=size_t(GpuPass::count);
// Per-submission timestamps around passes: reset(slot) at the start of a slot's command buffer, begin()/end() around a pass, collect(slot) once
// that slot's fence has completed. last_ms() keeps the latest measurement of each pass (0 until it ran).
class PassTimer {
public:
    void initialize(VkDevice device, float timestamp_period_ns, uint32_t slots);
    void reset(VkCommandBuffer command, uint32_t slot);
    void begin(VkCommandBuffer command, uint32_t slot, GpuPass pass);
    void end(VkCommandBuffer command, uint32_t slot, GpuPass pass);
    void collect(uint32_t slot);
    double last_ms(GpuPass pass) const { return last_[size_t(pass)]; }
    uint64_t runs(GpuPass pass) const { return runs_[size_t(pass)]; }   // how often the pass was recorded (tests: a frame without a scene records none)
    double take_total_ms(GpuPass pass) { const double total=total_[size_t(pass)]; total_[size_t(pass)]=0; return total; }   // summed over the submissions collected since the last call (a pass that runs in several packets of a frame)
    void shutdown();
private:
    uint32_t query(uint32_t slot, GpuPass pass) const { return (slot*uint32_t(kGpuPassCount)+uint32_t(pass))*2; }
    VkDevice device_=VK_NULL_HANDLE;
    VkQueryPool pool_=VK_NULL_HANDLE;
    float period_=0;
    uint32_t slots_=0;
    std::vector<std::array<bool,kGpuPassCount>> recorded_;   // per slot: the pass wrote both timestamps in the current submission
    std::array<double,kGpuPassCount> last_{}, total_{};
    std::array<uint64_t,kGpuPassCount> runs_{};
};
}
