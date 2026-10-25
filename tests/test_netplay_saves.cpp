#include "check.h"
#include "netplay_saves.h"
#include <windows.h>
#include <fstream>
#include <array>
#include <future>
#include <thread>
namespace fs=std::filesystem;
static std::vector<uint8_t> header(const std::string& name,uint32_t size=0) {
    std::vector<uint8_t> out{2,uint8_t(name.size())};
    out.insert(out.end(),name.begin(),name.end());
    for(int shift=24;shift>=0;shift-=8)out.push_back(uint8_t(size>>shift));
    return out;
}
static void run() {
    const auto base=fs::temp_directory_path()/("uc-net-save-test-"+std::to_string(GetCurrentProcessId()));
    CHECK(!fs::exists(base));fs::create_directories(base/"host"/"empty");fs::create_directory(base/"client");
    const std::string payload(70000,'x');
    {std::ofstream(base/"host"/"progress",std::ios::binary)<<payload;std::ofstream(base/"client"/"personal")<<"untouched";}
    uint16_t port=uint16_t(32000+GetCurrentProcessId()%20000);
    const auto connect=[&](auto send,auto receive) {
        const auto current=port++;
        auto host=std::async(std::launch::async,[&] {uc::net::Socket socket({true,"127.0.0.1",current},{},std::chrono::seconds(3));send(socket);});
        uc::net::Socket socket({false,"127.0.0.1",current},{},std::chrono::seconds(3));receive(socket);host.get();
    };
    fs::path temporary;
    {
        uc::NetSaves host(true,base/"host"),client(false,base/"client");temporary=client.root();
        CHECK(temporary!=base/"client");
        connect([&](auto& socket){host.transfer(socket);},[&](auto& socket){client.transfer(socket);});
        CHECK(fs::is_directory(temporary/"empty"));CHECK(fs::file_size(temporary/"progress")==payload.size());
        std::ifstream in(temporary/"progress",std::ios::binary);CHECK(std::string(std::istreambuf_iterator<char>(in),{})==payload);
        CHECK(!fs::exists(temporary/"personal"));CHECK(fs::exists(base/"client"/"personal"));
    }
    CHECK(!fs::exists(temporary));
    for(const auto& name:{"../escape","/absolute","C:/drive","a\\b","a:stream","CON","a/../b",".backups/file","trailing.","duplicate"}) {
        uc::NetSaves client(false,base/"client");bool rejected=false;
        connect([&](auto& socket){socket.send(header(name));if(std::string(name)=="duplicate")socket.send(header(name));socket.send(std::array<uint8_t,1>{0});},
            [&](auto& socket){try{client.transfer(socket);}catch(const std::exception&){rejected=true;}});
        CHECK(rejected);
    }
    {
        uc::NetSaves client(false,base/"client");bool rejected=false;
        connect([&](auto& socket){socket.send(header("huge",17u<<20));},[&](auto& socket){try{client.transfer(socket);}catch(const std::exception&){rejected=true;}});
        CHECK(rejected);
    }
    {
        uc::NetSaves client(false,base/"client");temporary=client.root();bool rejected=false;
        connect([&](auto& socket){socket.send(header("partial",100));socket.send(std::array<uint8_t,1>{42});},
            [&](auto& socket){try{client.transfer(socket);}catch(const std::exception&){rejected=true;}});
        CHECK(rejected);
    }
    CHECK(!fs::exists(temporary));
    {
        uc::NetSaves client(false,base/"client");bool rejected=false;
        connect([&](auto& socket){for(int i=0;i<257;++i)socket.send(header("file"+std::to_string(i)));},
            [&](auto& socket){try{client.transfer(socket);}catch(const std::exception&){rejected=true;}});
        CHECK(rejected);
    }
    CHECK(!fs::exists(base/"escape"));CHECK(fs::exists(base/"client"/"personal"));
    fs::remove_all(base);
}
int main() {try{run();return 0;}catch(const std::exception& error){std::fprintf(stderr,"net save test: %s\n",error.what());return 1;}}
