#include "check.h"
#include "hitch_report.h"
int main() {
    uc::FrameRow before,normal;
    normal.interval_ms=16.7;
    CHECK(uc::hitch_report(normal,before,16.7).empty());
    CHECK(uc::hitch_report(normal,before,0).empty());

    uc::FrameRow gpu;gpu.interval_ms=60;gpu.gpu_sum_ms=55;gpu.thread_ms[0]=5;
    CHECK(uc::hitch_report(gpu,before,16.7).find("cause=gpu ")!=std::string::npos);

    uc::FrameRow guest;guest.interval_ms=50;guest.thread_ms[0]=20;guest.thread_ms[2]=22;guest.gpu_sum_ms=3;
    CHECK(uc::hitch_report(guest,before,16.7).find("cause=guest-cpu ")!=std::string::npos);

    uc::FrameRow disc;disc.interval_ms=120;disc.idle_ms=100;disc.dvd_reads=9;before.dvd_reads=4;
    const auto text=uc::hitch_report(disc,before,16.7);
    CHECK(text.find("cause=guest-idle ")!=std::string::npos && text.find("disc-reads+5")!=std::string::npos);

    uc::FrameRow other;other.interval_ms=80;other.gpu_sum_ms=4;other.thread_ms[0]=6;
    CHECK(uc::hitch_report(other,before,16.7).find("cause=unattributed")!=std::string::npos);

    uc::FrameRow restarted;restarted.interval_ms=40;restarted.textures=1;before.textures=500;   // a restarted renderer counts from zero
    CHECK(uc::hitch_report(restarted,before,16.7).find("textures+0")!=std::string::npos);
    return 0;
}
