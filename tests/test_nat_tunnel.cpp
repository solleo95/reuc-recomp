#include "check.h"
#include "nat_tunnel.h"
#include "netplay_datagram.h"
#include "stun.h"
#include <algorithm>
#include <future>
#include <thread>
#include <atomic>
#include <source_location>
#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using namespace uc::net;
using namespace std::chrono_literals;

struct FakeNat {
    struct Winsock {
        Winsock(){WSADATA data{};CHECK(WSAStartup(MAKEWORD(2,2),&data)==0);}
        ~Winsock(){WSACleanup();}
    } winsock;
    struct Udp {
        SOCKET value=INVALID_SOCKET;
        sockaddr_in address{};
        Udp() {
            value=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);CHECK(value!=INVALID_SOCKET);
            address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            CHECK(bind(value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0);
            int size=sizeof(address);CHECK(getsockname(value,reinterpret_cast<sockaddr*>(&address),&size)==0);
            u_long nonblocking=1;CHECK(ioctlsocket(value,FIONBIO,&nonblocking)==0);
        }
        ~Udp(){if(value!=INVALID_SOCKET)closesocket(value);}
        std::string endpoint() const{return "127.0.0.1:"+std::to_string(ntohs(address.sin_port));}
    } host_public,client_public,host_stun,client_stun;
    std::atomic<bool> healthy{true},host_invalid{false},client_invalid{false},dropped{false};
    std::atomic<uint16_t> host_port{0},client_port{0};
    std::jthread worker;

