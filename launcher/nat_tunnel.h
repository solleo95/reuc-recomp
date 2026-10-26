#pragma once
#include "netplay.h"

namespace uc::net {
enum class TunnelPhase { starting,waiting,needs_punch,connected,failed };
struct TunnelState {
    TunnelPhase phase=TunnelPhase::starting;
    Endpoint local{false,"127.0.0.1",0};
    uint16_t bound_port=0;
    std::string public_address,error,discovery_error;
    bool symmetric=false,punching=false,port_conflict=false;
};
// One UDP socket survives the lobby/game process handoff. Only its worker touches ENet.
class NatTunnel {
public:
    explicit NatTunnel(Endpoint,std::vector<std::string> stun_servers={"stun.l.google.com:19302","stun.cloudflare.com:3478"},
                       std::chrono::milliseconds connect_timeout=std::chrono::seconds(15));
    ~NatTunnel();
    NatTunnel(const NatTunnel&)=delete;
    NatTunnel& operator=(const NatTunnel&)=delete;
    TunnelState state() const;
    void punch(const std::string& peer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
