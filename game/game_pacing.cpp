#include "game_pacing.h"
#include "cpu_sampler.h"
#include "counter_probe.h"
#include "state_dump.h"
#include "write_trap.h"
#include "dispatch.h"
#include "gx_vertices.h"
#include "clock.h"
#include "hle/os.h"
#include "native_input.h"
#include <atomic>
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
namespace {
constexpr uint32_t kClock=0x804D3318;
bool unlock_60fps; // Initialized before scheduler startup; guest baton owns later access.
uint32_t game_hz;  // same ownership
std::atomic<uint64_t> requested_mode{0};   // game_pacing_set_mode: 1<<63 | unlock<<32 | hz, 0 = none pending
std::atomic<bool> paused{false};
uint64_t deadline_ticks,last_boundary_ticks;
// Experimental --game-hz: the game clock waits for a wall-clock (virtual clock) deadline instead of a VI retrace and gets the real elapsed time as its timestep.
// shortcut: coarse scheduler sleep (1 ms granularity) plus a 2 ms spin; replace with a waitable timer if the spin shows up in CPU use.
void timed_wait() {
    uint64_t period=uc::kTimebaseHz/game_hz;
    // Deterministic runs have a virtual clock that cannot run faster: the test speed factor (x8 to skip to a scene) gives each step a longer timestep instead, i.e. fewer steps per game second (at most 50 ms, the clamp of timed_step).
    if(uc::deterministic_clock()) period=std::min<uint64_t>(period*uc::input_demo_speed(uc::clock_speed()),uc::ms_to_ticks(50));
    const uint64_t now=uc::now_ticks(),spin=uc::ms_to_ticks(2);
    deadline_ticks=std::max(deadline_ticks+period,now);   // a late frame starts the next one at once instead of waiting for the next slot
    if(deadline_ticks>now+spin) uc::os_sched().sleep_ticks(deadline_ticks-spin-now);
    while(uc::now_ticks()<deadline_ticks) {}
}
float timed_step() {
    const uint64_t now=uc::now_ticks();
    const double step=last_boundary_ticks ? double(now-last_boundary_ticks)/uc::kTimebaseHz : 1.0/game_hz;
    last_boundary_ticks=now;
    return float(std::clamp(step,0.001,0.05));
}
float step_scale=1.f;   // real step / nominal frame of the clock; guest baton owns it
std::atomic<uint64_t> calls{0},retraces{0},timestep_ns{0},threshold_two{0},adjusted{0},wait_ns{0};
// Fraction of a nominal frame carried per scaled counter word (key: its guest address); guest baton owns it.
// shortcut: entries are never freed (a few hundred objects, 4 bytes each); clear on game_pacing_init.
std::unordered_map<uint32_t,float> owed_frames;
}
namespace uc {
void game_pacing_init(bool unlock,uint32_t hz) {
    unlock_60fps=unlock; game_hz=hz; deadline_ticks=last_boundary_ticks=0; calls=retraces=timestep_ns=threshold_two=adjusted=wait_ns=0;
    step_scale=1.f; owed_frames.clear(); requested_mode=0;
}
PacingMode pacing_for_limit(std::optional<uint32_t> fps_limit,uint32_t game_hz_override,bool unlock_60fps) {
    if(game_hz_override) return {unlock_60fps,game_hz_override};
    if(!fps_limit) return {unlock_60fps,0};
    const uint32_t limit=*fps_limit;
    if(limit==0 || limit>60) return {unlock_60fps,limit ? std::min(limit,480u) : 480u};
    return {limit>30,0};
}
void game_pacing_set_mode(PacingMode mode) { requested_mode=(uint64_t(1)<<63)|(uint64_t(mode.unlock_60)<<32)|mode.game_hz; }
void game_pacing_set_paused(bool on) { paused=on; }
bool game_pacing_paused() { return paused.load(); }
float game_step_scale() { return step_scale; }
GamePacingStats game_pacing_stats() { return {calls.load(),retraces.load(),timestep_ns.load(),threshold_two.load(),adjusted.load(),wait_ns.load()}; }
}
void hle_GameFrameWait(ppc::Ctx& c) {
    uc::cpu_sampler_start_once();
    const auto object=c.r[3],stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,0x30); memory.read(stack-32,40);
    // Native lowering of prologue/wait;80158208 keeps original counter/timestep/return math.
    ppc::wr32(c,stack-32,stack); ppc::wr32(c,stack+4,c.lr); ppc::wr32(c,stack-4,c.r[31]);
    c.r[1]=stack-32; c.r[31]=object;
    if(object==kClock) if(const uint64_t mode=requested_mode.exchange(0)) {   // a new frame rate limit from the in-game menu
        unlock_60fps=(mode>>32)&1; game_hz=uint32_t(mode);
        deadline_ticks=last_boundary_ticks=0;
    }
    uint32_t target; bool change;
    const auto ready=[&] {
        c.r[3]=ppc::rd32(c,object+0x20); c.r[0]=target=ppc::rd32(c,object+0x28);
        change=unlock_60fps && object==kClock && target==2;
        return int32_t(c.r[3])>=int32_t(change ? 1 : target);
    };
    const bool elapsed=ready();
    const bool timed=game_hz && object==kClock;
    if(timed) {
        const auto wait_start=std::chrono::steady_clock::now();
        timed_wait();
        wait_ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wait_start).count());
        ppc::wr32(c,object+0x20,1); c.r[3]=1; change=false;   // the tail below sees exactly one elapsed step
    }
    // Original mode retains its mandatory wait; native60 avoids waiting twice after GPU work.
    else if(!change || !elapsed) {
        const auto wait_start=std::chrono::steady_clock::now();
        do {
            c.lr=0x801581F8; uc::call_indirect(c,0x8026E194);
        } while(!ready());
        if(object==kClock) wait_ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wait_start).count());
    }
    const bool native_step=timed || change;   // the game steps by something other than its nominal frame
    c.lr=0x801581F8;
    uc::call_indirect(c,0x80158208);
    if(timed) {
        const float step=timed_step(); uint32_t bits; std::memcpy(&bits,&step,4);
        ppc::wr32(c,object+4,bits); ppc::wr32(c,object+8,bits); ppc::wr32(c,object+0x20,0);   // callback increments during the frame are ignored
    }
    // Pause: the game clock steps by zero; the game keeps looping and rendering a still picture (probe: identical frames, actors, camera).
    const bool zero_step=object==kClock && paused.load(std::memory_order_relaxed);
    if(zero_step) { ppc::wr32(c,object+4,0); ppc::wr32(c,object+8,0); }
    if(object==kClock) {
        const float step=ppc::rdf32(c,object+8);
        if(!std::isfinite(step) || step<0 || step>1) throw std::invalid_argument("invalid native game timestep");
        {   // Systems that advance by a fixed amount per call assume one call per nominal frame (target / rate); see hle_ActorStepTime.
            const float rate=ppc::rdf32(c,object+0x1C);
            step_scale=1.f;
            if(native_step && std::isfinite(rate) && rate>0 && target>0) step_scale=std::clamp(step/(float(target)/rate),0.001f,20.f);
            if(zero_step) step_scale=0.f;
        }
        uc::counter_probe_step(c,step);
        uc::state_dump_step(c,step);
        uc::write_trap_step(c);
        ++calls; retraces+=ppc::rd32(c,object+0x24);
        timestep_ns+=uint64_t(std::llround(double(step)*1e9));
        threshold_two+=target==2;
        adjusted+=change;
    }
}
// Animated models already return elapsed time here; scaling again slows AI relative to animation. Only enforce the host pause.
void hle_ActorStepTime(ppc::Ctx& c) {
    const auto stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(stack-48,56);
    ppc::wr32(c,stack-48,stack); c.r[1]=stack-48;
    uc::call_indirect(c,0x800C7910);
    if(uc::game_step_scale()==0) c.f[1]=c.ps1[1]=0;
}
// The plain model getter supplies a per-call playback rate; normalize only its use in the actor timestep calculation.
void hle_FixedPlaybackStep(ppc::Ctx& c) {
    ppc::op_lfs(c,1,3,16);
    if(c.lr==0x800C799C) c.f[1]=c.ps1[1]=double(float(c.f[1])*uc::game_step_scale());
}
namespace {
// Runs the AOT body (after the lowered stwu of `frame` bytes) with the caller's stack and link register.
void call_body(ppc::Ctx& c,uint32_t frame,uint32_t entry) {
    const auto stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(stack-frame,frame+8);
    ppc::wr32(c,stack-frame,stack); c.r[1]=stack-frame;
    uc::call_indirect(c,entry);
}
}
// ODE's frame caller supplies a fixed 2/rate seconds. Scale before its four solver substeps so rigid bodies follow elapsed game time.
void hle_PhysicsStep(ppc::Ctx& c) {
    c.f[1]=double(float(c.f[1])*uc::game_step_scale());
    if(c.f[1]==0) return; // ODE requires a strictly positive timestep; pause leaves bodies untouched.
    call_body(c,80,0x801172A8);
}
// Hit events add force for one solver substep; keep their impulse constant as the substep shrinks.
void hle_PhysicsHit(ppc::Ctx& c) {
    const float scale=uc::game_step_scale();
    if(scale==0) { c.r[3]=0; return; }
    c.f[1]=double(float(c.f[1])/scale);
    call_body(c,32,0x80117468);
}
// Original 80210278: damped spring simulation whose damping, spring constant and age counter are per call (the train chandelier uses the separate ODE solver above),
// with no timestep. Above the 60 Hz step it runs once per 60 Hz step (a body that is not due keeps its state; the caller ignores the return value), so the swing lasts as long as at 60 Hz.
void hle_PropStep(ppc::Ctx& c) {
    const auto body=c.r[3],lr=c.lr;
    const float due=2.f*uc::game_step_scale();   // 1 at the 60 Hz step (nominal frame 1/29.97 s), 0.5 at 120 Hz, 2 in the original 30 Hz mode
    if(due<0.999f) {
        auto [slot,fresh]=owed_frames.try_emplace(body|1u,1.f);   // a new body runs at once; bit 0 keeps the key apart from the HUD counters
        float& owed=slot->second;
        owed+=due;
        if(owed<0.999f) { c.r[3]=ppc::rd32(c,body+84); return; }   // not due: same return value as the body (the age counter)
        owed=std::min(owed-1.f,1.f);
    }
    call_body(c,96,0x8021027C);
    static std::FILE* probe=[] { const char* path=std::getenv("UC_PROP_PROBE"); return path ? std::fopen(path,"w") : nullptr; }();   // diagnostic: body state after every executed step
    if(probe) {
        std::fprintf(probe,"S %llu %08X |",(unsigned long long)uc::game_pacing_stats().calls,body);
        for(uint32_t off=12;off<=80;off+=4) std::fprintf(probe," %.4g",ppc::rdf32(c,body+off));
        std::fputc(10,probe);
    }
    c.lr=lr;
}
// Original 8014D368: player HUD update. A counter (+7752) ticks once per call up to 10; on the 10th call the player names are copied from the player records
// (and the counter goes to 11). A call that is not due (a step shorter than the nominal frame) must not tick it: the counter is parked at 11, which the body leaves alone,
// and restored afterwards. Without this the names were copied before the records exist at ~450 Hz and later ("no data" instead of the name).
void hle_HudNameDelay(ppc::Ctx& c) {
    const auto object=c.r[3];
    const auto lr=c.lr;
    const auto counter=ppc::rd32(c,object+7752);
    bool park=false;
    if(counter<10 && uc::game_step_scale()!=1.f) {
        float& owed=owed_frames[object+7752];
        owed+=uc::game_step_scale();
        if(owed>=1.f) owed-=1.f; else park=true;   // shortcut: at most one tick per call (a step longer than the nominal frame counts as one)
    }
    if(park) ppc::wr32(c,object+7752,11);
    call_body(c,400,0x8014D36C);
    if(park) ppc::wr32(c,object+7752,counter);
    c.lr=lr;
}
