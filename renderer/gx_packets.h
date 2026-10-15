#pragma once
#include "block_pool.h"
#include "gx_vertices.h"
#include <condition_variable>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace gx {
struct TextureCopy {
    std::array<uint32_t,4> source; // x,y,width,height in the SDK's640x480 EFB coordinates
    std::shared_ptr<const TexturePixels> target;
};
struct FramePacket {
    uint64_t serial = 0;
    std::deque<DrawPacket, PoolAllocator<DrawPacket>> draws;   // a DrawPacket is ~6 KB: a deque never relocates them while a batch grows
    bool present = true;
    bool clear_after_copy = true;
    std::array<uint8_t,4> clear_color{16,32,64,255};
    uint32_t clear_depth = 0xFFFFFF, xfb = 0;
    std::optional<TextureCopy> texture_copy;
    std::array<uint32_t,4> display_source{0,0,640,480};
    std::vector<PromptDraw> prompts;
    bool black_output = false; // VI blanked: present black, the EFB copy still happens
};

// A game frame is ~23 packets in the train (one per EFB copy) and the renderer drains them in bursts: the packet window must cover
// a few frames (64 stalled the guest by ~4 ms per frame, 256 does not), while kMaxFramesInFlight bounds the added input latency.
constexpr size_t kMaxOutstandingPackets = 256, kMaxFramesInFlight = 2;
uint64_t seal_wait_ns(); // total time producers waited for a free packet slot, i.e. for the renderer
uint64_t seal_window_wait_ns(); // part of seal_wait_ns spent waiting for the packet window (the rest: frames in flight)
uint64_t seal_count();
struct FrameLatency { uint64_t frames, total_ns, max_ns; };   // sealed -> GPU complete, presenting packets only
FrameLatency frame_latency();
class Packets {
public:
    uint64_t seal_batch(FramePacket packet);
    std::unique_ptr<const FramePacket> try_take();
    bool wait_ready(std::chrono::nanoseconds timeout);
    bool complete(uint64_t serial);
    void cancel();
    struct Depth { size_t queued, outstanding, frames_in_flight; };
    Depth depth() { std::lock_guard lock(mutex_); return {queued_.size(), outstanding_.size(), frames_in_flight_.size()}; }
private:
    std::mutex mutex_;
    std::condition_variable space_;
    std::condition_variable ready_;
    std::deque<std::unique_ptr<const FramePacket>> queued_;
    std::map<uint64_t, bool> outstanding_;
    std::map<uint64_t, std::chrono::steady_clock::time_point> frames_in_flight_;   // unfinished presenting packets and when they were sealed
    uint64_t next_serial_ = 1;
    bool canceled_ = false;
};
} // namespace gx
