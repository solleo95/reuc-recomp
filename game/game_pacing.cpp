#include "game_pacing.h"
#include "hooks.h"
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
#include <bit>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
void hle_Leaf_80064C34(ppc::Ctx& c);
void hle_PSMTXIdentity(ppc::Ctx& c);
namespace {
constexpr uint32_t kClock=0x804D3318;
bool unlock_60fps; // Initialized before scheduler startup; guest baton owns later access.
uint32_t game_hz;  // same ownership
std::atomic<uint64_t> requested_mode{0};   // game_pacing_set_mode: 1<<63 | unlock<<32 | hz, 0 = none pending
std::atomic<bool> paused{false};
uint64_t deadline_ticks,last_boundary_ticks;
uint64_t network_period_ticks=0; // Guest baton owns network updates.
// Experimental --game-hz: the game clock waits for a wall-clock (virtual clock) deadline instead of a VI retrace and gets the real elapsed time as its timestep.
// shortcut: coarse scheduler sleep (1 ms granularity) plus a 2 ms spin; replace with a waitable timer if the spin shows up in CPU use.
void timed_wait() {
    uint64_t period=uc::kTimebaseHz/game_hz;
    // Deterministic runs have a virtual clock that cannot run faster: the test speed factor (x8 to skip to a scene) gives each step a longer timestep instead, i.e. fewer steps per game second (at most 50 ms, the clamp of timed_step).
    if(uc::deterministic_clock()) period=std::min<uint64_t>(period*uc::input_demo_speed(uc::clock_speed()),uc::ms_to_ticks(50));
    if(network_period_ticks)period=network_period_ticks;
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
double nominal_phase=0;
uint32_t nominal_ticks=1; // Shared sample boundaries; no object lifetime state.
std::atomic<uint64_t> calls{0},retraces{0},timestep_ns{0},threshold_two{0},adjusted{0},wait_ns{0};
// Fraction of a nominal frame carried per scaled counter word (key: its guest address); guest baton owns it.
// shortcut: entries are never freed (a few hundred objects, 4 bytes each); clear on game_pacing_init.
std::unordered_map<uint32_t,float> owed_frames;
struct SleepCarry { uint32_t counter=0,timer=0; double fraction=0; };
std::unordered_map<uint32_t,SleepCarry> sleep_carry; // Guest baton; erased on body destruction.
struct HomeInput { uint32_t source=0,type=0; std::array<uint32_t,4> edges{}; };
uint32_t home_object=0;
std::array<HomeInput,4> home_input{}; // Guest baton; reset by the menu initialization call.
}
namespace uc {
struct PacingSnapshot {
    bool unlock,paused;
    uint32_t hz;
    uint64_t requested,deadline,last_boundary,network_period;
    float scale;
    double phase;
    uint32_t ticks;
    decltype(owed_frames) owed;
    decltype(sleep_carry) carry;
    uint32_t home;
    decltype(home_input) input;
    GamePacingStats stats;
};
std::shared_ptr<const PacingSnapshot> game_pacing_checkpoint() {
    return std::make_shared<PacingSnapshot>(PacingSnapshot{unlock_60fps,paused.load(),game_hz,
        requested_mode.load(),deadline_ticks,last_boundary_ticks,network_period_ticks,step_scale,nominal_phase,
        nominal_ticks,owed_frames,sleep_carry,home_object,home_input,game_pacing_stats()});
}
void game_pacing_restore(const PacingSnapshot& saved) {
    unlock_60fps=saved.unlock; paused=saved.paused; game_hz=saved.hz; requested_mode=saved.requested;
    deadline_ticks=saved.deadline; last_boundary_ticks=saved.last_boundary; network_period_ticks=saved.network_period;
    step_scale=saved.scale; nominal_phase=saved.phase; nominal_ticks=saved.ticks; owed_frames=saved.owed;
    sleep_carry=saved.carry; home_object=saved.home; home_input=saved.input;
    calls=saved.stats.calls; retraces=saved.stats.retraces; timestep_ns=saved.stats.timestep_ns;
    threshold_two=saved.stats.threshold_two; adjusted=saved.stats.adjusted; wait_ns=saved.stats.wait_ns;
}
void game_pacing_init(bool unlock,uint32_t hz) {
    unlock_60fps=unlock; game_hz=hz; deadline_ticks=last_boundary_ticks=network_period_ticks=0; calls=retraces=timestep_ns=threshold_two=adjusted=wait_ns=0;
    step_scale=1.f; nominal_phase=0; nominal_ticks=1; owed_frames.clear(); sleep_carry.clear(); requested_mode=0;
    home_object=0; home_input={};
}
PacingMode pacing_for_limit(std::optional<uint32_t> fps_limit,uint32_t game_hz_override,bool unlock_60fps) {
    if(game_hz_override) return {unlock_60fps,game_hz_override};
    if(!fps_limit) return {unlock_60fps,0};
    const uint32_t limit=*fps_limit;
    if(limit==0 || limit>60) return {unlock_60fps,limit ? std::min(limit,480u) : 480u};
    return {limit>30,0};
}
void game_pacing_set_mode(PacingMode mode) { requested_mode=(uint64_t(1)<<63)|(uint64_t(mode.unlock_60)<<32)|mode.game_hz; }
void game_pacing_set_network_period(uint64_t ticks) {
    if(ticks && (ticks<kTimebaseHz/120 || ticks>kTimebaseHz/20))throw std::invalid_argument("network frame period out of range");
    network_period_ticks=ticks;
}
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
        nominal_phase+=step_scale;
        nominal_ticks=uint32_t(nominal_phase);
        nominal_phase-=nominal_ticks;
        // Sole readers: slow motion 800667B0, lighting 80140C4C, UV 80158C08, texture 80189034, history 801F2020.
        // Supply nominal-frame coefficients before guest updates; retain the original AOT math and wrap rules.
        ppc::wrf32(c,0x806C6158,step_scale==1.f ? 0.92f : std::pow(0.92f,step_scale));
        ppc::wrf32(c,0x806C6444,0.02f*step_scale);
        ppc::wrf64(c,0x806C6B50,0.008*step_scale);
        ppc::wrf32(c,0x806C8A18,0.2f*step_scale);
        ppc::wrf32(c,0x806C4EA8,0.12f*step_scale);
        // Texture layers 801897B8/80189814: scale both slope and intercept of each layer's scrolling speed.
        ppc::wrf32(c,0x806C6B90,0.00056f*step_scale); ppc::wrf32(c,0x806C6B8C,0.005f*step_scale);
        ppc::wrf32(c,0x806C6B98,0.001f*step_scale); ppc::wrf32(c,0x806C6B94,0.01f*step_scale);
        ppc::wrf32(c,0x806C9CF4,-98.f*step_scale); // Sole reader 8024488C: UI gravity before the existing dt-scaled movement.
        uc::counter_probe_step(c,step);
        uc::state_dump_step(c,step);
        uc::write_trap_step(c);
        if(!zero_step) uc::mods::mods_step(nominal_ticks,&c);   // mod scripts' on_step: once per nominal frame, on the guest thread
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
// 801C4438 scales bat displacement only in modes 16..20; the other modes assume one update per nominal frame.
void hle_FlightStep(ppc::Ctx& c) {
    const auto value=c.r[4],lr=c.lr;
    c.r[3]=0x804B5CC8; c.lr=0x801C445C;
    uc::call_indirect(c,0x801490FC);
    if(c.r[3]>=16 && c.r[3]<=20) {
        const float rate=ppc::rdf32(c,0x804D3318+28)*ppc::rdf32(c,c.r[2]-18452);
        const float distance=ppc::rdf32(c,value)*rate;
        ppc::wrf32(c,value,distance*ppc::rdf32(c,0x804D3318+4));
    } else ppc::wrf32(c,value,ppc::rdf32(c,value)*uc::game_step_scale());
    c.lr=lr;
}
// The falling bat's position uses a per-frame velocity; its later gravity addition already includes dt.
void hle_VectorAddStep(ppc::Ctx& c) {
    const float scale=uc::game_step_scale();
    if(c.lr!=0x801C387C || scale==1.f) { hle_Leaf_80064C34(c); return; }
    ppc::op_psq_l(c,0,4,0,0,0);
    ppc::op_psq_l(c,1,5,0,0,0);
    c.f[1]=double(float(c.f[1])*scale); c.ps1[1]=double(float(c.ps1[1])*scale);
    ppc::op_ps_add(c,0,0,1,false);
    ppc::op_psq_st(c,0,3,0,0,0);
    ppc::op_psq_l(c,0,4,8,1,0);
    ppc::op_psq_l(c,1,5,8,1,0);
    c.f[1]=double(float(c.f[1])*scale);
    ppc::op_ps_add(c,0,0,1,false);
    ppc::op_psq_st(c,0,3,8,1,0);
}
namespace uc {
// Runs the AOT body (after the lowered stwu of `frame` bytes) with the caller's stack and link register.
void pacing_call_body(ppc::Ctx& c,uint32_t frame,uint32_t entry) {
    const auto stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(stack-frame,frame+8);
    ppc::wr32(c,stack-frame,stack); c.r[1]=stack-frame;
    uc::call_indirect(c,entry);
}
}
namespace { void call_body(ppc::Ctx& c,uint32_t frame,uint32_t entry) { uc::pacing_call_body(c,frame,entry); } }
void hle_SoundFrameStep(ppc::Ctx& c) {
    const auto object=c.r[3],lr=c.lr;
    // Voice envelopes and stop delays use frames; run the complete locked list walk so removed voices are never reused during catch-up.
    for(uint32_t tick=0;tick<nominal_ticks;++tick) {
        c.r[3]=object; c.lr=lr;
        call_body(c,32,0x803AA7B4);
    }
}
void hle_SoundRetryStep(ppc::Ctx& c) {
    if(c.lr==0x8004A6FC) {
        const auto counter=c.r[3]+72;
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        memory.read(counter,4);
        const auto remaining=ppc::rd32(c,counter); // Caller already spent one attempt before this call.
        if(!nominal_ticks) { ppc::wr32(c,counter,remaining+1); c.r[3]=0; return; }
        ppc::wr32(c,counter,remaining>nominal_ticks-1 ? remaining-(nominal_ticks-1) : 0);
        // shortcut: retry once during catch-up; revisit if readiness can change within the same update.
    }
    call_body(c,96,0x8004C920);
}
// 8017A7B0 states 698/834 wait at most 30 calls for a sound stream; keep that a 30-nominal-frame timeout.
void hle_SoundWaitStep(ppc::Ctx& c) {
    const auto lr=c.lr;
    call_body(c,32,0x80049004);
    if((lr==0x8017AC74 || lr==0x8017AF5C) && c.r[3]) {
        const auto counter=c.r[31]+0x10000+7488; // r31 is the caller's object again after the epilogue
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        memory.read(counter,4);
        ppc::wr32(c,counter,ppc::rd32(c,counter)+1-nominal_ticks); // the caller subtracts one next
    }
}
void hle_HomeMenuStep(ppc::Ctx& c) {
    const auto object=c.r[3],table=c.r[4],lr=c.lr;
    if(home_object!=object || !table) { home_object=object; home_input={}; }
    if(!table) { call_body(c,112,0x803C1BFC); return; }
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,20);
    memory.read(table,64);
    constexpr std::array<uint32_t,6> offsets{0,4,8,96,100,104};
    struct Snapshot { uint32_t source=0,words=0; std::array<uint32_t,6> value{}; };
    std::array<Snapshot,4> saved{};
    for(unsigned channel=0;channel<4;++channel) {
        auto& snapshot=saved[channel];
        snapshot.source=ppc::rd32(c,table+16*channel);
        snapshot.words=ppc::rd32(c,table+16*channel+12)==2 ? 6 : 3;
        if(!snapshot.source) continue;
        memory.read(snapshot.source,snapshot.words==6 ? 108 : 12);
        for(unsigned word=0;word<snapshot.words;++word) snapshot.value[word]=ppc::rd32(c,snapshot.source+offsets[word]);
    }
    for(unsigned channel=0;channel<4;++channel) {
        const auto& snapshot=saved[channel]; auto& pending=home_input[channel];
        const auto type=ppc::rd32(c,table+16*channel+12);
        if(pending.source!=snapshot.source || pending.type!=type) pending={snapshot.source,type,{}};
        pending.edges[0]|=snapshot.value[1]; pending.edges[1]|=snapshot.value[2];
        if(snapshot.words==6) { pending.edges[2]|=snapshot.value[4]; pending.edges[3]|=snapshot.value[5]; }
    }
    if(!nominal_ticks) return;
    // Restore caller-owned KPAD data even when the guest body fails.
    struct Restore {
        ppc::Ctx& c; const std::array<Snapshot,4>& saved; const std::array<uint32_t,6>& offsets;
        ~Restore() {
            for(const auto& snapshot:saved) if(snapshot.source)
                for(unsigned word=0;word<snapshot.words;++word) ppc::wr32(c,snapshot.source+offsets[word],snapshot.value[word]);
        }
    } restore{c,saved,offsets};
    // Match 800A7BF4: subsequent catch-up iterations consume no held/edge buttons.
    // shortcut: multiple clicks between nominal updates coalesce, as they do in a 30 Hz input sample.
    for(uint32_t tick=0;tick<nominal_ticks;++tick) {
        for(unsigned channel=0;channel<4;++channel) {
            const auto& snapshot=saved[channel]; const auto& pending=home_input[channel];
            if(!snapshot.source) continue;
            for(unsigned word=0;word<snapshot.words;++word) {
                const auto value=word%3 ? pending.edges[(word/3)*2+(word%3)-1] : snapshot.value[word];
                ppc::wr32(c,snapshot.source+offsets[word],tick ? 0 : value);
            }
        }
        c.r[3]=object; c.r[4]=table; c.lr=lr;
        call_body(c,112,0x803C1BFC);
        for(auto& pending:home_input) pending.edges={};
        if(ppc::rd32(c,object+16)==18) break; // Caller exits as soon as the menu reports completion.
    }
}
void hle_WeaponTrailStep(ppc::Ctx& c) {
    const auto object=c.r[3],lr=c.lr;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,1188);
    const auto player=ppc::rd32(c,object+1172);
    if(int32_t(player)<0) return;
    c.r[3]=0x8043A818; c.r[4]=player; c.lr=0x8006DB74;
    uc::call_indirect(c,0x80068DBC);
    memory.read(c.r[3]+6536,4);
    if(ppc::rd32(c,c.r[3]+6536)==0xFFFFFFFF) { c.lr=lr; return; }
    const auto player_slot=c.r[13]-27464+player*4;
    memory.read(player_slot,4);
    c.r[3]=ppc::rd32(c,player_slot); c.lr=0x8006DB94;
    uc::call_indirect(c,0x80067CA4);
    const float zero=ppc::rdf32(c,c.r[2]-30472),limit=ppc::rdf32(c,c.r[2]-30464);
    const float rate=ppc::rdf32(c,c.r[2]-30388),dt=ppc::rdf32(c,kClock+4);
    const float fade=ppc::rdf32(c,object+1184);
    ppc::wrf32(c,object+1184,c.f[1]>zero ? std::min(limit,std::fma(rate,dt,fade)) : std::max(zero,std::fma(-rate,dt,fade)));
    // Fade remains dt-based; the eight history records retain their nominal-frame duration.
    // shortcut: catch-up samples repeat the current pointer position; interpolate if low-FPS trail quality requires it.
    for(uint32_t tick=0;tick<nominal_ticks;++tick) {
        const auto stack=c.r[1]; memory.read(stack-368,376);
        ppc::wr32(c,stack-368,stack); ppc::wr32(c,stack+4,lr); c.r[1]=stack-368;
        for(unsigned reg=30;reg<=31;++reg) {
            ppc::op_stfd(c,reg,1,336+(reg-30)*16);
            ppc::op_psq_st(c,reg,1,344+(reg-30)*16,0,0);
        }
        for(unsigned reg=27;reg<=31;++reg) ppc::wr32(c,c.r[1]+336-(32-reg)*4,c.r[reg]);
        c.r[30]=object;
        uc::call_indirect(c,0x8006DBF8);
    }
    c.lr=lr;
}
void hle_TimelineStep(ppc::Ctx& c) {
    const auto stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(stack-8688,8696); memory.read(c.r[3]+42312,4);
    // Lower the prologue through 80211784; change only the clock addition before event dispatch.
    ppc::op_stwu(c,1,1,-8688); ppc::op_mfspr(c,0,8); ppc::op_stw(c,0,1,8692);
    for(unsigned reg=29;reg<=31;++reg) {
        ppc::op_stfd(c,reg,1,8640+(reg-29)*16);
        c.r[0]=8648+(reg-29)*16; ppc::op_psq_stx(c,reg,1,0,0,0);
    }
    c.r[0]=8648; c.r[11]=c.r[1]+8640;
    for(unsigned reg=21;reg<=31;++reg) ppc::wr32(c,c.r[11]-(32-reg)*4,c.r[reg]);
    c.lr=0x8021176C;
    ppc::op_addis(c,26,3,1); ppc::op_lfs(c,29,2,-13524);
    ppc::op_lfs(c,0,26,-23224); ppc::op_or(c,3,31,3,false);
    ppc::op_lfs(c,30,2,-13520); c.r[24]=0;
    c.f[0]=c.ps1[0]=float(float(c.f[0])+float(c.f[29])*uc::game_step_scale());
    uc::call_indirect(c,0x80211788);
}
void hle_UiPhysicsStep(ppc::Ctx& c) {
    const auto address=c.r[3]+108;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(address,4);
    struct RestoreDrag {
        ppc::Ctx& c; uint32_t address,bits;
        ~RestoreDrag() { ppc::wr32(c,address,bits); }
    } restore{c,address,ppc::rd32(c,address)};
    // Configured angular drag is applied once per call at 802448F8/80244928.
    ppc::wrf32(c,address,ppc::rdf32(c,address)*uc::game_step_scale());
    call_body(c,80,0x802447DC);
}
void hle_TrailHistoryStep(ppc::Ctx& c) {
    const auto object=c.r[3],lr=c.lr,gate=c.r[13]-26960;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(gate,4);
    const auto original=ppc::rd32(c,gate);
    struct RestoreGate {
        ppc::Ctx& c; uint32_t address,value;
        ~RestoreGate() { ppc::wr32(c,address,value); }
    } restore{c,gate,original};
    // The existing secondary-camera path projects vertices without sampling history.
    if(!nominal_ticks) ppc::wr32(c,gate,1);
    for(uint32_t tick=0;tick<(original ? 1 : std::max(1u,nominal_ticks));++tick) {
        c.r[3]=object; c.lr=lr;
        call_body(c,528,0x80162A6C);
    }
}
void hle_TargetRefreshStep(ppc::Ctx& c) {
    const auto object=c.r[3],lr=c.lr;
    call_body(c,16,0x8012AEEC);
    if(lr==0x801297B0) {
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        memory.read(object+28,4);
        // The caller subtracts one next; retain its 90-nominal-frame target refresh interval.
        ppc::wr32(c,object+28,ppc::rd32(c,object+28)+1-nominal_ticks);
    }
}
void hle_EffectRetireStep(ppc::Ctx& c) {
    const auto object=c.r[3];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,16400);
    struct Record { uint32_t id,actor,age; };
    std::array<Record,64> before;
    for(unsigned i=0;i<before.size();++i) {
        const auto record=object+256*i;
        before[i]={ppc::rd32(c,record+16),ppc::rd32(c,record+268),ppc::rd32(c,record+264)};
    }
    call_body(c,6912,0x801B9170);
    // Preserve the one-nominal-frame grace period before checking animation completion.
    for(unsigned i=0;i<before.size();++i) {
        const auto record=object+256*i;
        const auto old=before[i];
        if(int32_t(old.id)>=0 && int32_t(old.age)>0 &&
           ppc::rd32(c,record+16)==old.id && ppc::rd32(c,record+268)==old.actor &&
           ppc::rd32(c,record+264)==old.age+1)
            ppc::wr32(c,record+264,old.age+nominal_ticks);
    }
}
void hle_RumbleStep(ppc::Ctx& c) {
    const auto object=c.r[3],lr=c.lr;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,1);
    // Each pattern byte originally lasted one 30 Hz frame; retain AOT list removal and motor transitions.
    for(uint32_t tick=0;tick<nominal_ticks;++tick) {
        c.r[3]=object; c.lr=lr;
        call_body(c,48,0x80136EF0);
    }
    c.r[3]=ppc::rd8(c,object)!=0;
}
// Preserve the camera's nominal-frame history delay while interpolating its output at the render/update rate.
void hle_CameraHistoryStep(ppc::Ctx& c) {
    const auto object=c.r[3],stack=c.r[1],input=c.r[4];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,412); memory.read(input,12); memory.read(stack-64,72);
    auto index=ppc::rd32(c,object+400)&31;
    std::array<uint32_t,3> current;
    std::array<float,3> previous;
    for(unsigned axis=0;axis<3;++axis) {
        current[axis]=ppc::rd32(c,input+axis*4);
        previous[axis]=ppc::rdf32(c,object+16+((index-1)&31)*12+axis*4);
    }
    for(uint32_t tick=1;tick<=nominal_ticks;++tick) {
        const float fraction=float(tick/(nominal_ticks+nominal_phase));
        for(unsigned axis=0;axis<3;++axis) {
            const auto dest=object+16+index*12+axis*4;
            if(fraction==1) ppc::wr32(c,dest,current[axis]);
            else ppc::wrf32(c,dest,std::lerp(previous[axis],std::bit_cast<float>(current[axis]),fraction));
        }
        index=(index+1)&31;
    }
    ppc::wr32(c,object+400,index);
    const auto delay=ppc::rd32(c,object+408)&31;
    const auto slot=(index-delay)&31,address=object+16+slot*12;
    struct RestoreSample {
        ppc::Ctx& c; uint32_t address; std::array<uint32_t,3> bits;
        ~RestoreSample() { for(unsigned axis=0;axis<3;++axis) ppc::wr32(c,address+axis*4,bits[axis]); }
    } restore{c,address,{ppc::rd32(c,address),ppc::rd32(c,address+4),ppc::rd32(c,address+8)}};
    for(unsigned axis=0;axis<3;++axis) {
        if(delay==1) ppc::wr32(c,address+axis*4,current[axis]);
        else if(nominal_phase>0) ppc::wrf32(c,address+axis*4,std::lerp(ppc::rdf32(c,address+axis*4),
            ppc::rdf32(c,object+16+((slot+1)&31)*12+axis*4),float(nominal_phase)));
    }
    ppc::wr32(c,stack-64,stack); ppc::wr32(c,stack+4,c.lr); ppc::wr32(c,stack-4,c.r[31]);
    c.r[1]=stack-64; c.r[31]=object;
    c.r[3]=index;
    c.r[0]=ppc::rd32(c,object+12); ppc::op_cmpi(c,0,0,1);
    uc::call_indirect(c,0x801B0AEC);
}
// 801926B0 divides (10*dt)/(20*dt), cancelling elapsed time before its history advance.
void hle_ModelHistoryIdentity(ppc::Ctx& c) {
    hle_PSMTXIdentity(c);
    if(c.lr==0x801929B0) {
        // Rebuild the numerator for each child; f28 is overwritten at 80192B34 and restored by the caller's epilogue.
        c.f[28]=c.ps1[28]=float(ppc::rdf32(c,0x806C6EF8)*ppc::rdf32(c,kClock+4))*uc::game_step_scale();
    }
}
// Nearby actors receive staggered nominal-frame delays at 801A15B4; keep their regular update running every frame.
void hle_DeferredActorStep(ppc::Ctx& c) {
    const auto actor=c.r[3],stack=c.r[1];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(actor,8492); memory.read(stack-16,24);
    ppc::wr32(c,stack-16,stack); ppc::wr32(c,stack+4,c.lr); ppc::wr32(c,stack-4,c.r[31]);
    c.r[1]=stack-16; c.r[31]=actor;
    for(uint32_t tick=0;tick<nominal_ticks;++tick) {
        const auto count=ppc::rd32(c,actor+8488);
        if(int32_t(count)<0) break;
        if(count==0) {
            c.r[3]=actor; c.lr=0x801A515C;
            uc::call_indirect(c,0x800ACB54);
        }
        ppc::wr32(c,actor+8488,ppc::rd32(c,actor+8488)-1);
    }
    uc::call_indirect(c,0x801A5168);
}
// Monkey jumps blend toward the current target each call; subdivide that nominal-frame blend.
void hle_JumpBlendStep(ppc::Ctx& c) {
    ppc::op_fmuls(c,4,1,1,false);
    uc::call_indirect(c,0x80123D18);
    const float scale=uc::game_step_scale();
    if(c.lr==0x80200B8C && scale!=1.f) {
        const double alpha=std::clamp(c.f[1],0.,1.);
        // shortcut: elapsed-dependent alpha is sampled per update; trajectories retain timestep integration error.
        c.f[1]=c.ps1[1]=scale==0 ? 0 : alpha==1 ? 1 : float(-std::expm1(scale*std::log1p(-alpha)));
    }
}
// Layer crossfade 80165610: weight += rate * elapsed seconds * playback, but the game sets playback to nominal frames per update (800AD8CC),
// so the update rate enters twice. Use elapsed nominal-frame time here; at 30 Hz the scale is 1 and nothing changes.
void hle_CrossfadeStep(ppc::Ctx& c) {
    const float scale=uc::game_step_scale();
    if(c.lr==0x8016975C && scale>0 && scale!=1.f) c.f[1]=c.ps1[1]=double(float(c.f[1])/scale);
    call_body(c,256,0x80165614);
}
// Only flight states 32/34 advance their phase by a nominal-frame speed; states 31/33 already use dt.
void hle_FlightPhaseStep(ppc::Ctx& c) {
    const auto actor=c.r[3];
    const auto state=ppc::rd16(c,actor+8174);
    const float scale=uc::game_step_scale();
    if((state==32 || state==34) && scale!=1.f) {
        struct RestoreSpeed {
            ppc::Ctx& c; uint32_t address,bits;
            ~RestoreSpeed() { ppc::wr32(c,address,bits); }
        } restore{c,actor+8176,ppc::rd32(c,actor+8176)};
        ppc::wrf32(c,actor+8176,ppc::rdf32(c,actor+8176)*scale);
        call_body(c,128,0x801C56F4);
        return;
    }
    call_body(c,128,0x801C56F4);
}

