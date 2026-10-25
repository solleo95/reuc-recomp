#include "check.h"
#include "netplay.h"
#include <future>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using namespace std::chrono_literals;
using uc::net::Endpoint;
using uc::net::Socket;
using uc::net::Lobby;
using uc::net::LobbyPhase;
template<class Predicate> void until(Predicate predicate) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while(!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(5ms);
    CHECK(predicate());
}
void malformed_packet(uint16_t port, std::vector<uint8_t> bytes, const char* expected) {
    auto peer=std::async(std::launch::async,[=] {
        try { Socket socket({true,"127.0.0.1",port},{},2s); socket.receive(); return std::string{}; }
        catch (const std::exception& error) { return std::string(error.what()); }
    });
    SOCKET raw=INVALID_SOCKET;
    const auto end=std::chrono::steady_clock::now()+2s;
    do {
        raw=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); CHECK(raw!=INVALID_SOCKET);
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port);
        InetPtonA(AF_INET,"127.0.0.1",&address.sin_addr);
        if(connect(raw,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0) break;
        closesocket(raw); raw=INVALID_SOCKET;
        std::this_thread::sleep_for(5ms);
    } while(std::chrono::steady_clock::now()<end);
    CHECK(raw!=INVALID_SOCKET);
    for(uint8_t byte:bytes) CHECK(send(raw,reinterpret_cast<const char*>(&byte),1,0)==1);
    shutdown(raw,SD_SEND); closesocket(raw);
    CHECK(peer.get().find(expected)!=std::string::npos);
}
int main() {
    CHECK(uc::net::valid_address("127.0.0.1"));
    CHECK(!uc::net::valid_address("999.2.3.4"));
    CHECK(!uc::net::valid_address("127.0.0.1 --bad"));
    const auto port=uint16_t(30000+GetCurrentProcessId()%20000);
    uc::net::Endpoint host{true,"127.0.0.1",port},client{false,"127.0.0.1",port};
    auto peer=std::async(std::launch::async,[&] {
        uc::net::Socket s(host,{},5s);
        auto data=s.receive(); CHECK(data==std::vector<uint8_t>({1,2,3}));
        s.send(data);
        auto full=s.receive(); CHECK(full==std::vector<uint8_t>(uc::net::max_packet,42)); s.send(full);
    });
    {
        uc::net::Socket s(client,{},5s); s.send(std::vector<uint8_t>{1,2,3}); CHECK(s.receive()==std::vector<uint8_t>({1,2,3}));
        bool rejected=false; try { s.send({}); } catch(const std::exception&) { rejected=true; } CHECK(rejected);
        rejected=false; try { s.send(std::vector<uint8_t>(uc::net::max_packet+1)); } catch(const std::exception&) { rejected=true; } CHECK(rejected);
        s.send(std::vector<uint8_t>(uc::net::max_packet,42)); CHECK(s.receive()==std::vector<uint8_t>(uc::net::max_packet,42));
    }
    peer.get();
    ++host.port; ++client.port;
    uc::net::Lobby a(host),b(client);
    until([&]{return a.state().phase==uc::net::LobbyPhase::connected && b.state().phase==uc::net::LobbyPhase::connected;});
    a.ready(true);
    until([&]{return b.state().remote_ready;});
    CHECK(!a.take_start() && !b.take_start());
    a.ready(false); until([&]{return !b.state().remote_ready;});
    b.ready(true); until([&]{return a.state().remote_ready;});
    CHECK(!a.take_start() && !b.take_start());
    a.ready(true);
    b.ready(true);
    until([&]{return a.state().phase==uc::net::LobbyPhase::launching && b.state().phase==uc::net::LobbyPhase::launching;});
    CHECK(a.take_start().has_value() && b.take_start().has_value());
    CHECK(!a.take_start() && !b.take_start());
    std::stop_source cancel;
    auto blocked=std::async(std::launch::async,[&] { try { uc::net::Socket s({true,"127.0.0.1",uint16_t(port+2)},cancel.get_token(),5s); return false; } catch(const std::exception&) {return true;} });
    cancel.request_stop(); CHECK(blocked.wait_for(1s)==std::future_status::ready && blocked.get());
    std::stop_source connect_cancel;
    auto connecting=std::async(std::launch::async,[&] { try { Socket s({false,"127.0.0.1",uint16_t(port+3)},connect_cancel.get_token(),5s); return false; } catch(const std::exception&) {return true;} });
    std::this_thread::sleep_for(30ms); connect_cancel.request_stop();
    CHECK(connecting.wait_for(1s)==std::future_status::ready && connecting.get());
    std::stop_source receive_cancel;
    std::promise<void> accepted;
    auto receiving=std::async(std::launch::async,[&] {
        Socket socket({true,"127.0.0.1",uint16_t(port+4)},receive_cancel.get_token(),5s); accepted.set_value();
        try { socket.receive(); return false; } catch(const std::exception&) {return true;}
    });
    { Socket socket({false,"127.0.0.1",uint16_t(port+4)},{},5s); accepted.get_future().wait();
      receive_cancel.request_stop(); CHECK(receiving.wait_for(1s)==std::future_status::ready && receiving.get()); }
    malformed_packet(uint16_t(port+5),{'U','C',1,0,0,0,128,1},"length");
    malformed_packet(uint16_t(port+6),{'U','C',1,0,0,0,0,0},"length");
    malformed_packet(uint16_t(port+7),{'U','C',2,0,0,0,0,1},"version");
    malformed_packet(uint16_t(port+8),{'U','C',1,0,0,0,0,3,42},"disconnected");
    malformed_packet(uint16_t(port+9),{'U','C',1},"disconnected");
    malformed_packet(uint16_t(port+10),{},"disconnected");
    {
        Lobby waiting({true,"127.0.0.1",uint16_t(port+11)});
        { Lobby departing({false,"127.0.0.1",uint16_t(port+11)});
          until([&]{return waiting.state().phase==LobbyPhase::connected && departing.state().phase==LobbyPhase::connected;});
          waiting.ready(true); until([&]{return departing.state().remote_ready;}); }
        until([&]{return waiting.state().phase==LobbyPhase::failed;});
        CHECK(!waiting.take_start() && !waiting.state().error.empty());
    }
    const auto before=std::chrono::steady_clock::now();
    { Lobby waiting({true,"127.0.0.1",uint16_t(port+12)}); std::this_thread::sleep_for(30ms); }
    CHECK(std::chrono::steady_clock::now()-before<1s);
    // A new game must be able to bind the host's lobby port immediately.
    auto reused=std::async(std::launch::async,[&] { Socket socket(host,{},2s); socket.send(std::vector<uint8_t>{42}); });
    { Socket socket(client,{},2s); CHECK(socket.receive()==std::vector<uint8_t>({42})); }
    reused.get();
}
