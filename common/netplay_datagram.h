#pragma once
#include "netplay.h"
#include <array>

namespace uc::net {
constexpr size_t max_datagram_payload = 1152;
// Call send and take from one thread; stop tokens cancel either operation.
class Datagram {
public:
    Datagram(const Endpoint&, std::array<uint8_t, 16> session, std::stop_token = {});
    ~Datagram();
    Datagram(const Datagram&) = delete;
    Datagram& operator=(const Datagram&) = delete;
    void send(std::span<const uint8_t>);
    std::optional<std::vector<uint8_t>> take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
