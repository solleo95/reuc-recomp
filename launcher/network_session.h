#pragma once
#include "nat_tunnel.h"

namespace uc::net {
std::optional<Endpoint> parse_connection_address(const std::string&);
struct NetworkState {
    LobbyState lobby;
    uint16_t port=0;
    std::vector<std::string> local_addresses;
    std::string public_address,discovery_error,punch_address;
    bool tunneled=false,needs_punch=false,punching=false;
};
// Poll and consume on the UI thread; transport workers publish their own synchronized snapshots.
class NetworkSession {
public:
    explicit NetworkSession(Endpoint,std::vector<std::string> servers={"stun.l.google.com:19302","stun.cloudflare.com:3478"});
    ~NetworkSession();
    void poll(bool ready);
    void punch(const std::string& peer);
    NetworkState state() const {return state_;}
    std::optional<Endpoint> take_start();
private:
    void start_host(uint16_t port);
    Endpoint endpoint_;
    std::vector<std::string> servers_;
    std::string local_address_;
    NetworkState state_;
    std::unique_ptr<NatTunnel> tunnel_;
    std::unique_ptr<Lobby> direct_,bridged_;
    enum class Route { none,direct,tunnel } route_=Route::none;
    std::optional<Endpoint> start_;
    unsigned attempts_=0;
    std::chrono::steady_clock::time_point started_=std::chrono::steady_clock::now();
};
}
