#include "netplay.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uc::net {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Winsock {
    Winsock() { WSADATA data{}; if (const int error = WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Cannot initialize networking (Winsock " + std::to_string(error) + ")"); }
    ~Winsock() { WSACleanup(); }
};
void initialize() { static Winsock winsock; }
struct PortConflict : std::runtime_error { PortConflict():std::runtime_error("Listening port is already in use"){} };
[[noreturn]] void socket_error(const char* operation, int error = WSAGetLastError()) {
    throw std::runtime_error(std::string(operation) + " (Winsock " + std::to_string(error) + ")");
}
struct Handle {
    SOCKET value = INVALID_SOCKET;
    explicit Handle(SOCKET socket = INVALID_SOCKET) : value(socket) {}
    ~Handle() { if (value != INVALID_SOCKET) closesocket(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    SOCKET release() { return std::exchange(value, INVALID_SOCKET); }
};
void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Network operation cancelled");
}
struct Disconnected : std::runtime_error {
    bool clean;
    explicit Disconnected(bool at_boundary) : std::runtime_error(at_boundary ? "Peer disconnected" : "Peer disconnected during packet transfer"), clean(at_boundary) {}
};
void nonblocking(SOCKET socket) {
    u_long value = 1;
    if (ioctlsocket(socket, FIONBIO, &value) == SOCKET_ERROR) socket_error("Cannot configure nonblocking socket");
}
void wait_socket(SOCKET socket, bool writing, Clock::time_point deadline, std::stop_token stop) {
    for (;;) {
        check_stop(stop);
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) throw std::runtime_error("Network operation timed out");
        const auto delay = std::min(std::chrono::duration_cast<std::chrono::microseconds>(remaining), 25000us);
        timeval timeout{0, static_cast<long>(delay.count())};
        fd_set wanted, errors;
        FD_ZERO(&wanted); FD_SET(socket, &wanted);
        FD_ZERO(&errors); FD_SET(socket, &errors);
        const int result = select(0, writing ? nullptr : &wanted, writing ? &wanted : nullptr, &errors, &timeout);
        if (result == SOCKET_ERROR) socket_error("Cannot wait for network socket");
        if (result > 0) { check_stop(stop); return; }
    }
}
void transfer(SOCKET socket, uint8_t* bytes, size_t size, bool writing, Clock::time_point deadline, std::stop_token stop, bool packet_boundary = false) {
    const size_t initial_size = size;
    while (size) {
        check_stop(stop);
        wait_socket(socket, writing, deadline, stop);
        const int amount = writing ? ::send(socket, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0)
                                   : recv(socket, reinterpret_cast<char*>(bytes), static_cast<int>(size), 0);
        if (amount == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            socket_error(writing ? "Cannot send packet" : "Cannot receive packet");
        }
        if (!amount) throw Disconnected(packet_boundary && size == initial_size);
        bytes += amount; size -= static_cast<size_t>(amount);
    }
}
constexpr uint8_t protocol_version = 1;
enum class Message : uint8_t { hello = 1, ready, start, acknowledge, commit };
void send_message(Socket& socket, Message kind, bool ready = false) {
    const std::array<uint8_t, 2> message{static_cast<uint8_t>(kind), static_cast<uint8_t>(ready)};
    socket.send(message);
}
bool receive_message(Socket& socket, Message expected) {
    const auto message = socket.receive();
    if (message.size() != 2 || message[0] != static_cast<uint8_t>(expected) || message[1] > 1 ||
        (expected != Message::ready && expected != Message::acknowledge && message[1] != 0))
        throw std::runtime_error("Invalid lobby message");
    return message[1] != 0;
}
}

