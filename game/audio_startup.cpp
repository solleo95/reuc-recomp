#include "audio_startup.h"
#include "clock.h"
#include "hle/os.h"
#include <cstdio>
#include <stdexcept>
namespace uc {
void audio_wait_ready(ppc::Ctx& c) {
    constexpr uint32_t manager=0x8045D4F0;
    if(c.r[3]!=manager || !ppc::rd8(c,manager+0x16C))return;
    if(ppc::rd32(c,manager+0x1710))return;
    // shortcut: RBUE08's worker can outrun player initialization; replace with a native resource dependency when its loader is ported.
    const auto deadline=now_ticks()+ms_to_ticks(10000);
    std::fprintf(stderr,"[audio-startup] group %u waiting for player initialization\n",c.r[4]);
    while(!ppc::rd32(c,manager+0x1710) || ppc::rd32(c,manager+0x1FA4)!=2) {
        if(!ppc::rd8(c,manager+0x16C))return;
        if(now_ticks()>=deadline)throw std::runtime_error("audio group load timed out waiting for player initialization");
        os_sched().sleep_ticks(ms_to_ticks(1));
    }
    std::fprintf(stderr,"[audio-startup] group %u player ready\n",c.r[4]);
}
}