// The path follower duplicates FlightStep's mode-dependent scaling instead of calling it.
void hle_FlightPathStep(ppc::Ctx& c) {
    const auto output=c.r[3],path=c.r[4],lr=c.lr;
    c.r[3]=0x804B5CC8; c.lr=0x801C44EC;
    uc::call_indirect(c,0x801490FC);
    if(c.r[3]<16 || c.r[3]>20) c.f[1]=double(float(c.f[1])*uc::game_step_scale());
    c.r[3]=output; c.r[4]=path; c.lr=lr;
    call_body(c,160,0x801C44BC);
}

// The monkey's landing ray starts 2 units above its feet; retain its nominal reach when the gravity step becomes shorter than that offset.
void hle_WorldRaycast(ppc::Ctx& c) {
    if(c.lr==0x80200DFC && uc::game_step_scale()>0 && uc::game_step_scale()<1.f) {
        const float reach=120.f*(2.f/ppc::rdf32(c,kClock+28));
        ppc::wrf32(c,c.r[6]+4,std::min(ppc::rdf32(c,c.r[6]+4),ppc::rdf32(c,c.r[5]+4)-reach));
    }
    call_body(c,3664,0x80044D18);
}

// ODE's frame caller supplies a fixed 2/rate seconds. Scale before its four solver substeps so rigid bodies follow elapsed game time.
void hle_PhysicsStep(ppc::Ctx& c) {
    c.f[1]=double(float(c.f[1])*uc::game_step_scale());
    if(c.f[1]==0) return; // ODE requires a strictly positive timestep; pause leaves bodies untouched.
    call_body(c,80,0x801172A8);
}
// ODE's quiet-step countdown must advance in nominal substeps; its seconds countdown already uses dt.
void hle_PhysicsAutoDisable(ppc::Ctx& c) {
    const float scale=uc::game_step_scale();
    if(scale==0) return;
    const float dt=float(c.f[1]);
    for(uint32_t body=ppc::rd32(c,c.r[3]);body;body=ppc::rd32(c,body+4)) {
        const auto flags=ppc::rd32(c,body+24);
        if((flags&20)!=16) { sleep_carry.erase(body); continue; }
        const auto speed_squared=[&](uint32_t offset) {
            const float x=ppc::rdf32(c,body+offset),y=ppc::rdf32(c,body+offset+4),z=ppc::rdf32(c,body+offset+8);
            return std::fma(z,z,std::fma(x,x,y*y));
        };
        uint32_t count=ppc::rd32(c,body+348);
        float time=ppc::rdf32(c,body+344);
        if(speed_squared(248)>ppc::rdf32(c,body+328) || speed_squared(264)>ppc::rdf32(c,body+332)) {
            count=ppc::rd32(c,body+340); time=ppc::rdf32(c,body+336);
            sleep_carry.erase(body);
        } else {
            auto& carry=sleep_carry[body];
            if(carry.counter!=count || carry.timer!=ppc::rd32(c,body+344)) carry.fraction=0;
            carry.fraction+=scale;
            const auto ticks=uint32_t(carry.fraction);
            carry.fraction-=ticks; count-=ticks;
            time-=dt;
            carry.counter=count;
            std::memcpy(&carry.timer,&time,sizeof(time));
        }
        ppc::wr32(c,body+348,count); ppc::wrf32(c,body+344,time);
        if(int32_t(count)<0 && time<0) { ppc::wr32(c,body+24,flags|4); sleep_carry.erase(body); }
    }
}
void hle_PhysicsBodyDestroy(ppc::Ctx& c) {
    sleep_carry.erase(c.r[3]);
    call_body(c,32,0x800EA9B4);
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
    if(due==0) { c.r[3]=ppc::rd32(c,body+84); return; }
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
