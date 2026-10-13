#include "game_pacing.h"
#include "dispatch.h"
#include "gx_vertices.h"
#include <atomic>
#include <cmath>
#include <stdexcept>
namespace {
constexpr uint32_t kClock=0x804D3318;
bool unlock_60fps; // Initialized before scheduler startup; guest baton owns later access.
std::atomic<uint64_t> calls{0},retraces{0},timestep_ns{0},threshold_two{0},adjusted{0};
}
namespace uc {
void game_pacing_init(bool unlock) {
    unlock_60fps=unlock; calls=retraces=timestep_ns=threshold_two=adjusted=0;
}
GamePacingStats game_pacing_stats() { return {calls.load(),retraces.load(),timestep_ns.load(),threshold_two.load(),adjusted.load()}; }
}
void hle_GameFrameWait(ppc::Ctx& c) {
    const auto object=c.r[3],stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,0x30); memory.read(stack-32,40);
    // Native lowering of prologue/wait;80158208 keeps original counter/timestep/return math.
    ppc::wr32(c,stack-32,stack); ppc::wr32(c,stack+4,c.lr); ppc::wr32(c,stack-4,c.r[31]);
    c.r[1]=stack-32; c.r[31]=object;
    uint32_t target; bool change;
    const auto ready=[&] {
        c.r[3]=ppc::rd32(c,object+0x20); c.r[0]=target=ppc::rd32(c,object+0x28);
        change=unlock_60fps && object==kClock && target==2;
        return int32_t(c.r[3])>=int32_t(change ? 1 : target);
    };
    const bool elapsed=ready();
    // Original mode retains its mandatory wait; native60 avoids waiting twice after GPU work.
    if(!change || !elapsed) do {
        c.lr=0x801581F8; uc::call_indirect(c,0x8026E194);
    } while(!ready());
    c.lr=0x801581F8;
    uc::call_indirect(c,0x80158208);
    if(object==kClock) {
        const float step=ppc::rdf32(c,object+8);
        if(!std::isfinite(step) || step<0 || step>1) throw std::invalid_argument("invalid native game timestep");
        ++calls; retraces+=ppc::rd32(c,object+0x24);
        timestep_ns+=uint64_t(std::llround(double(step)*1e9));
        threshold_two+=target==2;
        adjusted+=change;
    }
}
