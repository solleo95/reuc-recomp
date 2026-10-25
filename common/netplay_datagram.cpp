#include "netplay_datagram.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <stdexcept>

namespace uc::net {
namespace {
constexpr std::array<uint8_t, 4> header{'U', 'C', 'D', 1};
constexpr size_t payload_offset = header.size() + 16;
struct Winsock {
    Winsock() { WSADATA data{}; if (const int error = WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Cannot initialize networking (Winsock " + std::to_string(error) + ")"); }
    ~Winsock() { WSACleanup(); }
};
struct Handle {
    SOCKET value = INVALID_SOCKET;
    ~Handle() { if (value != INVALID_SOCKET) closesocket(value); }
};
[[noreturn]] void socket_error(const char* operation, int error = WSAGetLastError()) {
    throw std::runtime_error(std::string(operation) + " (Winsock " + std::to_string(error) + ")");
}
void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Network operation cancelled");
}
bool same_peer(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
}
struct Datagram::Impl {
    Winsock winsock;
    Handle socket;
    std::array<uint8_t, 16> session;
    std::stop_token stop;
    sockaddr_in peer{};
    bool peer_known = false;
    Impl(std::array<uint8_t, 16> token, std::stop_token cancel) : session(token), stop(cancel) {}
};
Datagram::Datagram(const Endpoint& endpoint, std::array<uint8_t, 16> session, std::stop_token stop)
    : impl_(std::make_unique<Impl>(session, stop)) {
    check_stop(stop);
    if (!valid_address(endpoint.address)) throw std::runtime_error("Enter a numeric IPv4 address");
    if (!endpoint.port) throw std::runtime_error("Network port must be nonzero");
    impl_->socket.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket.value == INVALID_SOCKET) socket_error("Cannot create datagram socket");
    const BOOL exclusive = TRUE;
    if (setsockopt(impl_->socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
        socket_error("Cannot reserve datagram port");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(endpoint.loopback ? INADDR_LOOPBACK : INADDR_ANY);
    local.sin_port = endpoint.host ? htons(endpoint.port) : 0;
    if (bind(impl_->socket.value, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        socket_error("Cannot bind datagram port");
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket.value, FIONBIO, &nonblocking) == SOCKET_ERROR)
        socket_error("Cannot configure nonblocking socket");
    if (!endpoint.host) {
        impl_->peer.sin_family = AF_INET;
        impl_->peer.sin_port = htons(endpoint.port);
        InetPtonA(AF_INET, endpoint.address.c_str(), &impl_->peer.sin_addr);
        impl_->peer_known = true;
    }
    check_stop(stop);
}
Datagram::~Datagram() = default;
void Datagram::send(std::span<const uint8_t> payload) {
    check_stop(impl_->stop);
    if (payload.empty() || payload.size() > max_datagram_payload)
        throw std::runtime_error("Datagram length must be between 1 and 1152 bytes");
    if (!impl_->peer_known) return;
    std::array<uint8_t, payload_offset + max_datagram_payload> wire{};
    std::copy(header.begin(), header.end(), wire.begin());
    std::copy(impl_->session.begin(), impl_->session.end(), wire.begin() + header.size());
    std::copy(payload.begin(), payload.end(), wire.begin() + payload_offset);
    const int size = static_cast<int>(payload_offset + payload.size());
    const int sent = sendto(impl_->socket.value, reinterpret_cast<const char*>(wire.data()), size, 0,
                            reinterpret_cast<const sockaddr*>(&impl_->peer), sizeof(impl_->peer));
    if (sent == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAENOBUFS) return;
        socket_error("Cannot send datagram", error);
    }
    if (sent != size) throw std::runtime_error("Incomplete datagram send");
}
std::optional<std::vector<uint8_t>> Datagram::take() {
    for (int attempt = 0; attempt < 64; ++attempt) {
        check_stop(impl_->stop);
        std::array<uint8_t, payload_offset + max_datagram_payload> wire{};
        sockaddr_in source{};
        int source_size = sizeof(source);
        const int received = recvfrom(impl_->socket.value, reinterpret_cast<char*>(wire.data()), static_cast<int>(wire.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) return std::nullopt;
            if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
            socket_error("Cannot receive datagram", error);
        }
        if (received <= static_cast<int>(payload_offset) || source_size != sizeof(source) || source.sin_family != AF_INET ||
            !std::equal(header.begin(), header.end(), wire.begin()) ||
            !std::equal(impl_->session.begin(), impl_->session.end(), wire.begin() + header.size()) ||
            (impl_->peer_known && !same_peer(impl_->peer, source))) continue;
        if (!impl_->peer_known) { impl_->peer = source; impl_->peer_known = true; }
        return std::vector<uint8_t>(wire.begin() + payload_offset, wire.begin() + received);
    }
    return std::nullopt;
}
}
