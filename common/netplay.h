#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace uc::net {
constexpr uint16_t default_port=27888;
constexpr size_t max_packet=32768;
struct Endpoint { bool host=false; std::string address="127.0.0.1"; uint16_t port=default_port; bool loopback=false; };
bool valid_address(const std::string&);
// At most one sender and one receiver may run concurrently; join both before destruction.
// A stop token cancels connect, accept and packet I/O.
class Socket {
public:
    Socket(const Endpoint&,std::stop_token={},std::chrono::milliseconds timeout=std::chrono::seconds(15));
    ~Socket();
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
    void send(std::span<const uint8_t>);
    std::vector<uint8_t> receive();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
enum class LobbyPhase { waiting,connected,launching,failed };
struct LobbyState { LobbyPhase phase=LobbyPhase::waiting; bool local_ready=false,remote_ready=false; std::string error; };
class Lobby {
public:
    explicit Lobby(Endpoint);
    ~Lobby();
    Lobby(const Lobby&)=delete;
    Lobby& operator=(const Lobby&)=delete;
    void ready(bool);
    LobbyState state() const;
    std::optional<Endpoint> take_start(); // Exactly once, after both ready and start acknowledged.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
