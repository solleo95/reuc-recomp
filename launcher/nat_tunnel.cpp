#include "nat_tunnel.h"
#include "stun.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <enet/enet.h>
#include <algorithm>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uc::net {
namespace {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr uint32_t tunnel_version=0x55435401;
constexpr size_t queue_limit=4*1024*1024,chunk_size=16384,datagram_limit=1172;
enum class Wire : uint8_t { open=1,data,fin };
struct EnetRuntime {
    EnetRuntime(){if(enet_initialize())throw std::runtime_error("Cannot initialize UDP networking");}
    ~EnetRuntime(){enet_deinitialize();}
};
void initialize(){static EnetRuntime runtime;}
struct SocketHandle {
    SOCKET value=INVALID_SOCKET;
    ~SocketHandle(){reset();}
    void reset(SOCKET socket=INVALID_SOCKET){if(value!=INVALID_SOCKET)closesocket(value);value=socket;}
};
struct SocketError : std::runtime_error {
    int code;
    SocketError(const char* operation,int error):std::runtime_error(std::string(operation)+" (Winsock "+std::to_string(error)+")"),code(error){}
};
[[noreturn]] void socket_error(const char* operation){throw SocketError(operation,WSAGetLastError());}
sockaddr_in address(uint16_t port,uint32_t ip=htonl(INADDR_LOOPBACK)) {
    sockaddr_in value{};value.sin_family=AF_INET;value.sin_addr.s_addr=ip;value.sin_port=htons(port);return value;
}
void nonblocking(SOCKET socket) {
    u_long enabled=1;
    if(ioctlsocket(socket,FIONBIO,&enabled)==SOCKET_ERROR)socket_error("Cannot configure tunnel socket");
}
uint16_t bind_local(SocketHandle& socket,int type,uint16_t port=0) {
    socket.reset(::socket(AF_INET,type,type==SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP));
    if(socket.value==INVALID_SOCKET)socket_error("Cannot create local tunnel socket");
    const BOOL exclusive=TRUE;
    if(setsockopt(socket.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==SOCKET_ERROR)socket_error("Cannot reserve local tunnel socket");
    auto local=address(port);
    if(bind(socket.value,reinterpret_cast<const sockaddr*>(&local),sizeof(local))==SOCKET_ERROR)socket_error("Cannot bind local tunnel socket");
    int size=sizeof(local);
    if(getsockname(socket.value,reinterpret_cast<sockaddr*>(&local),&size)==SOCKET_ERROR)socket_error("Cannot inspect tunnel port");
    nonblocking(socket.value);return ntohs(local.sin_port);
}
uint16_t bind_pair(SocketHandle& tcp,SocketHandle& udp) {
    // TCP and UDP have independent ephemeral allocators. Reserve both before publishing a port.
    for(unsigned attempt=0;attempt<16;++attempt) {
        const auto port=bind_local(udp,SOCK_DGRAM);
        try {bind_local(tcp,SOCK_STREAM,port);return port;}
        catch(const SocketError& error) {
            if(error.code!=WSAEADDRINUSE && error.code!=WSAEACCES)throw;
            tcp.reset();udp.reset();
        }
    }
    throw std::runtime_error("Cannot reserve a local TCP/UDP port pair");
}
std::array<uint8_t,12> transaction() {
    std::array<uint8_t,12> result{};
    if(BCryptGenRandom(nullptr,result.data(),ULONG(result.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Cannot generate STUN transaction");
    return result;
}
std::optional<ENetAddress> resolve(const std::string& server,std::stop_token stop) {
    const auto colon=server.find(':');
    if(colon==std::string::npos || server.find('\0')!=std::string::npos)return {};
    unsigned port=0;auto [end,error]=std::from_chars(server.data()+colon+1,server.data()+server.size(),port);
    if(error!=std::errc{} || end!=server.data()+server.size() || !port || port>65535)return {};
    const auto hostname=server.substr(0,colon);ENetAddress result{};result.port=uint16_t(port);
    if(InetPtonA(AF_INET,hostname.c_str(),&result.host)==1)return result;
    ADDRINFOEXW hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;
    PADDRINFOEXW answer=nullptr;HANDLE cancel=nullptr;OVERLAPPED pending{};
    pending.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!pending.hEvent)throw std::runtime_error("Cannot create DNS completion event");
    const std::wstring name(hostname.begin(),hostname.end());
    int status=GetAddrInfoExW(name.c_str(),nullptr,NS_DNS,nullptr,&hints,&answer,nullptr,&pending,nullptr,&cancel);
    if(status==WSA_IO_PENDING) {
        const auto deadline=Clock::now()+2s;
        while(WaitForSingleObject(pending.hEvent,25)==WAIT_TIMEOUT) {
            if(stop.stop_requested() || Clock::now()>=deadline) {
                GetAddrInfoExCancel(&cancel);
                WaitForSingleObject(pending.hEvent,INFINITE);break;
            }
        }
        status=GetAddrInfoExOverlappedResult(&pending);
    }
    CloseHandle(pending.hEvent);
    if(status==0 && answer && answer->ai_addrlen>=sizeof(sockaddr_in))result.host=reinterpret_cast<sockaddr_in*>(answer->ai_addr)->sin_addr.s_addr;
    if(answer)FreeAddrInfoExW(answer);
    if(status!=0 || !result.host)return {};
    return result;
}
bool same_address(const ENetAddress& a,const ENetAddress& b){return a.host==b.host && a.port==b.port;}
}

struct NatTunnel::Impl {
    Endpoint endpoint;
    std::vector<std::string> servers;
    std::chrono::milliseconds connect_timeout;
    mutable std::mutex mutex;
    TunnelState snapshot;
    std::optional<Endpoint> requested_punch;
    size_t queued=0;
    std::unique_ptr<ENetHost,decltype(&enet_host_destroy)> udp{nullptr,enet_host_destroy};
    ENetPeer* peer=nullptr;
    SocketHandle listener,tcp,datagrams;
    uint16_t local_port=0;
    sockaddr_in local_datagram{};
    bool datagram_known=false,connected=false,active=false,connecting=false,local_fin=false,remote_fin=false,write_closed=false;
    std::vector<uint8_t> incoming;
    size_t written=0;
    Clock::time_point stream_deadline{},next_local_connect{},next_connect{},punch_end{},next_punch{},next_probe{};
    ENetAddress punch_target{};
    struct Probe {ENetAddress server;std::array<uint8_t,12> id;std::string mapped;};
    std::vector<Probe> probes;
    std::optional<ENetAddress> discovered_by;
    static thread_local Impl* current;
    std::jthread worker;

    Impl(Endpoint value,std::vector<std::string> targets,std::chrono::milliseconds timeout)
        :endpoint(std::move(value)),servers(std::move(targets)),connect_timeout(timeout) {
        if(!valid_address(endpoint.address) || (!endpoint.host && !parse_peer_address(endpoint.address+":"+std::to_string(endpoint.port))) || timeout<=0ms || servers.size()>4)
            throw std::invalid_argument("Invalid tunnel endpoint");
        initialize();worker=std::jthread([this](std::stop_token stop){run(stop);});
    }
    ~Impl(){worker.request_stop();worker.join();}
    void phase(TunnelPhase value){std::lock_guard lock(mutex);snapshot.phase=value;}
    void send_packet(uint8_t channel,std::span<const uint8_t> bytes,enet_uint32 flags) {
        if(bytes.size()>queue_limit-queued)throw std::runtime_error("Tunnel send queue full");
        auto* packet=enet_packet_create(bytes.data(),bytes.size(),flags);
        if(!packet)throw std::runtime_error("Cannot allocate tunnel packet");
        queued+=packet->dataLength;packet->userData=&queued;
        packet->freeCallback=[](ENetPacket* p){*static_cast<size_t*>(p->userData)-=p->dataLength;};
        if(enet_peer_send(peer,channel,packet)<0){enet_packet_destroy(packet);throw std::runtime_error("Cannot send tunnel packet");}
    }
    void send_control(Wire kind){const uint8_t byte=uint8_t(kind);send_packet(0,{&byte,1},ENET_PACKET_FLAG_RELIABLE);}
    void raw_send(const ENetAddress& target,const std::array<uint8_t,20>& packet) {
        ENetBuffer buffer{packet.size(),const_cast<uint8_t*>(packet.data())};
        // UDP probes are best effort; STUN timeout/peer timeout reports failure to the UI.
        enet_socket_send(udp->socket,&target,&buffer,1);
    }
    static int ENET_CALLBACK intercept(ENetHost* host,ENetEvent*) {
        const std::span<const uint8_t> bytes(host->receivedData,host->receivedDataLength);
        if(bytes.size()<20 || bytes[4]!=0x21 || bytes[5]!=0x12 || bytes[6]!=0xa4 || bytes[7]!=0x42)return 0;
        for(auto& probe:current->probes)if(same_address(probe.server,host->receivedAddress)) {
            if(auto value=stun_response(bytes,probe.id)) {
                probe.mapped=value->address+":"+std::to_string(value->port);
                std::lock_guard lock(current->mutex);
                if(!current->discovered_by)current->discovered_by=probe.server;
                if(same_address(*current->discovered_by,probe.server))current->snapshot.public_address=probe.mapped;
                current->snapshot.discovery_error.clear();
                for(const auto& other:current->probes)if(other.server.host!=probe.server.host && !other.mapped.empty() && other.mapped!=probe.mapped)current->snapshot.symmetric=true;
            }
        }
        return 1;
    }
    void open_stream() {
        active=true;connecting=false;local_fin=remote_fin=write_closed=false;incoming.clear();written=0;
        stream_deadline=Clock::now()+15s;next_local_connect={};
        datagram_known=endpoint.host;
        if(endpoint.host)local_datagram=address(local_port);
    }
    void receive_packet(const ENetEvent& event) {
        std::unique_ptr<ENetPacket,decltype(&enet_packet_destroy)> packet(event.packet,enet_packet_destroy);
        const std::span<const uint8_t> bytes(packet->data,packet->dataLength);
        if(event.channelID==1) {
            if(bytes.empty() || bytes.size()>datagram_limit)throw std::runtime_error("Invalid tunnel datagram");
            if(datagram_known) {
                const int sent=sendto(datagrams.value,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&local_datagram),sizeof(local_datagram));
                if(sent==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK && WSAGetLastError()!=WSAENOBUFS)socket_error("Cannot forward local datagram");
            }
            return;
        }
        if(event.channelID!=0 || bytes.empty() || !(packet->flags&ENET_PACKET_FLAG_RELIABLE))throw std::runtime_error("Invalid tunnel stream packet");
        switch(Wire(bytes[0])) {
        case Wire::open:
            if(bytes.size()!=1 || !endpoint.host || active)throw std::runtime_error("Unexpected tunnel stream open");
            open_stream();break;
        case Wire::data:
            if(!active || remote_fin || bytes.size()<2 || bytes.size()>chunk_size+1 || bytes.size()-1>queue_limit-(incoming.size()-written))throw std::runtime_error("Invalid tunnel stream data");
            if(written){incoming.erase(incoming.begin(),incoming.begin()+written);written=0;}
            incoming.insert(incoming.end(),bytes.begin()+1,bytes.end());break;
        case Wire::fin:
            if(bytes.size()!=1 || !active || remote_fin)throw std::runtime_error("Unexpected tunnel stream close");
            remote_fin=true;break;
        default:throw std::runtime_error("Unsupported tunnel protocol");
        }
    }
    void service_stream() {
        if(!connected)return;
        const auto now=Clock::now();
        if(!endpoint.host && !active) {
            auto accepted=accept(listener.value,nullptr,nullptr);
            if(accepted!=INVALID_SOCKET){tcp.reset(accepted);nonblocking(accepted);open_stream();send_control(Wire::open);}
            else if(WSAGetLastError()!=WSAEWOULDBLOCK)socket_error("Cannot accept local connection");
        }
        if(!active)return;
        if(endpoint.host && tcp.value==INVALID_SOCKET && now>=next_local_connect) {
            tcp.reset(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
            if(tcp.value==INVALID_SOCKET)socket_error("Cannot create local connection");
            nonblocking(tcp.value);const auto local=address(local_port);
            const int result=connect(tcp.value,reinterpret_cast<const sockaddr*>(&local),sizeof(local));
            if(result==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)socket_error("Cannot connect to local game");
            connecting=result==SOCKET_ERROR;
        }
        if(connecting) {
            fd_set writing,errors;FD_ZERO(&writing);FD_ZERO(&errors);FD_SET(tcp.value,&writing);FD_SET(tcp.value,&errors);timeval timeout{};
            const int ready=select(0,nullptr,&writing,&errors,&timeout);
            if(ready==SOCKET_ERROR)socket_error("Cannot poll local connection");
            if(ready>0) {
                int error=0,size=sizeof(error);
                if(getsockopt(tcp.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)==SOCKET_ERROR)socket_error("Cannot inspect local connection");
                connecting=false;
                if(error){tcp.reset();next_local_connect=now+25ms;}
            }
        }
        if(tcp.value==INVALID_SOCKET || connecting) {
            if(now>=stream_deadline)throw std::runtime_error("Local game connection timed out");
            return;
        }
        const BOOL no_delay=TRUE;
        if(setsockopt(tcp.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&no_delay),sizeof(no_delay))==SOCKET_ERROR)socket_error("Cannot configure local TCP");
        if(written<incoming.size()) {
            const int sent=::send(tcp.value,reinterpret_cast<const char*>(incoming.data()+written),int(incoming.size()-written),0);
            if(sent>0)written+=size_t(sent);
            else if(sent==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)socket_error("Cannot forward local stream");
            if(written==incoming.size()){incoming.clear();written=0;}
        }
        if(remote_fin && incoming.empty() && !write_closed) {
            if(shutdown(tcp.value,SD_SEND)==SOCKET_ERROR)socket_error("Cannot close local stream");
            write_closed=true;
        }
        if(!local_fin && queued<queue_limit-chunk_size-1) {
            std::array<uint8_t,chunk_size+1> bytes{};bytes[0]=uint8_t(Wire::data);
            const int count=recv(tcp.value,reinterpret_cast<char*>(bytes.data()+1),int(chunk_size),0);
            if(count>0)send_packet(0,{bytes.data(),size_t(count)+1},ENET_PACKET_FLAG_RELIABLE);
            else if(count==0){local_fin=true;send_control(Wire::fin);}
            else if(WSAGetLastError()!=WSAEWOULDBLOCK)socket_error("Cannot read local stream");
        }
        if(local_fin && write_closed){tcp.reset();active=false;}
    }
    void service_datagrams() {
        if(!connected)return;
        for(unsigned i=0;i<32;++i) {
            std::array<uint8_t,datagram_limit> bytes{};sockaddr_in source{};int size=sizeof(source);
            const int count=recvfrom(datagrams.value,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&source),&size);
            if(count==SOCKET_ERROR) {
                const int error=WSAGetLastError();
                if(error==WSAEWOULDBLOCK)return;
                if(error==WSAECONNRESET || error==WSAEMSGSIZE)continue;
                socket_error("Cannot read local datagram");
            }
            if(!count || source.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || (datagram_known && source.sin_port!=local_datagram.sin_port))continue;
            if(!datagram_known){local_datagram=source;datagram_known=true;}
            if(queued<queue_limit-datagram_limit)send_packet(1,{bytes.data(),size_t(count)},ENET_PACKET_FLAG_UNSEQUENCED);
        }
    }
    void run(std::stop_token stop) {
        try {
            current=this;
            ENetAddress bind_address{ENET_HOST_ANY,endpoint.host ? endpoint.port : uint16_t(0)};
            udp.reset(enet_host_create(&bind_address,1,2,0,0));
            if(!udp)throw std::runtime_error("Cannot bind UDP tunnel port");
            udp->intercept=intercept;udp->maximumPacketSize=chunk_size+1;udp->maximumWaitingData=queue_limit;
            // The public UDP socket and the local game socket must have distinct ports.
            if(endpoint.host) {
                SocketHandle tcp_reservation,udp_reservation;
                local_port=bind_pair(tcp_reservation,udp_reservation);bind_local(datagrams,SOCK_DGRAM);
            } else {
                local_port=bind_pair(listener,datagrams);
                if(listen(listener.value,1)==SOCKET_ERROR)socket_error("Cannot listen for local game");
            }
            {std::lock_guard lock(mutex);snapshot.local={endpoint.host,"127.0.0.1",local_port,true};snapshot.bound_port=udp->address.port;snapshot.phase=TunnelPhase::waiting;}
            for(const auto& server:servers) {
                if(stop.stop_requested())break;
                if(auto target=resolve(server,stop))probes.push_back({*target,transaction(),{}});
            }
            const auto started=Clock::now();next_probe=started;
            ENetAddress remote{};InetPtonA(AF_INET,endpoint.address.c_str(),&remote.host);remote.port=endpoint.port;
            while(!stop.stop_requested()) {
                const auto now=Clock::now();
                if(!endpoint.host && !connected && now>=next_connect) {
                    if(peer)enet_peer_reset(peer);
                    raw_send(remote,stun_request(transaction()));
                    peer=enet_host_connect(udp.get(),&remote,2,tunnel_version);
                    if(!peer)throw std::runtime_error("Cannot start UDP connection");
                    enet_peer_timeout(peer,32,5000,15000);next_connect=now+4s;
                }
                if(!endpoint.host && !connected && now-started>=connect_timeout)phase(TunnelPhase::needs_punch);
                std::optional<Endpoint> punch;
                {std::lock_guard lock(mutex);punch=std::exchange(requested_punch,{});}
                if(punch){InetPtonA(AF_INET,punch->address.c_str(),&punch_target.host);punch_target.port=punch->port;punch_end=now+30s;next_punch=now;}
                const bool punching=!connected && now<punch_end;
                {std::lock_guard lock(mutex);snapshot.punching=punching;}
                if(punching && now>=next_punch){raw_send(punch_target,stun_request(transaction()));next_punch=now+50ms;}
                if(!connected && now>=next_probe) {
                    for(auto& probe:probes)raw_send(probe.server,stun_request(probe.id));
                    next_probe=now+(now-started<5s ? 1s : 20s);
                }
                if(now-started>5s) {std::lock_guard lock(mutex);if(snapshot.public_address.empty())snapshot.discovery_error="STUN unavailable. Check Internet access and UDP firewall rules.";}
                ENetEvent event{};
                for(unsigned i=0;i<64;++i) {
                    const int result=enet_host_service(udp.get(),&event,0);
                    if(result<0)throw std::runtime_error("UDP tunnel service failed");
                    if(!result)break;
                    if(event.type==ENET_EVENT_TYPE_CONNECT) {
                        if(endpoint.host && event.data!=tunnel_version){enet_peer_disconnect_now(event.peer,0);continue;}
                        peer=event.peer;connected=true;enet_peer_ping_interval(peer,1000);enet_peer_timeout(peer,32,5000,15000);phase(TunnelPhase::connected);
                    } else if(event.type==ENET_EVENT_TYPE_RECEIVE)receive_packet(event);
                    else if(event.type==ENET_EVENT_TYPE_DISCONNECT) {
                        if(connected)throw std::runtime_error("Other player disconnected");
                        peer=nullptr;next_connect=now;
                    }
                }
                service_stream();service_datagrams();enet_host_flush(udp.get());
                std::this_thread::sleep_for(1ms);
            }
        } catch(const std::exception& error) {
            std::lock_guard lock(mutex);snapshot.phase=TunnelPhase::failed;snapshot.error=error.what();
        }
        if(peer)enet_peer_disconnect_now(peer,0);
        tcp.reset();listener.reset();datagrams.reset();udp.reset();current=nullptr;
    }
};
thread_local NatTunnel::Impl* NatTunnel::Impl::current=nullptr;
NatTunnel::NatTunnel(Endpoint endpoint,std::vector<std::string> servers,std::chrono::milliseconds timeout)
    :impl_(std::make_unique<Impl>(std::move(endpoint),std::move(servers),timeout)){}
NatTunnel::~NatTunnel()=default;
TunnelState NatTunnel::state() const {std::lock_guard lock(impl_->mutex);return impl_->snapshot;}
void NatTunnel::punch(const std::string& text) {
    auto peer=parse_peer_address(text);
    if(!peer)throw std::invalid_argument("Enter the client's IPv4:port");
    std::lock_guard lock(impl_->mutex);
    if(!impl_->endpoint.host || impl_->snapshot.phase!=TunnelPhase::waiting || impl_->snapshot.punching || impl_->requested_punch)throw std::runtime_error("NAT punch unavailable");
    impl_->requested_punch=std::move(peer);
}
}
