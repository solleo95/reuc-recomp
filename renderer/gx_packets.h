#pragma once
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
    std::vector<DrawPacket> draws;
    bool present = true;
    bool clear_after_copy = true;
    std::array<uint8_t,4> clear_color{16,32,64,255};
    uint32_t clear_depth = 0xFFFFFF, xfb = 0;
    std::optional<TextureCopy> texture_copy;
    std::array<uint32_t,4> display_source{0,0,640,480};
};

class Packets {
public:
    uint64_t seal_batch(FramePacket packet);
    std::unique_ptr<const FramePacket> try_take();
    bool wait_ready(std::chrono::nanoseconds timeout);
    bool complete(uint64_t serial);
    void cancel();
private:
    std::mutex mutex_;
    std::condition_variable space_;
    std::condition_variable ready_;
    std::deque<std::unique_ptr<const FramePacket>> queued_;
    std::map<uint64_t, bool> outstanding_;
    uint64_t next_serial_ = 1;
    bool canceled_ = false;
};
} // namespace gx
