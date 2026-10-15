#include "gx_packets.h"
#include <atomic>
#include <chrono>
#include <limits>

namespace gx {
namespace { std::atomic<uint64_t> total_seal_wait_ns{0}, total_window_wait_ns{0}, total_seals{0}, latency_frames{0}, latency_total_ns{0}, latency_max_ns{0}; }
uint64_t seal_wait_ns() { return total_seal_wait_ns.load(); }
uint64_t seal_window_wait_ns() { return total_window_wait_ns.load(); }
uint64_t seal_count() { return total_seals.load(); }
FrameLatency frame_latency() { return {latency_frames.load(), latency_total_ns.load(), latency_max_ns.load()}; }
uint64_t Packets::seal_batch(FramePacket packet) {
    std::unique_lock lock(mutex_);
    const auto wait_start = std::chrono::steady_clock::now();
    space_.wait(lock, [&] { return canceled_ || outstanding_.size() < kMaxOutstandingPackets; });
    const auto window_done = std::chrono::steady_clock::now();
    total_window_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(window_done - wait_start).count();
    space_.wait(lock, [&] { return canceled_ || !packet.present || frames_in_flight_.size() < kMaxFramesInFlight; });
    ++total_seals;
    total_seal_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
    if (canceled_) throw Error(ErrorCode::canceled, 0, 0, 0);
    if (next_serial_ == std::numeric_limits<uint64_t>::max()) throw Error(ErrorCode::sequence, 0, 0, 0);
    uint64_t serial = next_serial_;
    packet.serial = serial;
    const bool presenting = packet.present;
    auto owned = std::make_unique<const FramePacket>(std::move(packet));
    outstanding_.emplace(serial, false);
    if (presenting) frames_in_flight_.emplace(serial, std::chrono::steady_clock::now());
    try { queued_.push_back(std::move(owned)); }
    catch (...) { outstanding_.erase(serial); frames_in_flight_.erase(serial); throw; }
    ++next_serial_;
    ready_.notify_one();
    return serial;
}
std::unique_ptr<const FramePacket> Packets::try_take() {
    std::lock_guard lock(mutex_);
    if (queued_.empty()) return {};
    outstanding_.at(queued_.front()->serial) = true;
    auto packet = std::move(queued_.front());
    queued_.pop_front();
    return packet;
}
bool Packets::complete(uint64_t serial) {
    std::lock_guard lock(mutex_);
    auto found = outstanding_.find(serial);
    if (found == outstanding_.end() || !found->second) return false;
    outstanding_.erase(found);
    if (auto frame = frames_in_flight_.find(serial); frame != frames_in_flight_.end()) {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - frame->second).count());
        ++latency_frames; latency_total_ns += ns;
        if (ns > latency_max_ns) latency_max_ns = ns;
        frames_in_flight_.erase(frame);
    }
    space_.notify_all();
    return true;
}
bool Packets::wait_ready(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    ready_.wait_for(lock,timeout,[&]{return canceled_ || !queued_.empty();});
    return !canceled_ && !queued_.empty();
}
void Packets::cancel() {
    std::lock_guard lock(mutex_);
    canceled_ = true;
    queued_.clear();
    outstanding_.clear();
    space_.notify_all();
    ready_.notify_all();
}
} // namespace gx
