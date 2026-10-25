#include "netplay_stream.h"

#include <stdexcept>
#include <utility>

namespace uc::net {
namespace {
constexpr size_t queue_limit = 128;
}

Stream::Stream(Socket& socket, std::stop_source& stop, std::vector<uint8_t> heartbeat)
    : socket_(socket), stop_(stop), heartbeat_(std::move(heartbeat)) {
    if (heartbeat_.size() > max_packet) throw std::invalid_argument("netplay heartbeat is too large");
    try {
        sender_ = std::thread([this] { send_loop(); });
        receiver_ = std::thread([this] { receive_loop(); });
    } catch (...) {
        stop_.request_stop();
        cv_.notify_all();
        if (sender_.joinable()) sender_.join();
        throw;
    }
}

Stream::~Stream() {
    stop_.request_stop();
    cv_.notify_all();
    if (sender_.joinable()) sender_.join();
    if (receiver_.joinable()) receiver_.join();
}

void Stream::check_error() const {
    if (!error_.empty()) throw std::runtime_error(error_);
}

void Stream::fail(std::string message) {
    {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = std::move(message);
    }
    stop_.request_stop();
    cv_.notify_all();
}

void Stream::enqueue(std::vector<uint8_t> packet) {
    std::unique_lock lock(mutex_);
    check_error();
    if (stop_.stop_requested()) throw std::runtime_error("netplay stream stopped");
    if (packet.empty() || packet.size() > max_packet || outgoing_.size() >= queue_limit) {
        error_ = "netplay stream outbound packet invalid or queue full";
        const auto message = error_;
        lock.unlock();
        stop_.request_stop();
        cv_.notify_all();
        throw std::runtime_error(message);
    }
    outgoing_.push_back(std::move(packet));
    lock.unlock();
    cv_.notify_all();
}

std::optional<std::vector<uint8_t>> Stream::take() {
    std::lock_guard lock(mutex_);
    check_error();
    if (incoming_.empty()) return std::nullopt;
    auto packet = std::move(incoming_.front());
    incoming_.pop_front();
    return packet;
}

std::vector<uint8_t> Stream::wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, stop_.get_token(), [this] { return !error_.empty() || !incoming_.empty(); });
    check_error();
    if (stop_.stop_requested()) throw std::runtime_error("netplay stream stopped");
    auto packet = std::move(incoming_.front());
    incoming_.pop_front();
    return packet;
}

std::string Stream::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void Stream::send_loop() {
    try {
        while (!stop_.stop_requested()) {
            std::unique_lock lock(mutex_);
            if (heartbeat_.empty()) {
                if (!cv_.wait(lock, stop_.get_token(), [this] { return !outgoing_.empty(); })) return;
            } else cv_.wait_for(lock, stop_.get_token(), std::chrono::seconds(1), [this] { return !outgoing_.empty(); });
            if (stop_.stop_requested()) return;
            auto packet = heartbeat_;
            if (!outgoing_.empty()) { packet = std::move(outgoing_.front()); outgoing_.pop_front(); }
            lock.unlock();
            socket_.send(packet);
        }
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("netplay stream sender failed");
    }
}

void Stream::receive_loop() {
    try {
        while (!stop_.stop_requested()) {
            auto packet = socket_.receive();
            if (!heartbeat_.empty() && packet == heartbeat_) continue;
            {
                std::lock_guard lock(mutex_);
                if (stop_.stop_requested()) return;
                if (packet.empty() || packet.size() > max_packet || incoming_.size() >= queue_limit)
                    throw std::runtime_error("netplay stream inbound packet invalid or queue full");
                incoming_.push_back(std::move(packet));
            }
            cv_.notify_all();
        }
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("netplay stream receiver failed");
    }
}

} // namespace uc::net
