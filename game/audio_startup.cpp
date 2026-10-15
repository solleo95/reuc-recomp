#include "audio_startup.h"
#include "clock.h"
#include "hle/os.h"
#include <atomic>
#include <cstdio>
namespace uc {
void audio_wait_ready(ppc::Ctx& c, uint32_t timeout_ms) {
    constexpr uint32_t manager=0x8045D4F0;
    // shortcut: readiness is latched after the first observation because only the startup race is known; re-check if the player is ever re-created.
    static std::atomic<bool> seen_ready{false};
    if(c.r[3]!=manager || !ppc::rd8(c,manager+0x16C) || seen_ready)return;
    const auto ready=[&]{ return ppc::rd32(c,manager+0x1710) && ppc::rd32(c,manager+0x1FA4)==2; };
    if(ready()) { seen_ready=true; return; }
    // shortcut: RBUE08's worker can outrun player initialization; replace with a native resource dependency when its loader is ported.
    const auto deadline=now_ticks()+ms_to_ticks(timeout_ms);
    std::fprintf(stderr,"[audio-startup] group %u waiting for player initialization\n",c.r[4]);
    while(!ready()) {
        if(!ppc::rd8(c,manager+0x16C))return;
        if(now_ticks()>=deadline) {
            std::fprintf(stderr,"[audio-startup] group %u: player not ready after %u ms, loading without it\n",c.r[4],timeout_ms);
            return;
        }
        os_sched().sleep_ticks(ms_to_ticks(1));
    }
    seen_ready=true;
    std::fprintf(stderr,"[audio-startup] group %u player ready\n",c.r[4]);
}
}
