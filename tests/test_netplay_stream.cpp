#include "check.h"
#include "netplay_stream.h"
#include <windows.h>
#include <future>
#include <thread>
using namespace std::chrono_literals;
static void run() {
    const auto port=uint16_t(30000+GetCurrentProcessId()%20000);
    auto server=std::async(std::launch::async,[&] {
        std::stop_source stop;
        uc::net::Socket socket({true,"127.0.0.1",port},stop.get_token(),5s);
        uc::net::Stream stream(socket,stop);
        // More than a socket buffer: sending must progress while the other side hasn't replied.
        for(unsigned i=0;i<64;++i)stream.enqueue(std::vector<uint8_t>(uc::net::max_packet,uint8_t(i)));
        for(unsigned i=0;i<64;++i)CHECK(stream.wait()==std::vector<uint8_t>({uint8_t(i)}));
        stream.enqueue({123});
        CHECK(stream.wait()==std::vector<uint8_t>({124}));
    });
    {
        std::stop_source stop;
        uc::net::Socket socket({false,"127.0.0.1",port},stop.get_token(),5s);
        uc::net::Stream stream(socket,stop);
        for(unsigned i=0;i<64;++i)CHECK(stream.wait()==std::vector<uint8_t>(uc::net::max_packet,uint8_t(i)));
        for(unsigned i=0;i<64;++i)stream.enqueue({uint8_t(i)});
        CHECK(stream.wait()==std::vector<uint8_t>({123}));stream.enqueue({124});
        server.get();
    }
    std::promise<void> connected;
    auto idle=std::async(std::launch::async,[&] {
        std::stop_source token;
        uc::net::Socket socket({true,"127.0.0.1",uint16_t(port+3)},token.get_token(),2s);
        uc::net::Stream stream(socket,token,{0});
        CHECK(stream.wait()==std::vector<uint8_t>({7}));
        stream.enqueue({8});
        CHECK(stream.wait()==std::vector<uint8_t>({9}));
    });
    {
        std::stop_source token;
        uc::net::Socket socket({false,"127.0.0.1",uint16_t(port+3)},token.get_token(),2s);
        uc::net::Stream stream(socket,token,{0});
        // The game can be loading: control liveness must not depend on guest frames.
        std::this_thread::sleep_for(3s);
        CHECK(stream.error().empty()); CHECK(!stream.take());
        stream.enqueue({7}); CHECK(stream.wait()==std::vector<uint8_t>({8})); stream.enqueue({9});
        idle.get();
    }
    std::stop_source stop;
    auto blocked=std::async(std::launch::async,[&] {
        uc::net::Socket socket({true,"127.0.0.1",uint16_t(port+1)},stop.get_token(),5s);
        uc::net::Stream stream(socket,stop);connected.set_value();
        try{stream.wait();return false;}catch(const std::exception&){return true;}
    });
    {
        uc::net::Socket socket({false,"127.0.0.1",uint16_t(port+1)},{},5s);
        connected.get_future().wait();const auto began=std::chrono::steady_clock::now();stop.request_stop();
        CHECK(blocked.get());CHECK(std::chrono::steady_clock::now()-began<1s);
    }
    auto overflow=std::async(std::launch::async,[&] {
        std::stop_source token;
        uc::net::Socket socket({true,"127.0.0.1",uint16_t(port+2)},token.get_token(),5s);
        uc::net::Stream stream(socket,token);
        const auto until=std::chrono::steady_clock::now()+3s;
        while(stream.error().empty() && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
        CHECK(stream.error().find("queue full")!=std::string::npos);
        bool rejected=false;try{stream.take();}catch(const std::exception&){rejected=true;}CHECK(rejected);
    });
    {
        uc::net::Socket socket({false,"127.0.0.1",uint16_t(port+2)},{},5s);
        for(unsigned i=0;i<129;++i)socket.send(std::vector<uint8_t>{1});
        overflow.get();
    }
}
int main(){try{run();return 0;}catch(const std::exception& e){std::fprintf(stderr,"stream: %s\n",e.what());return 1;}}
