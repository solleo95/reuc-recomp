#include "network_session.h"
#include "nat_mapping.h"
#include "stun.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace uc::net {
namespace {
constexpr auto connect_timeout=std::chrono::seconds(5);
struct LocalAddress {std::string text;uint32_t address,mask;};
std::vector<LocalAddress> local_addresses() {
    ULONG size=16384;
    std::vector<uint8_t> storage(size);
    ULONG result=ERROR_BUFFER_OVERFLOW;
    for(unsigned attempt=0;attempt<3 && result==ERROR_BUFFER_OVERFLOW;++attempt) {
        storage.resize(size);
        result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()),&size);
    }
    if(result==ERROR_NO_DATA)return {};
    if(result!=NO_ERROR)throw std::runtime_error("Cannot enumerate local IP addresses (Windows "+std::to_string(result)+")");
    std::vector<LocalAddress> addresses;
    for(auto* adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());adapter;adapter=adapter->Next) {
        if(adapter->OperStatus!=IfOperStatusUp || adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        for(auto* unicast=adapter->FirstUnicastAddress;unicast;unicast=unicast->Next) {
            if(unicast->Address.lpSockaddr->sa_family!=AF_INET || unicast->OnLinkPrefixLength>32)continue;
            const auto& address=reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr;
            char text[INET_ADDRSTRLEN]{};
            if(!InetNtopA(AF_INET,&address,text,sizeof(text)))throw std::runtime_error("Cannot format local IP address");
            const auto ip=ntohl(address.s_addr);
            if(!ip || (ip>>24)==127)continue;
            const auto mask=unicast->OnLinkPrefixLength ? UINT32_MAX<<(32-unicast->OnLinkPrefixLength) : 0;
            if(std::none_of(addresses.begin(),addresses.end(),[&](const auto& item){return item.address==ip;}))addresses.push_back({text,ip,mask});
        }
    }
    return addresses;
}
bool direct_peer(const std::string& address,const std::vector<LocalAddress>& locals) {
    if(!public_router_address(address))return true;
    in_addr parsed{};InetPtonA(AF_INET,address.c_str(),&parsed);
    const auto ip=ntohl(parsed.s_addr);
    return std::any_of(locals.begin(),locals.end(),[&](const auto& item){return ip==item.address || (item.mask && (ip&item.mask)==(item.address&item.mask));});
}
std::string source_address(const std::string& target) {
    SOCKADDR_INET destination{},source{};destination.Ipv4.sin_family=AF_INET;
    InetPtonA(AF_INET,target.c_str(),&destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};
    if(GetBestRoute2(nullptr,0,nullptr,&destination,0,&route,&source)!=NO_ERROR)return {};
    char text[INET_ADDRSTRLEN]{};
    if(!InetNtopA(AF_INET,&source.Ipv4.sin_addr,text,sizeof(text)))return {};
    return text;
}
}
std::optional<Endpoint> parse_connection_address(const std::string& text) {
    const auto first=text.find_first_not_of(" \t\r\n"),last=text.find_last_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    auto value=text.substr(first,last-first+1);
    if(value.starts_with("localhost:"))value.replace(0,9,"127.0.0.1");
    return parse_peer_address(value);
}
NetworkSession::NetworkSession(Endpoint endpoint,std::vector<std::string> servers):endpoint_(std::move(endpoint)),servers_(std::move(servers)) {
    if(!endpoint_.host && !parse_connection_address(endpoint_.address+":"+std::to_string(endpoint_.port)))throw std::invalid_argument("Enter IPv4:port");
    const auto locals=local_addresses();
    if(endpoint_.host) {
        for(const auto& local:locals)state_.local_addresses.push_back(local.text);
        state_.local_addresses.push_back("127.0.0.1");
        start_host(endpoint_.port);
    } else {
        endpoint_.address=parse_connection_address(endpoint_.address+":"+std::to_string(endpoint_.port))->address;
        state_.port=endpoint_.port;
        if(direct_peer(endpoint_.address,locals)) {
            local_address_=source_address(endpoint_.address);
            direct_=std::make_unique<Lobby>(endpoint_,connect_timeout);
        } else {state_.tunneled=true;tunnel_=std::make_unique<NatTunnel>(endpoint_,servers_,connect_timeout);}
        std::fprintf(stderr,"[network] joining %s:%u route=%s timeout=5s\n",endpoint_.address.c_str(),endpoint_.port,state_.tunneled ? "UDP" : "TCP");
    }
}
NetworkSession::~NetworkSession()=default;
void NetworkSession::start_host(uint16_t port) {
    direct_.reset();bridged_.reset();tunnel_.reset();
    state_.port=0;
    if(++attempts_>16)throw std::runtime_error("Cannot reserve an available TCP/UDP port");
    tunnel_=std::make_unique<NatTunnel>(Endpoint{true,"0.0.0.0",port},servers_);
    std::fprintf(stderr,"[network] binding all IPv4 interfaces, requested port=%u\n",port);
}
void NetworkSession::poll(bool ready) {
    if(state_.lobby.phase==LobbyPhase::failed)return;
    try {
        if(tunnel_) {
            const auto tunnel=tunnel_->state();
            state_.public_address=tunnel.public_address;state_.discovery_error=tunnel.discovery_error;
            state_.needs_punch=!endpoint_.host && (tunnel.phase==TunnelPhase::waiting || tunnel.phase==TunnelPhase::needs_punch)
                && std::chrono::steady_clock::now()-started_>=connect_timeout;
            state_.punching=tunnel.punching;
            if(!endpoint_.host)state_.punch_address=local_address_.empty() ? tunnel.public_address :
                (tunnel.bound_port ? local_address_+":"+std::to_string(tunnel.bound_port) : std::string{});
            if(tunnel.phase==TunnelPhase::failed) {
                if(endpoint_.host && !state_.port && tunnel.port_conflict) {start_host(0);return;}
                throw std::runtime_error(tunnel.error);
            }
            if(state_.lobby.phase==LobbyPhase::launching)return;
            if(endpoint_.host && route_==Route::none && tunnel.bound_port) {
                if(!direct_)direct_=std::make_unique<Lobby>(Endpoint{true,"0.0.0.0",tunnel.bound_port});
                const auto direct=direct_->state();
                if(direct.phase==LobbyPhase::failed) {
                    if(!state_.port && direct.port_conflict) {start_host(0);return;}
                    throw std::runtime_error(direct.error);
                }
                if(!direct.listening)return;
                if(!state_.port)std::fprintf(stderr,"[network] TCP listener ready: 0.0.0.0:%u\n",tunnel.bound_port);
                state_.port=tunnel.bound_port;
                if(!bridged_)bridged_=std::make_unique<Lobby>(tunnel.local);
            } else if(!endpoint_.host) {
                if(tunnel.phase==TunnelPhase::connected && !bridged_)bridged_=std::make_unique<Lobby>(tunnel.local,connect_timeout);
            }
        }
        if(state_.lobby.phase==LobbyPhase::launching)return;
        if(route_==Route::none) {
            if(direct_ && direct_->state().phase==LobbyPhase::connected) {
                route_=Route::direct;bridged_.reset();tunnel_.reset();state_.tunneled=false;
            } else if(bridged_ && bridged_->state().phase==LobbyPhase::connected) {
                route_=Route::tunnel;direct_.reset();state_.tunneled=true;
            }
        }
        auto* lobby=route_==Route::tunnel ? bridged_.get() : direct_.get();
        if(!lobby && bridged_)lobby=bridged_.get();
        if(!lobby)return;
        if(route_!=Route::none)lobby->ready(ready);
        state_.lobby=lobby->state();
        if(state_.lobby.phase==LobbyPhase::failed) {
            if(!endpoint_.host && route_==Route::none && direct_) {
                std::fprintf(stderr,"[network] TCP %s:%u failed: %s; trying UDP\n",endpoint_.address.c_str(),endpoint_.port,state_.lobby.error.c_str());
                direct_.reset();state_.lobby={};state_.tunneled=true;
                tunnel_=std::make_unique<NatTunnel>(endpoint_,std::vector<std::string>{},connect_timeout);
                return;
            }
            throw std::runtime_error(state_.lobby.error);
        }
        if(route_!=Route::none && !start_)start_=lobby->take_start();
    } catch(const std::exception& error) {
        state_.lobby.phase=LobbyPhase::failed;state_.lobby.error=error.what();
        direct_.reset();bridged_.reset();tunnel_.reset();
    }
}
std::optional<Endpoint> NetworkSession::take_start() {return std::exchange(start_,{});}
void NetworkSession::punch(const std::string& peer) {
    if(!endpoint_.host || route_!=Route::none || !tunnel_)throw std::runtime_error("NAT punch unavailable");
    const auto parsed=parse_connection_address(peer);
    if(!parsed)throw std::invalid_argument("Enter the other player's IPv4:port");
    tunnel_->punch(parsed->address+":"+std::to_string(parsed->port));
}
}
