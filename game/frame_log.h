#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uc {
// One row per presented frame: how long it took and where the time went. Written by a background thread every few seconds (and at exit) so
// that logging adds no hitch of its own. Enabled by UC_FRAME_LOG=<file>, or by an empty file `frame_log.on` next to uc.exe (debug/frames-<pid>.csv).
struct FrameRow {
    double t_ms = 0, interval_ms = 0;   // since start; since the previous present
    double busy_ms = 0;                 // time spent in render() for the packets of this frame
    double acquire_ms = 0, submit_ms = 0, present_ms = 0;   // inside render(): vkAcquireNextImage, vkQueueSubmit, vkQueuePresent
    double events_ms = 0, cpu_mcycles = 0;   // SDL event handling since the previous present; CPU cycles the main thread ran since then (QueryThreadCycleTime)
    double idle_ms = 0, thread_ms[8] = {};   // the scheduler waiting with every guest thread blocked; baton time of guest threads 1..8 since the previous present
    double gpu_upscale_ms = 0, gpu_smaa_ms = 0, gpu_occlusion_ms = 0;   // latest GPU time of each effect pass (renderer pass timestamps), 0 while the pass does not run
    double gpu_rt_shadows_ms = 0;   // ray-traced shadows (trace, denoise, composite) of the frame's boundary packet
    double gpu_rt_reflections_ms = 0;   // ray-traced reflections likewise
    double gpu_rt_lighting_ms = 0;      // ray-traced lighting likewise
    double gpu_rt_as_ms = 0;   // ray tracing: acceleration structures summed over the frame's packets
    uint32_t rt_blas = 0; uint64_t rt_triangles = 0;   // the frame's BLASes (one per scene packet with instances) and their triangles
    double gpu_ms = 0;   // longest GPU execution of one submission retired since the previous present (timestamp queries)
    double sealed_to_done_ms = 0, taken_to_done_ms = 0;   // worst presenting packet completed since the previous present: sealed by the guest -> GPU done, and taken by the renderer -> GPU done
    uint32_t events = 0, queued = 0, outstanding = 0;
    uint64_t retraces = 0, textures = 0, decodes = 0, dvd_reads = 0, audio_blocks = 0, seal_wait_ns = 0, switches = 0;
};
class FrameLog {
public:
    explicit FrameLog(const std::filesystem::path& path);
    ~FrameLog();
    void add(const FrameRow& row);
    void note(const std::string& text) { std::lock_guard lock(mutex_); notes_ += text; }   // appended as comment lines when the log is closed
private:
    void run();
    void flush(std::vector<FrameRow>& rows);
    std::FILE* file_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<FrameRow> rows_;
    std::string notes_;
    bool stop_ = false;
    std::thread writer_;
};
}
