#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
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
    uint32_t events = 0, queued = 0, outstanding = 0;
    uint64_t retraces = 0, textures = 0, decodes = 0, dvd_reads = 0, audio_blocks = 0, seal_wait_ns = 0, switches = 0;
};
class FrameLog {
public:
    explicit FrameLog(const std::filesystem::path& path);
    ~FrameLog();
    void add(const FrameRow& row);
private:
    void run();
    void flush(std::vector<FrameRow>& rows);
    std::FILE* file_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<FrameRow> rows_;
    bool stop_ = false;
    std::thread writer_;
};
}
