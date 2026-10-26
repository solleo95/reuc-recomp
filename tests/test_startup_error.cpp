#include "check.h"
#include "startup_error.h"
int main() {
    uc::Options options;
    const auto logs=std::filesystem::path(L"C:/Игры/Umbrella/logs/game-123");
    const auto message=uc::startup_error_message(options,"Vulkan device initialization failed",logs);
    CHECK(message && message->find("Vulkan device initialization failed")!=std::string::npos);
    CHECK(message->find("game-123")!=std::string::npos);
    const auto utf8=logs.u8string();
    CHECK(message->find(std::string(utf8.begin(),utf8.end()))!=std::string::npos);
    const auto without_logs=uc::startup_error_message(options,"Vulkan device initialization failed",{});   // the public release writes no logs
    CHECK(without_logs && without_logs->find("Vulkan device initialization failed")!=std::string::npos && without_logs->find("Diagnostic logs")==std::string::npos);
    for(int mode=0;mode<6;++mode) {
        options={};
        if(mode==0)options.features=true;
        if(mode==1)options.test_server="regression";
        if(mode==2)options.test_control=L"control.txt";
        if(mode==3)options.headless=true;
        if(mode==4)options.demo_play=L"fight.demo";
        if(mode==5)options.seconds=1;
        CHECK(!uc::startup_error_message(options,"failure",logs));
    }
}
