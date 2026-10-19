#include "frame_log.h"
#include "fatal.h"
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
FrameLog::FrameLog(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    file_ = _wfopen(path.c_str(), L"wb");
    if (!file_) fatal("cannot create the frame log %ls", path.c_str());
    std::fprintf(file_, "t_ms,interval_ms,busy_ms,acquire_ms,submit_ms,present_ms,idle_ms,t1_ms,t2_ms,t3_ms,t4_ms,t5_ms,t6_ms,t7_ms,t8_ms,events_ms,events,cpu_mcycles,retraces,textures,decodes,dvd_reads,audio_blocks,seal_wait_ns,switches,queued,outstanding,sealed_to_done_ms,taken_to_done_ms,gpu_ms,gpu_upscale_ms,gpu_smaa_ms,gpu_occlusion_ms,gpu_rt_as_ms,rt_blas,rt_triangles,gpu_rt_shadows_ms,gpu_rt_reflections_ms\n");
    rows_.reserve(4096);
    writer_ = std::thread([this] { run(); });
}
FrameLog::~FrameLog() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    wake_.notify_all();
    writer_.join();
    if (!notes_.empty()) std::fputs(notes_.c_str(), file_);
    std::fclose(file_);
}
void FrameLog::add(const FrameRow& row) {
    std::lock_guard lock(mutex_);
    rows_.push_back(row);
}
void FrameLog::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<FrameRow> rows; rows.reserve(4096);
    for (;;) {
        bool stopping;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_; });
            stopping = stop_;
            rows.swap(rows_);
        }
        flush(rows);
        if (stopping) return;
    }
}
void FrameLog::flush(std::vector<FrameRow>& rows) {
    for (const auto& r : rows)
        std::fprintf(file_, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%llu,%.3f,%.3f\n", r.t_ms, r.interval_ms, r.busy_ms, r.acquire_ms, r.submit_ms, r.present_ms, r.idle_ms, r.thread_ms[0], r.thread_ms[1], r.thread_ms[2], r.thread_ms[3], r.thread_ms[4], r.thread_ms[5], r.thread_ms[6], r.thread_ms[7], r.events_ms, r.events, r.cpu_mcycles,
                     (unsigned long long)r.retraces, (unsigned long long)r.textures, (unsigned long long)r.decodes, (unsigned long long)r.dvd_reads, (unsigned long long)r.audio_blocks, (unsigned long long)r.seal_wait_ns,
                     (unsigned long long)r.switches, r.queued, r.outstanding, r.sealed_to_done_ms, r.taken_to_done_ms, r.gpu_ms, r.gpu_upscale_ms, r.gpu_smaa_ms, r.gpu_occlusion_ms, r.gpu_rt_as_ms, r.rt_blas, (unsigned long long)r.rt_triangles, r.gpu_rt_shadows_ms, r.gpu_rt_reflections_ms);
    rows.clear();
    std::fflush(file_);
}
}
