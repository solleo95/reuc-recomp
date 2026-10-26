#include "check.h"
#include "network_session.h"
#include "netplay_datagram.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <source_location>
#include <future>
using namespace uc::net;
using namespace std::chrono_literals;
template<class F> void until(F predicate,std::source_location at=std::source_location::current()) {
    const auto end=std::chrono::steady_clock::now()+10s;
    while(!predicate()) {if(std::chrono::steady_clock::now()>=end){std::fprintf(stderr,"Network wait timed out at line %u\n",at.line());CHECK(false);}std::this_thread::sleep_for(2ms);}
}
struct Reservation {
    SOCKET socket=INVALID_SOCKET;
    uint16_t port=0;
    explicit Reservation(int type) {
        socket=::socket(AF_INET,type,0);CHECK(socket!=INVALID_SOCKET);
        const BOOL exclusive=TRUE;CHECK(setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);
        CHECK(bind(socket,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0);
        int size=sizeof(address);CHECK(getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size)==0);port=ntohs(address.sin_port);
    }
    ~Reservation(){closesocket(socket);}
};
int main() {
    WSADATA data{};CHECK(WSAStartup(MAKEWORD(2,2),&data)==0);
    auto local=parse_connection_address(" localhost:27888 ");
    CHECK(local && local->address=="127.0.0.1" && local->port==default_port);
    CHECK(parse_connection_address("192.168.50.170:12345"));
    for(auto bad:{"","127.0.0.1","1.2.3.4:0","1.2.3.4:65536","1.2.3.4:1x","localhost","localhost:0","0.0.0.0:2","224.0.0.1:2"})CHECK(!parse_connection_address(bad));
    CHECK(!parse_connection_address(std::string("127.0.0.1:123\0tail",18)));
    for(int occupied:{0,SOCK_STREAM,SOCK_DGRAM}) {
        std::unique_ptr<Reservation> reserved;
        if(occupied)reserved=std::make_unique<Reservation>(occupied);
        NetworkSession host({true,"0.0.0.0",reserved ? reserved->port : uint16_t(0)},{});
        until([&]{host.poll(false);CHECK(host.state().lobby.phase!=LobbyPhase::failed);return host.state().port!=0;});
        const auto port=host.state().port;
        CHECK(!reserved || port!=reserved->port);
        CHECK(!host.state().local_addresses.empty());
        NetworkSession client({false,"127.0.0.1",port},{});
        until([&]{host.poll(false);client.poll(true);return host.state().lobby.phase==LobbyPhase::connected && host.state().lobby.remote_ready;});
        CHECK(!host.take_start() && !client.take_start());
        CHECK(!host.state().tunneled && !client.state().tunneled);
        until([&]{host.poll(true);client.poll(true);return host.state().lobby.phase==LobbyPhase::launching && client.state().lobby.phase==LobbyPhase::launching;});
        const auto a=host.take_start(),b=client.take_start();
        CHECK(a && b && a->host && !b->host && a->port==port && b->port==port && !a->loopback && !b->loopback);
        CHECK(!host.take_start() && !client.take_start());
        std::array<uint8_t,16> session{1};
        Datagram game_host(*a,session),game_client(*b,session); // The tunnel relinquished public UDP before launch.
        std::optional<std::vector<uint8_t>> packet;
        until([&]{game_client.send(std::vector<uint8_t>{1,2,3});packet=game_host.take();return bool(packet);});
        CHECK(*packet==std::vector<uint8_t>({1,2,3}));
    }
    // The Internet lobby keeps its bridge across the game's fresh TCP connection.
    {
        NetworkSession host({true,"0.0.0.0",0},{});
        until([&]{host.poll(true);return host.state().port!=0;});
        NatTunnel client({false,"127.0.0.1",host.state().port},{});
        until([&]{host.poll(true);return client.state().phase==TunnelPhase::connected;});
        Lobby peer(client.state().local);peer.ready(true);
        until([&]{host.poll(true);CHECK(host.state().lobby.phase!=LobbyPhase::failed);return host.state().lobby.phase==LobbyPhase::launching && peer.state().phase==LobbyPhase::launching;});
        CHECK(host.state().tunneled);
        const auto a=host.take_start(),b=peer.take_start();
        CHECK(a && b && a->loopback && b->loopback);
        auto server=std::async(std::launch::async,[&]{Socket game(*a,{},5s);game.send(game.receive());});
        {Socket game(*b,{},5s);game.send(std::vector<uint8_t>{4,5,6});CHECK(game.receive()==std::vector<uint8_t>({4,5,6}));}
        server.get();
    }
    // Both lobbies can connect, but only one can commit a launch.
    {
        NetworkSession host({true,"0.0.0.0",0},{});
        until([&]{host.poll(false);return host.state().port!=0;});
        NatTunnel internet({false,"127.0.0.1",host.state().port},{});
        until([&]{return internet.state().phase==TunnelPhase::connected;});
        Lobby remote(internet.state().local),local({false,"127.0.0.1",host.state().port});
        until([&]{return remote.state().phase==LobbyPhase::connected && local.state().phase==LobbyPhase::connected;});
        remote.ready(true);local.ready(true);
        until([&]{host.poll(true);return host.state().lobby.phase==LobbyPhase::launching && local.state().phase==LobbyPhase::launching;});
        CHECK(!host.state().tunneled && host.take_start() && local.take_start());
        CHECK(!remote.take_start() && !host.take_start());
    }
    // If TCP is filtered, retry the same LAN endpoint through UDP after five seconds.
    {
        NatTunnel host({true,"0.0.0.0",0},{});
        until([&]{return host.state().local.port!=0;});
        Lobby peer(host.state().local);peer.ready(true);
        const auto started=std::chrono::steady_clock::now();
        NetworkSession client({false,"127.0.0.1",host.state().bound_port},{});
        until([&]{client.poll(true);CHECK(client.state().lobby.phase!=LobbyPhase::failed);return client.state().tunneled;});
        const auto elapsed=std::chrono::steady_clock::now()-started;
        CHECK(elapsed>=4500ms && elapsed<8s);
        until([&]{client.poll(true);return client.state().lobby.phase==LobbyPhase::launching && peer.state().phase==LobbyPhase::launching;});
        CHECK(client.take_start() && peer.take_start());
    }
    // Timeout can be published between poll's snapshots; every pending client must still retry UDP.
    {
        Reservation silent_udp(SOCK_DGRAM);
        std::vector<std::future<bool>> clients;
        for(unsigned i=0;i<16;++i)clients.push_back(std::async(std::launch::async,[port=silent_udp.port] {
            NetworkSession client({false,"127.0.0.1",port},{});
            const auto deadline=std::chrono::steady_clock::now()+8s;
            std::this_thread::sleep_for(4500ms);
            while(!client.state().tunneled) {
                client.poll(false);
                if(client.state().lobby.phase==LobbyPhase::failed) {
                    std::fprintf(stderr,"TCP fallback skipped: %s\n",client.state().lobby.error.c_str());return false;
                }
                if(std::chrono::steady_clock::now()>=deadline)return false;
            }
            return true;
        }));
        for(auto& client:clients)CHECK(client.get());
    }
    // An unconnected UDP route remains available for the host's punch action.
    {
        NetworkSession host({true,"0.0.0.0",0},{});
        until([&]{host.poll(false);return host.state().port!=0;});
        host.punch("127.0.0.1:9");
        until([&]{host.poll(false);return host.state().punching;});
    }
    // Cancel while both listeners are idle; the public ports must be reusable.
    uint16_t cancelled_port=0;
    {
        NetworkSession waiting({true,"0.0.0.0",0},{});
        until([&]{waiting.poll(false);return waiting.state().port!=0;});
        cancelled_port=waiting.state().port;
    }
    {NetworkSession replacement({true,"0.0.0.0",cancelled_port},{});
        until([&]{replacement.poll(false);return replacement.state().port!=0;});CHECK(replacement.state().port==cancelled_port);}
    WSACleanup();
}
