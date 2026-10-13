#include "gx_packets.h"
#include <limits>

namespace gx {
uint64_t Packets::seal_batch(FramePacket packet) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [&] { return canceled_ || outstanding_.size() < 2; });
    if (canceled_) throw Error(ErrorCode::canceled, 0, 0, 0);
    if (next_serial_ == std::numeric_limits<uint64_t>::max()) throw Error(ErrorCode::sequence, 0, 0, 0);
    uint64_t serial = next_serial_;
    packet.serial = serial;
    auto owned = std::make_unique<const FramePacket>(std::move(packet));
    outstanding_.emplace(serial, false);
    try { queued_.push_back(std::move(owned)); }
    catch (...) { outstanding_.erase(serial); throw; }
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
