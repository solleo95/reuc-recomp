#include "check.h"
#include "netplay_datagram.h"
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
using namespace std::chrono_literals;
using uc::net::Datagram;
std::vector<uint8_t> receive(Datagram& channel) {
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(std::chrono::steady_clock::now()<deadline) {
        if(auto packet=channel.take()) return *packet;
        std::this_thread::sleep_for(1ms);
    }
    CHECK(false); return {};
}
int main() {
    const auto port=uint16_t(30000+GetCurrentProcessId()%20000);
    const uc::net::Endpoint host{true,"127.0.0.1",port},client{false,"127.0.0.1",port};
    std::array<uint8_t,16> token{}; token[0]=42; token[15]=71;
    std::stop_source stop;
    Datagram a(host,token,stop.get_token()),b(client,token,stop.get_token());
    auto wrong=token; ++wrong[0]; Datagram intruder(client,wrong,{});
    intruder.send(std::vector<uint8_t>{9});
    b.send(std::vector<uint8_t>{1,2,3});
    CHECK(receive(a)==std::vector<uint8_t>({1,2,3}));
    a.send(std::vector<uint8_t>{4,5}); CHECK(receive(b)==std::vector<uint8_t>({4,5}));
    Datagram other_source(client,token,{});
    other_source.send(std::vector<uint8_t>{8});
    const std::vector<uint8_t> full(uc::net::max_datagram_payload,7);
    b.send(full); CHECK(receive(a)==full);
    CHECK(!a.take());
    bool rejected=false;
    try { b.send(std::vector<uint8_t>(uc::net::max_datagram_payload+1)); } catch(const std::runtime_error&) { rejected=true; }
    CHECK(rejected);
    rejected=false; try { a.send({}); } catch(const std::runtime_error&) { rejected=true; } CHECK(rejected);
    stop.request_stop();
    rejected=false; try { a.take(); } catch(const std::runtime_error&) { rejected=true; } CHECK(rejected);
}
