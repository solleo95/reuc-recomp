#pragma once
#include "block_pool.h"
#include "frame_arena.h"
#include "gx_vertices.h"
#include <condition_variable>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace gx {
struct SceneFrame;
struct SceneSnapshot;
class SceneCapture;
using DrawQueue=std::deque<DrawPacket,FrameAllocator<DrawPacket>>;
struct TextureCopy {
    std::array<uint32_t,4> source; // x,y,width,height in the SDK's640x480 EFB coordinates
    std::shared_ptr<const TexturePixels> target;
};
struct FramePacket {
    uint64_t serial = 0;
    DrawQueue draws;   // stable draw addresses; arena storage retires with the frame
    bool present = true;
    bool clear_after_copy = true;
    std::array<uint8_t,4> clear_color{16,32,64,255};
    uint32_t clear_depth = 0xFFFFFF, xfb = 0;
    std::optional<TextureCopy> texture_copy;
    std::array<uint32_t,4> display_source{0,0,640,480};
    std::vector<PromptDraw> prompts;
    bool black_output = false; // VI blanked: present black, the EFB copy still happens
    std::array<float,2> aim_consumed{};   // pointer (aim units) the game read when it drew this batch; the reference of the late-aim shift
    bool replay = false; // Preserve GX work and frame boundaries without displaying intermediate rollback frames.
    bool clear_only = false;
    std::shared_ptr<const SceneFrame> scene;
};

// A game frame is ~23 packets in the train (one per EFB copy) and the renderer drains them in bursts: the packet window must cover
// a few frames (64 stalled the guest by ~4 ms per frame, 256 does not), while kMaxFramesInFlight bounds the added input latency.
constexpr size_t kMaxOutstandingPackets = 256, kMaxFramesInFlight = 2;
uint64_t seal_wait_ns(); // total time producers waited for a free packet slot, i.e. for the renderer
uint64_t seal_window_wait_ns(); // part of seal_wait_ns spent waiting for the packet window (the rest: frames in flight)
uint64_t seal_count();
struct FrameLatency { uint64_t frames, total_ns, max_ns; };   // sealed -> GPU complete, presenting packets only
FrameLatency frame_latency();
struct LatencyWindow { double sealed_to_done_ms, taken_to_done_ms; };   // worst presenting packet completed since the last call (frame log): in the queue + rendered, and rendered only
LatencyWindow take_latency_window();
class Packets {
public:
    explicit Packets(bool detached=false);
    ~Packets();
    bool detached() const { return bool(scene_capture_); }
    std::shared_ptr<const SceneSnapshot> checkpoint();
    void restore(const SceneSnapshot&);
    uint64_t seal_batch(FramePacket packet);
    std::unique_ptr<const FramePacket> try_take();
    bool wait_ready(std::chrono::nanoseconds timeout);
    bool complete(uint64_t serial);
    void cancel();
    struct Depth { size_t queued, outstanding, frames_in_flight; };
    Depth depth() { std::lock_guard lock(mutex_); return {queued_.size()+size_t(bool(latest_scene_)), outstanding_.size(), frames_in_flight_.size()}; }
private:
    std::mutex mutex_;
    std::condition_variable space_;
    std::condition_variable ready_;
    std::deque<std::unique_ptr<const FramePacket>> queued_;
    std::map<uint64_t, bool> outstanding_;
    std::map<uint64_t, std::chrono::steady_clock::time_point> frames_in_flight_;   // unfinished presenting packets and when they were sealed
    std::map<uint64_t, std::chrono::steady_clock::time_point> taken_at_;           // ... and when the renderer took them
    uint64_t next_serial_ = 1;
    bool canceled_ = false;
    std::unique_ptr<SceneCapture> scene_capture_;
    std::shared_ptr<const SceneFrame> latest_scene_;
};
} // namespace gx
