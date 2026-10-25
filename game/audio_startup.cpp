#include "audio_startup.h"
#include "clock.h"
#include "hle/os.h"
#include <atomic>
#include <cstdio>
#include <map>
namespace uc {
struct AudioStartupSnapshot { std::map<int,uint64_t> deadlines; };
static AudioStartupSnapshot startup;
std::shared_ptr<const AudioStartupSnapshot> audio_startup_checkpoint() { return std::make_shared<AudioStartupSnapshot>(startup); }
void audio_startup_restore(const AudioStartupSnapshot& saved) { startup=saved; }
void audio_wait_ready(ppc::Ctx& c, uint32_t timeout_ms) {
    constexpr uint32_t manager=0x8045D4F0;
    if(c.r[3]!=manager || !ppc::rd8(c,manager+0x16C))return;
    const auto ready=[&]{ return ppc::rd32(c,manager+0x1710) && ppc::rd32(c,manager+0x1FA4)==2; };
    if(ready())return;
    // shortcut: RBUE08's worker can outrun player initialization; replace with a native resource dependency when its loader is ported.
    const auto id=os_sched().current()->id;
    const auto deadline=startup.deadlines.try_emplace(id,now_ticks()+ms_to_ticks(timeout_ms)).first->second;
    struct Clear { int id; ~Clear() { startup.deadlines.erase(id); } } clear{id};
    // This hook precedes the guest prologue: retry its entry with the original caller LR.
    PendingHle pending(os_sched(),c,nullptr,0x80049D64);
    std::fprintf(stderr,"[audio-startup] group %u waiting for player initialization\n",c.r[4]);
    while(!ready()) {
        if(!ppc::rd8(c,manager+0x16C))return;
        if(now_ticks()>=deadline) {
            std::fprintf(stderr,"[audio-startup] group %u: player not ready after %u ms, loading without it\n",c.r[4],timeout_ms);
            return;
        }
        os_sched().sleep_ticks(ms_to_ticks(1));
    }
    std::fprintf(stderr,"[audio-startup] group %u player ready\n",c.r[4]);
}
}