    FakeNat():worker([this](std::stop_token stop){run(stop);}){}
    ~FakeNat(){worker.request_stop();worker.join();}
    void run(std::stop_token stop) {
        try {
            sockaddr_in host{},client{};
            bool open=false;
            std::array<std::optional<std::array<uint8_t,12>>,2> transactions;
            const auto same=[](const sockaddr_in& a,const sockaddr_in& b){return a.sin_addr.s_addr==b.sin_addr.s_addr && a.sin_port==b.sin_port;};
            const auto remember=[&](sockaddr_in& known,const sockaddr_in& source,std::atomic<uint16_t>& port) {
                CHECK(source.sin_addr.s_addr==htonl(INADDR_LOOPBACK) && source.sin_port!=0);
                if(known.sin_port)CHECK(same(known,source));
                known=source;port=ntohs(source.sin_port);
            };
            const auto send=[](Udp& socket,const sockaddr_in& target,std::span<const uint8_t> bytes) {
                CHECK(sendto(socket.value,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&target),sizeof(target))==int(bytes.size()));
            };
            while(!stop.stop_requested()) {
                std::array<Udp*,4> sockets{&host_stun,&client_stun,&host_public,&client_public};
                for(size_t index=0;index<sockets.size();++index) {
                    std::array<uint8_t,65536> storage{};sockaddr_in source{};int size=sizeof(source);
                    const int count=recvfrom(sockets[index]->value,reinterpret_cast<char*>(storage.data()),int(storage.size()),0,reinterpret_cast<sockaddr*>(&source),&size);
                    if(count==SOCKET_ERROR){CHECK(WSAGetLastError()==WSAEWOULDBLOCK);continue;}
                    const std::span<const uint8_t> bytes(storage.data(),size_t(count));
                    if(index<2) {
                        remember(index==0 ? host : client,source,index==0 ? host_port : client_port);
                        CHECK(bytes.size()==20);
                        std::array<uint8_t,12> id{};std::copy(bytes.begin()+8,bytes.end(),id.begin());
                        const auto request=stun_request(id);
                        CHECK(std::equal(request.begin(),request.end(),bytes.begin()));
                        CHECK(std::any_of(id.begin(),id.end(),[](uint8_t value){return value!=0;}));
                        if(transactions[index])CHECK(*transactions[index]==id);
                        const auto port=ntohs((index==0 ? host_public : client_public).address.sin_port);
                        std::vector<uint8_t> reply(bytes.begin(),bytes.end());reply[0]=1;reply[3]=12;
                        reply.insert(reply.end(),{0,0x20,0,8,0,1,uint8_t((port>>8)^0x21),uint8_t(port^0x12),uint8_t(127^0x21),0x12,0xa4,uint8_t(1^0x42)});
                        if(!transactions[index]) {
                            transactions[index]=id;
                            auto bad=reply;bad[8]^=1;send(*sockets[index],source,bad);
                            bad=reply;bad[3]=11;send(*sockets[index],source,bad);
                            (index==0 ? host_invalid : client_invalid)=true;
                        } else send(*sockets[index],source,reply);
                    } else if(index==2) {
                        remember(client,source,client_port);
                        if(!open)continue;
                        const bool stun=bytes.size()>=8 && bytes[4]==0x21 && bytes[5]==0x12 && bytes[6]==0xa4 && bytes[7]==0x42;
                        if(!stun && !dropped.exchange(true))continue;
                        CHECK(host.sin_port!=0);send(client_public,host,bytes);
                    } else {
                        remember(host,source,host_port);open=true;
                        CHECK(client.sin_port!=0);send(host_public,client,bytes);
                    }
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch(const std::exception& error){std::fprintf(stderr,"Fake NAT failed: %s\n",error.what());healthy=false;}
    }
};

template<class F> void until(F predicate,std::source_location caller=std::source_location::current()) {
    const auto end=std::chrono::steady_clock::now()+10s;
    while(!predicate() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(2ms);
    if(!predicate()){std::fprintf(stderr,"NAT wait timed out at line %u\n",caller.line());CHECK(false);}
}
void run_tests() {
    auto parsed=parse_peer_address(" 1.2.3.4:27888 ");
    CHECK(parsed && parsed->address=="1.2.3.4" && parsed->port==27888);
    for(auto value:{"1.2.3.4","1.2.3.4:0","1.2.3.4:65536","1.2.3.4:-1","1.2.3.4:1x","1.2.3.4:1:2","localhost:1","0.0.0.0:1","255.255.255.255:1","224.0.0.1:1"})CHECK(!parse_peer_address(value));
    CHECK(!parse_peer_address(std::string("1.2.3.4:80\0bad",16)));
    const std::array<uint8_t,12> id{1,2,3,4,5,6,7,8,9,10,11,12};
    auto request=stun_request(id);CHECK(request.size()==20 && request[1]==1);
    std::vector<uint8_t> response(request.begin(),request.end());
    response[0]=1;response[3]=12;
    // XOR-MAPPED-ADDRESS = 1.2.3.4:5000.
    response.insert(response.end(),{0,0x20,0,8,0,1,uint8_t(0x13^0x21),uint8_t(0x88^0x12),uint8_t(1^0x21),uint8_t(2^0x12),uint8_t(3^0xa4),uint8_t(4^0x42)});
    auto mapped=stun_response(response,id);CHECK(mapped && mapped->address=="1.2.3.4" && mapped->port==5000);
    auto bad=response;bad[8]^=1;CHECK(!stun_response(bad,id));
    bad=response;bad[4]^=1;CHECK(!stun_response(bad,id));
    bad=response;bad.pop_back();CHECK(!stun_response(bad,id));
    bad=response;bad[23]=12;CHECK(!stun_response(bad,id));
    bad=response;bad[3]=11;CHECK(!stun_response(bad,id));
    bad=response;bad.insert(bad.end(),{0,1,0,100});bad[3]=16;CHECK(!stun_response(bad,id));

    FakeNat nat;
    NatTunnel host({true,"127.0.0.1",0},{nat.host_stun.endpoint()});
    until([&]{return host.state().local.port!=0;});
    NatTunnel client({false,"127.0.0.1",ntohs(nat.host_public.address.sin_port)},{nat.client_stun.endpoint()},50ms);
    until([&]{return nat.host_invalid && nat.client_invalid;});
    std::this_thread::sleep_for(100ms);
    CHECK(host.state().public_address.empty() && client.state().public_address.empty());
    until([&]{return !host.state().public_address.empty() && !client.state().public_address.empty();});
    CHECK(host.state().public_address==nat.host_public.endpoint());
    CHECK(client.state().public_address==nat.client_public.endpoint());
    CHECK(host.state().bound_port==nat.host_port && client.state().bound_port==nat.client_port);
    CHECK(host.state().bound_port!=ntohs(nat.host_public.address.sin_port));
    CHECK(client.state().bound_port!=ntohs(nat.client_public.address.sin_port));
    CHECK(client.state().phase==TunnelPhase::needs_punch && host.state().phase==TunnelPhase::waiting);
    const auto host_bound=host.state().bound_port,client_bound=client.state().bound_port;
    host.punch(client.state().public_address);
    until([&]{return client.state().phase==TunnelPhase::connected && host.state().phase==TunnelPhase::connected;});
    CHECK(nat.healthy && nat.dropped);
    CHECK(host.state().bound_port==host_bound && client.state().bound_port==client_bound);
    const auto public_port=client.state().bound_port;
    const auto a=host.state().local,b=client.state().local;
    {
        Lobby left(a),right(b);
        until([&]{return left.state().phase==LobbyPhase::connected && right.state().phase==LobbyPhase::connected;});
        left.ready(true);right.ready(true);
        until([&]{return left.state().phase==LobbyPhase::launching && right.state().phase==LobbyPhase::launching;});
        CHECK(left.take_start()->port==a.port && right.take_start()->port==b.port);
    }
    // The same tunnel must carry the game's fresh TCP connection after the lobby closes.
    auto peer=std::async(std::launch::async,[&] {
        Socket s(a,{},5s);
        for(int i=0;i<32;++i){auto bytes=s.receive();CHECK(bytes==std::vector<uint8_t>(max_packet,uint8_t(i)));s.send(bytes);}
    });
    {
        Socket s(b,{},5s);
        for(int i=0;i<32;++i){s.send(std::vector<uint8_t>(max_packet,uint8_t(i)));CHECK(s.receive()==std::vector<uint8_t>(max_packet,uint8_t(i)));}
    }
    peer.get();
    CHECK(client.state().bound_port==public_port);
    std::array<uint8_t,16> session{42};
    Datagram left(a,session),right(b,session);
    std::optional<std::vector<uint8_t>> received;
    // Input datagrams are deliberately unreliable; the game repeats its input history.
    until([&]{if(!received){right.send(std::vector<uint8_t>{1,2,3});received=left.take();}return bool(received);});
    CHECK(*received==std::vector<uint8_t>({1,2,3}));
    received.reset();
    until([&]{if(!received){left.send(std::vector<uint8_t>{4,5,6});received=right.take();}return bool(received);});
    CHECK(*received==std::vector<uint8_t>({4,5,6}));
    CHECK(nat.healthy);

    const auto before=std::chrono::steady_clock::now();
    {
        NatTunnel waiting({false,"127.0.0.1",9},{},50ms);
        until([&]{return waiting.state().phase==TunnelPhase::needs_punch;});
        const auto port=waiting.state().bound_port;
        std::this_thread::sleep_for(100ms);CHECK(waiting.state().bound_port==port);
    }
    CHECK(std::chrono::steady_clock::now()-before<1s);
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--bridge") {
            NatTunnel host({true,"127.0.0.1",0},{});
            until([&]{return host.state().bound_port!=0;});
            NatTunnel client({false,"127.0.0.1",host.state().bound_port},{});
            until([&]{return client.state().phase==TunnelPhase::connected && host.state().phase==TunnelPhase::connected;});
            {
                Lobby left(host.state().local),right(client.state().local);
                until([&]{return left.state().phase==LobbyPhase::connected && right.state().phase==LobbyPhase::connected;});
                left.ready(true);right.ready(true);
                until([&]{return left.state().phase==LobbyPhase::launching;});
                until([&]{return right.state().phase==LobbyPhase::launching;});
            }
            std::cout<<host.state().local.port<<' '<<client.state().local.port<<std::endl;
            std::string quit;std::getline(std::cin,quit);
            CHECK(host.state().phase==TunnelPhase::connected && client.state().phase==TunnelPhase::connected);
        } else if(argc==2 && std::string_view(argv[1])=="--stun") {
            NatTunnel host({true,"127.0.0.1",0});
            until([&]{return !host.state().public_address.empty() || !host.state().discovery_error.empty() || host.state().phase==TunnelPhase::failed;});
            auto state=host.state();
            std::cout<<"STUN "<<(state.public_address.empty() ? "unavailable" : "mapped")<<", symmetric="<<state.symmetric<<", error="<<state.error<<state.discovery_error<<std::endl;
            return state.public_address.empty() ? 3 : 0;
        } else {CHECK(argc==1);run_tests();}
        return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"NAT test failed: %s\n",error.what());return 1;}
}
