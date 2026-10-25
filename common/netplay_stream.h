#pragma once

#include "netplay.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace uc::net {

class Stream {
public:
    // Socket and shared stop source must outlive the stream.
    Stream(Socket& socket, std::stop_source& stop, std::vector<uint8_t> heartbeat = {});
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    void enqueue(std::vector<uint8_t> packet);
    std::optional<std::vector<uint8_t>> take();
    // Blocking receive for startup and accelerated harness use only.
    std::vector<uint8_t> wait();
    std::string error() const;

private:
    void send_loop();
    void receive_loop();
    void fail(std::string message);
    void check_error() const;

    Socket& socket_;
    std::stop_source& stop_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::vector<uint8_t>> outgoing_, incoming_;
    std::string error_;
    const std::vector<uint8_t> heartbeat_;
    std::thread sender_, receiver_;
};

} // namespace uc::net