bool valid_address(const std::string& address) {
    in_addr parsed{};
    return address.find('\0') == std::string::npos && InetPtonA(AF_INET, address.c_str(), &parsed) == 1;
}
struct Socket::Impl {
    Handle socket;
    std::stop_token stop;
    std::chrono::milliseconds timeout;
    // A host may wait all day for a connection; a connected peer must keep responding.
    Impl(std::stop_token token, std::chrono::milliseconds limit) : stop(token), timeout(std::min(limit, std::chrono::milliseconds(15000))) {}
};
Socket::Socket(const Endpoint& endpoint, std::stop_token stop, std::chrono::milliseconds timeout,std::function<void()> listening)
    : impl_(std::make_unique<Impl>(stop, timeout)) {
    if (!valid_address(endpoint.address)) throw std::runtime_error("Enter a numeric IPv4 address");
    if (!endpoint.port) throw std::runtime_error("Network port must be nonzero");
    if (timeout <= 0ms) throw std::runtime_error("Network timeout must be positive");
    initialize(); check_stop(stop);
    const auto deadline = Clock::now() + timeout;
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(endpoint.port);
    InetPtonA(AF_INET, endpoint.address.c_str(), &address.sin_addr);
    if (endpoint.host) {
        Handle listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (listener.value == INVALID_SOCKET) socket_error("Cannot create listening socket");
        const BOOL exclusive = TRUE;
        if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
            socket_error("Cannot reserve listening port");
        address.sin_addr.s_addr = htonl(endpoint.loopback ? INADDR_LOOPBACK : INADDR_ANY);
        if (bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            const int error=WSAGetLastError();
            if(error==WSAEADDRINUSE || error==WSAEACCES)throw PortConflict{};
            socket_error("Cannot bind listening port",error);
        }
        if (listen(listener.value, 1) == SOCKET_ERROR) socket_error("Cannot listen for peer");
        nonblocking(listener.value);
        if(listening)listening();
        for (;;) {
            wait_socket(listener.value, false, deadline, stop);
            const SOCKET connected = accept(listener.value, nullptr, nullptr);
            if (connected != INVALID_SOCKET) { impl_->socket.value = connected; break; }
            if (WSAGetLastError() != WSAEWOULDBLOCK) socket_error("Cannot accept peer");
        }
    } else {
        for (;;) {
            check_stop(stop);
            Handle candidate(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (candidate.value == INVALID_SOCKET) socket_error("Cannot create connection socket");
            nonblocking(candidate.value);
            int error = 0;
            if (connect(candidate.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
                error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                    wait_socket(candidate.value, true, deadline, stop);
                    int length = sizeof(error);
                    if (getsockopt(candidate.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == SOCKET_ERROR)
                        socket_error("Cannot inspect connection result");
                }
            }
            if (!error) { impl_->socket.value = candidate.release(); break; }
            if (error != WSAECONNREFUSED) socket_error("Cannot connect to peer", error);
            if (Clock::now() >= deadline) throw std::runtime_error("Connection to peer timed out");
            std::this_thread::sleep_for(10ms);
        }
    }
    nonblocking(impl_->socket.value);
    const BOOL no_delay = TRUE;
    if (setsockopt(impl_->socket.value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) == SOCKET_ERROR)
        socket_error("Cannot configure peer connection");
}
Socket::~Socket() = default;
void Socket::send(std::span<const uint8_t> packet) {
    if (packet.empty() || packet.size() > max_packet) throw std::runtime_error("Packet length must be between 1 and 32768 bytes");
    const auto size = static_cast<uint32_t>(packet.size());
    std::array<uint8_t, 8> header{'U', 'C', protocol_version, 0, static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
    std::array<uint8_t, max_packet+8> wire;
    std::copy(header.begin(),header.end(),wire.begin());
    std::copy(packet.begin(),packet.end(),wire.begin()+header.size());
    const auto deadline = Clock::now() + impl_->timeout;
    transfer(impl_->socket.value, wire.data(), header.size()+packet.size(), true, deadline, impl_->stop);
}
std::vector<uint8_t> Socket::receive() {
    std::array<uint8_t, 8> header{};
    const auto deadline = Clock::now() + impl_->timeout;
    transfer(impl_->socket.value, header.data(), header.size(), false, deadline, impl_->stop, true);
    if (header[0] != 'U' || header[1] != 'C' || header[2] != protocol_version || header[3]) throw std::runtime_error("Unsupported packet protocol version");
    const uint32_t size = (uint32_t(header[4]) << 24) | (uint32_t(header[5]) << 16) | (uint32_t(header[6]) << 8) | header[7];
    if (!size || size > max_packet) throw std::runtime_error("Invalid packet length");
    std::vector<uint8_t> packet(size);
    transfer(impl_->socket.value, packet.data(), packet.size(), false, deadline, impl_->stop);
    return packet;
}

struct Lobby::Impl {
    Endpoint endpoint;
    std::chrono::milliseconds connect_timeout;
    mutable std::mutex mutex;
    LobbyState state;
    std::atomic<bool> desired_ready{false};
    bool start_taken = false;
    std::jthread worker;
    explicit Impl(Endpoint value,std::chrono::milliseconds timeout) : endpoint(std::move(value)),connect_timeout(timeout),worker([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() { worker.request_stop(); worker.join(); }
    void run(std::stop_token stop) {
        try {
            auto socket = std::make_unique<Socket>(endpoint, stop, endpoint.host ? std::chrono::hours(24) : connect_timeout,
                [this]{std::lock_guard lock(mutex);state.listening=true;});
            send_message(*socket, Message::hello);
            receive_message(*socket, Message::hello);
            { std::lock_guard lock(mutex); state.phase = LobbyPhase::connected; }
            for (;;) {
                check_stop(stop);
                bool local = desired_ready.load();
                bool remote;
                if (endpoint.host) {
                    send_message(*socket, Message::ready, local);
                    remote = receive_message(*socket, Message::ready);
                } else {
                    remote = receive_message(*socket, Message::ready);
                    local = desired_ready.load();
                    send_message(*socket, Message::ready, local);
                }
                { std::lock_guard lock(mutex); state.remote_ready = remote; }
                if (local && remote) {
                    if (endpoint.host) {
                        // The client needs a response even if readiness changed during this exchange.
                        const bool confirmed = desired_ready.load();
                        send_message(*socket, Message::start, false);
                        const bool accepted = receive_message(*socket, Message::acknowledge);
                        send_message(*socket, Message::acknowledge, confirmed && accepted);
                        if (!confirmed || !accepted) continue;
                        receive_message(*socket, Message::commit);
                        // The client closes first so the host port avoids TCP TIME_WAIT.
                        try { socket->receive(); throw std::runtime_error("Unexpected packet after lobby start"); }
                        catch (const Disconnected& error) { if (!error.clean) throw; }
                    } else {
                        receive_message(*socket, Message::start);
                        const bool accepted = desired_ready.load();
                        send_message(*socket, Message::acknowledge, accepted);
                        const bool confirmed = receive_message(*socket, Message::acknowledge);
                        if (confirmed && !accepted) throw std::runtime_error("Invalid lobby start acknowledgement");
                        if (!confirmed) continue;
                        send_message(*socket, Message::commit);
                    }
                    socket.reset();
                    std::lock_guard lock(mutex);
                    if (!stop.stop_requested()) state.phase = LobbyPhase::launching;
                    return;
                }
                std::this_thread::sleep_for(10ms);
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            state.phase = LobbyPhase::failed;
            state.error = error.what();
            state.port_conflict = dynamic_cast<const PortConflict*>(&error)!=nullptr;
        }
    }
};
Lobby::Lobby(Endpoint endpoint,std::chrono::milliseconds timeout) : impl_(std::make_unique<Impl>(std::move(endpoint),timeout)) {}
Lobby::~Lobby() = default;
void Lobby::ready(bool ready) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state.phase == LobbyPhase::launching || impl_->state.phase == LobbyPhase::failed) return;
    impl_->desired_ready.store(ready); impl_->state.local_ready = ready;
}
LobbyState Lobby::state() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
std::optional<Endpoint> Lobby::take_start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state.phase != LobbyPhase::launching || impl_->start_taken) return std::nullopt;
    impl_->start_taken = true;
    return impl_->endpoint;
}
}
