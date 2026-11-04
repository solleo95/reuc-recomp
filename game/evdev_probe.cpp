// Linux only, a developer tool: prints what the evdev layer sees for a few seconds, to check it against virtual devices (scripts/linux/uinput_fakes.py) or real ones.
//   uc_evdev_probe pointers <seconds> [grab]   mice and guns as PointerEvents (the first device that presses a button is grabbed with `grab`)
//   uc_evdev_probe remote <seconds>             the first Wii Remote's reports (buttons, accelerometer, IR, nunchuk) parsed back from the 0x37 report
#include "evdev_backend.h"
#include "wiimote_protocol.h"
#include "wiimote_transport.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace std::chrono;
int main(int argc,char** argv) {
    if(argc<3) { std::fprintf(stderr,"usage: uc_evdev_probe pointers|remote <seconds> [grab]\n"); return 2; }
    const auto until=steady_clock::now()+seconds(std::atoi(argv[2]));
    if(!std::strcmp(argv[1],"pointers")) {
        auto backend=uc::make_evdev_pointer_backend();
        const bool grab=argc>3 && !std::strcmp(argv[3],"grab");
        bool grabbed=false;
        while(steady_clock::now()<until) {
            std::vector<uc::PointerEvent> events; std::vector<uint64_t> removed;
            backend->poll(events,removed);
            for(const auto& e:events) {
                const char* kind=e.kind==uc::PointerEvent::Kind::button ? "button" : e.kind==uc::PointerEvent::Kind::relative ? "relative" : e.kind==uc::PointerEvent::Kind::absolute ? "absolute" : "wheel";
                std::printf("event device=%llu %s button=%u down=%d x=%.4f y=%.4f\n",(unsigned long long)e.device,kind,unsigned(e.button),int(e.down),e.x,e.y);
                if(grab && !grabbed && e.kind==uc::PointerEvent::Kind::button && e.down) { grabbed=backend->capture(e.device,true); std::printf("grab device=%llu -> %d\n",(unsigned long long)e.device,int(grabbed)); }
            }
            for(const auto id:removed) std::printf("removed device=%llu\n",(unsigned long long)id);
            const auto status=backend->status(); if(!status.empty()) std::printf("status: %s\n",status.c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(milliseconds(5));
        }
        return 0;
    }
    if(!std::strcmp(argv[1],"remote")) {
        std::string error; auto remote=uc::wii_open_first(error);
        if(!remote) { std::printf("no remote: %s\n",error.c_str()); return 1; }
        std::printf("remote id=%s\n",remote->id().c_str());
        const std::vector<uint8_t> status_request{0x15,0x00}; remote->write(status_request);
        const std::vector<uint8_t> led{0x11,0x20}; remote->write(led);   // player 2's LED
        uint32_t last_buttons=~0u;
        while(steady_clock::now()<until) {
            std::vector<uint8_t> report;
            if(!remote->read(report,20)) { std::printf("remote gone\n"); return 0; }
            uc::WiiState state;
            if(report.size()>0 && report[0]==0x20) { std::printf("status extension=%d\n",int((report.size()>3 && (report[3]&2)!=0))); continue; }
            if(!uc::wii_parse_report(report,state)) continue;
            if(state.buttons!=last_buttons) { last_buttons=state.buttons; std::printf("buttons=%04x accel=%u,%u,%u ir0=%d(%u,%u) nunchuk=%d stick=%u,%u\n",state.buttons,state.accel[0],state.accel[1],state.accel[2],int(state.ir[0].valid),state.ir[0].x,state.ir[0].y,int(state.has_nunchuk),state.stick[0],state.stick[1]); std::fflush(stdout); }
        }
        return 0;
    }
    return 2;
}
