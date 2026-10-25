#include "check.h"
#include "game_pacing.h"
#include "clock.h"
#include "dispatch.h"
#include "ppc_fixture.h"
#include <cmath>
#include <stdexcept>

static uint32_t expected_target;
static uint32_t active_object,expected_elapsed,wait_calls;
static bool fail;
static float measured_step;
static bool actor_elapsed_time;
static uint32_t flight_mode;
static bool phase_fail;
static float camera_output[3];
static unsigned deferred_fired,deferred_updates;
static unsigned rumble_ticks;
static unsigned trail_samples,trail_projections;
static bool trail_fail;
static bool ui_fail;
static unsigned timeline_events;
static unsigned weapon_samples;
static bool weapon_active;
static unsigned retire_updates;
static unsigned target_updates;
static unsigned sound_ticks;
static unsigned sound_retries;
static bool retry_ready;
static void sound_retry_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-96 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80050000);
    ++sound_retries;
    c.r[3]=retry_ready ? 1 : 0; c.r[1]+=96;
}
static double crossfade_step;
static void crossfade_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-256 && ppc::rd32(c,c.r[1])==0x817D0000);
    crossfade_step=c.f[1]; c.r[1]+=256;
}
static double fix_seen,fix_return=0.25;
static void fix_body(ppc::Ctx& c) {
    CHECK(c.r[1]<0x817D0000 && ppc::rd32(c,c.r[1])==0x817D0000);   // lowered prologue: back chain to the caller's stack
    fix_seen=c.f[1]; c.f[1]=c.ps1[1]=fix_return; c.r[1]=0x817D0000;
}
static bool stream_busy;
static void sound_wait_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-32 && ppc::rd32(c,c.r[1])==0x817D0000);
    c.r[3]=stream_busy ? 1 : 0; c.r[1]+=32;
}
static unsigned home_ticks,home_presses,home_releases,home_classic;
static bool home_fail,home_complete;
static void home_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-112 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80050000 && c.lr==0x80100000);
    ++home_ticks;
    if(c.r[4]) {
        CHECK(c.r[4]==0x80051000);
        const auto input=ppc::rd32(c,c.r[4]);
        if(input) {
            home_presses+=ppc::rd32(c,input+4)!=0;
            home_releases+=ppc::rd32(c,input+8)!=0;
            home_classic+=ppc::rd32(c,input+100)!=0;
        }
    }
    if(home_fail) throw std::runtime_error("home menu fixture failure");
    if(home_complete) ppc::wr32(c,c.r[3]+16,18);
    c.r[1]+=112; c.r[3]=0; c.r[4]=0;
}
static void sound_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-32 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80050000 && c.lr==0x80100000);
    ++sound_ticks;
    c.r[1]+=32; c.r[3]=0;
}
static void target_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-16);
    ++target_updates;
    c.r[1]+=16;
}
static bool retire_complete,retire_replace;
static void retire_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-6912 && ppc::rd32(c,c.r[1])==0x817D0000);
    const auto object=c.r[3];
    ++retire_updates;
    for(unsigned i=0;i<64;++i) {
        const auto record=object+256*i,count=ppc::rd32(c,record+264);
        if(int32_t(ppc::rd32(c,record+16))<0 || int32_t(count)<=0) continue;
        if(count>=2 && retire_complete) ppc::wr32(c,record+16,~0u);
        else ppc::wr32(c,record+264,count+1);
    }
    if(retire_replace) { ppc::wr32(c,object+268,123); ppc::wr32(c,object+264,2); }
    c.r[1]+=6912;
}
static void weapon_player(ppc::Ctx& c) { CHECK(c.r[3]==0x8043A818 && c.r[4]==0); c.r[3]=0x80060000; }
static void weapon_activity(ppc::Ctx& c) { CHECK(c.r[3]==0x80061000); c.f[1]=weapon_active ? 1 : 0; }
static void weapon_tail(ppc::Ctx& c) {
    const auto stack=c.r[1],object=c.r[30];
    CHECK(stack==0x817D0000-368 && ppc::rd32(c,stack)==0x817D0000);
    ++weapon_samples;
    ppc::wr32(c,object+1180,(ppc::rd32(c,object+1180)+1)%8);
    for(unsigned reg=27;reg<=31;++reg) c.r[reg]=ppc::rd32(c,stack+336-(32-reg)*4);
    for(unsigned reg=30;reg<=31;++reg) {
        ppc::op_psq_l(c,reg,1,344+(reg-30)*16,0,0); ppc::op_lfd(c,reg,1,336+(reg-30)*16);
    }
    c.lr=ppc::rd32(c,stack+372); c.r[1]+=368;
}
static void timeline_tail(ppc::Ctx& c) {
    const auto stack=c.r[1],object=c.r[31];
    CHECK(stack==0x817D0000-8688 && ppc::rd32(c,stack)==0x817D0000);
    CHECK(c.r[26]==object+65536 && c.f[29]==1 && c.f[30]==0);
    ppc::wrf32(c,object+42312,float(c.f[0]));
    while(timeline_events<3 && ppc::rdf32(c,object+42312)>=float((timeline_events+1)*10)) ++timeline_events;
    for(unsigned reg=21;reg<=31;++reg) c.r[reg]=ppc::rd32(c,stack+8640-(32-reg)*4);
    for(unsigned reg=29;reg<=31;++reg) {
        c.r[0]=8648+(reg-29)*16; ppc::op_psq_lx(c,reg,1,0,0,0);
        ppc::op_lfd(c,reg,1,8640+(reg-29)*16);
    }
    c.lr=ppc::rd32(c,stack+8692); c.r[1]+=8688;
}
static void ui_physics_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-80 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.lr==0x80100000);
    const auto object=c.r[3];
    if(ui_fail) throw std::runtime_error("UI fixture failure");
    const float velocity=ppc::rdf32(c,object+68),drag=ppc::rdf32(c,object+108);
    ppc::wrf32(c,object+68,velocity>0 ? std::max(0.f,velocity-drag) : std::min(0.f,velocity+drag));
    c.r[1]+=80;
}
static void trail_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-528 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80050000 && c.lr==0x80100000);
    if(!ppc::rd32(c,c.r[13]-26960)) ++trail_samples;
    ++trail_projections;
    if(trail_fail) throw std::runtime_error("trail fixture failure");
    c.r[1]+=528; c.r[3]=1;
}
static void rumble_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-48 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80050000 && c.lr==0x80100000);
    c.r[1]+=48;
    if(ppc::rd8(c,c.r[3])) { ++rumble_ticks; c.r[3]=1; }
    else c.r[3]=0;
}
static void deferred_action(ppc::Ctx& c) {
    CHECK(c.lr==0x801A515C && c.r[3]==0x80050000);
    ++deferred_fired;
}
static void deferred_tail(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-16 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[31]==0x80050000);
    ++deferred_updates;
    c.lr=ppc::rd32(c,c.r[1]+20); c.r[31]=ppc::rd32(c,c.r[1]+12); c.r[1]+=16;
}
static void camera_history_tail(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-64 && ppc::rd32(c,c.r[1])==0x817D0000);
    const auto object=c.r[31];
    const auto slot=(ppc::rd32(c,object+400)-ppc::rd32(c,object+408))&31;
    for(unsigned axis=0;axis<3;++axis) camera_output[axis]=ppc::rdf32(c,object+16+slot*12+axis*4);
    c.lr=ppc::rd32(c,c.r[1]+68); c.r[31]=ppc::rd32(c,c.r[1]+60); c.r[1]+=64;
}
static void flight_phase_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-128 && ppc::rd32(c,c.r[1])==0x817D0000);
    const auto actor=c.r[3],state=ppc::rd16(c,actor+8174);
    const float dt=ppc::rdf32(c,0x804D331C);
    ppc::wrf32(c,actor+8672,ppc::rdf32(c,actor+8672)+dt);
    if(phase_fail) throw std::runtime_error("phase fixture failure");
    float age=ppc::rdf32(c,actor+8208)+(state==32 || state==34 ? ppc::rdf32(c,actor+8176) : dt);
    if((state==32 || state==34) && age>=ppc::rdf32(c,actor+8668)) {
        ppc::wr16(c,actor+8174,state-1); age=0;
    }
    ppc::wrf32(c,actor+8208,age); c.r[1]+=128;
}
static void jump_blend_tail(ppc::Ctx& c) {
    CHECK(float(c.f[4])==float(c.f[1])*float(c.f[1]));
    const double x=c.f[1]; c.f[1]=float(x*x*x*(10+x*(-15+6*x)));
}
static void flight_path_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-160 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x80005000 && c.r[4]==0x80006000 && c.lr==0x801D2264);
    if(flight_mode>=16 && flight_mode<=20)
        c.f[1]=float(c.f[1])*(ppc::rdf32(c,0x804D3318+28)*0.5f)*ppc::rdf32(c,0x804D3318+4);
    c.r[1]+=160;
}
static void raycast_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-3664 && ppc::rd32(c,c.r[1])==0x817D0000);
    const float start=ppc::rdf32(c,c.r[5]+4),end=ppc::rdf32(c,c.r[6]+4);
    c.r[3]=start>=170.f && end<=170.f;
    c.r[1]+=3664;
}
static void flight_selector(ppc::Ctx& c) {
    CHECK(c.r[3]==0x804B5CC8);
    c.r[3]=flight_mode;
}
static void retrace(ppc::Ctx& c) {
    ++wait_calls;
    CHECK(ppc::rd32(c,active_object+0x28)==expected_target); // Other threads/callbacks always see original policy.
    if(fail) throw std::runtime_error("wait fixture failure");
    ppc::wr32(c,active_object+0x20,ppc::rd32(c,active_object+0x20)+1);
}
static void original_tail(ppc::Ctx& c) {
    const auto object=c.r[31];
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000 && c.lr==0x801581F8);
    CHECK(ppc::rd32(c,c.r[1]+36)==0x80100000);
    CHECK(ppc::rd32(c,object+0x28)==expected_target);
    CHECK(c.r[3]==expected_elapsed);
    ppc::wr32(c,object+0x24,c.r[3]); ppc::wr32(c,object+0x20,0);
    ppc::wrf32(c,object+8,measured_step ? measured_step : float(c.r[3])/60);
    c.lr=ppc::rd32(c,c.r[1]+36); c.r[31]=ppc::rd32(c,c.r[1]+28);
    c.r[1]=ppc::rd32(c,c.r[1]); c.r[3]=123;
}
// Animated models return elapsed time; plain models divide their fixed playback rate by the nominal frame rate.
static void actor_step_body(ppc::Ctx& c) {
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000 && c.r[1]==0x817D0000-48 && c.lr==0x80100000);   // caller stack/LR intact, prologue lowered
    if(actor_elapsed_time) c.f[1]=ppc::rdf32(c,0x804D3318+8);
    else {
        c.r[3]=0x80004000; ppc::wrf32(c,c.r[3]+16,1.f); c.lr=0x800C799C;
        hle_FixedPlaybackStep(c);
        c.f[1]=double(float(c.f[1])/30.f); c.lr=0x80100000;
    }
    c.r[1]+=48;
}
// Original body of the HUD name delay: counts up to 10, fires once on the 10th call and goes to 11.
static uint32_t hud_fired;
static void hud_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-400 && ppc::rd32(c,c.r[1])==0x817D0000);
    auto counter=ppc::rd32(c,c.r[3]+7752);
    if(counter<10) ppc::wr32(c,c.r[3]+7752,++counter);
    if(counter==10) { ++hud_fired; ppc::wr32(c,c.r[3]+7752,11); }
    c.r[3]=0; c.r[1]+=400;
}
// Original body of the prop integrator (80210278): counts runs; the caller's stack and link register stay intact.
static uint32_t prop_runs;
static float physics_elapsed;
static unsigned physics_runs;
static float hit_force,hit_impulse;
static void physics_destroy_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-32 && ppc::rd32(c,c.r[1])==0x817D0000);
    c.r[1]+=32;
}
static void physics_hit_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-32 && ppc::rd32(c,c.r[1])==0x817D0000);
    hit_force+=float(c.f[1]); c.r[1]+=32; c.r[3]=1;
}
static void physics_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-80 && ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.r[3]==0x804A197C && c.r[4]==0 && c.r[5]==4 && c.r[6]==0);
    physics_elapsed+=float(c.f[1]); ++physics_runs; c.r[1]+=80;
    hit_impulse+=hit_force*float(c.f[1])/4; hit_force=0; // ODE clears an event's force after its first substep.
}
static void prop_body(ppc::Ctx& c) {
    CHECK(c.r[1]==0x817D0000-96 && ppc::rd32(c,c.r[1])==0x817D0000);
    ++prop_runs; c.r[3]=ppc::rd32(c,c.r[3]+84)+1; c.r[1]+=96;
}
int main() {
    for(const auto ticks:{uint64_t(1),uc::kTimebaseHz/120-1,uc::kTimebaseHz/20+1}) {
        bool rejected=false;
        try {uc::game_pacing_set_network_period(ticks);}catch(const std::invalid_argument&) {rejected=true;}
        CHECK(rejected);
    }
    uc::game_pacing_set_network_period(uc::kTimebaseHz/120);
    uc::game_pacing_set_network_period(uc::kTimebaseHz/60);
    uc::game_pacing_set_network_period(uc::kTimebaseHz/20);
    uc::game_pacing_set_network_period(0);
    Fix fixture; auto& c=fixture.c;
    const uc::FnEntry entries[]={{0x80158208,original_tail},{0x8026E194,retrace},{0x800C7910,actor_step_body},{0x8014D36C,hud_body},{0x8021027C,prop_body},{0x801172A8,physics_body},{0x80117468,physics_hit_body},{0x801490FC,flight_selector},{0x80044D18,raycast_body},{0x801C44BC,flight_path_body},{0x80123D18,jump_blend_tail},{0x801C56F4,flight_phase_body},{0x800EA9B4,physics_destroy_body},{0x801B0AEC,camera_history_tail},{0x800ACB54,deferred_action},{0x801A5168,deferred_tail}};
    const uc::FnEntry rumble_entry{0x80136EF0,rumble_body};
    std::vector<uc::FnEntry> all_entries(std::begin(entries),std::end(entries));
    all_entries.push_back(rumble_entry);
    all_entries.push_back({0x80162A6C,trail_body});
    all_entries.push_back({0x802447DC,ui_physics_body});
    all_entries.push_back({0x80211788,timeline_tail});
    all_entries.push_back({0x80068DBC,weapon_player});
    all_entries.push_back({0x80067CA4,weapon_activity});
    all_entries.push_back({0x8006DBF8,weapon_tail});
    all_entries.push_back({0x801B9170,retire_body});
    all_entries.push_back({0x8012AEEC,target_body});
    all_entries.push_back({0x803AA7B4,sound_body});
    all_entries.push_back({0x8004C920,sound_retry_body});
    all_entries.push_back({0x80049004,sound_wait_body});
    all_entries.push_back({0x80165614,crossfade_body});
    all_entries.push_back({0x80170004,fix_body});
    for(const auto& fix:uc::pacing_fix_table()) all_entries.push_back({fix.entry+4,fix_body});
    all_entries.push_back({0x803C1BFC,home_body});
    uc::dispatch_init(all_entries.data(),all_entries.size(),0x80044D18,0x803D0000);
    auto run=[&](uint32_t object,uint32_t target,uint32_t expected,uint32_t initial=0) {
        c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=object; c.r[31]=987;
        ppc::wr32(c,object+0x20,initial); ppc::wr32(c,object+0x28,target);
        expected_target=target; active_object=object; expected_elapsed=expected; wait_calls=0;
        hle_GameFrameWait(c);
        CHECK(ppc::rd32(c,object+0x28)==target && c.r[1]==0x817D0000 && c.r[3]==123);
        CHECK(ppc::rd32(c,object+0x24)==expected);
        CHECK(wait_calls==expected-initial && c.r[31]==987 && c.lr==0x80100000);
    };
    uc::game_pacing_init(false);
    ppc::wrf32(c,0x804D3334,59.94f);   // rate; the nominal frame time is target / rate
    run(0x804D3318,2,2);
    CHECK(uc::game_pacing_stats().calls==1 && uc::game_pacing_stats().adjusted==0);
    CHECK(uc::game_step_scale()==1.f);   // original mode keeps one call per nominal frame
    auto actor=[&]{ c.r[1]=0x817D0000; c.lr=0x80100000; hle_ActorStepTime(c); CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000); return float(c.f[1]); };
    {
        const uint32_t object=0x80050000;
        auto retire=[&] { c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=object; hle_EffectRetireStep(c); CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000); };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            auto step=[&] { run(0x804D3318,2,hz>30 ? 1 : 2); };
            for(unsigned i=0;i<64;++i) { ppc::wr32(c,object+256*i+16,i); ppc::wr32(c,object+256*i+264,1); ppc::wr32(c,object+256*i+268,100+i); }
            retire_updates=0; retire_complete=false; retire_replace=false;
            for(unsigned frame=0;frame<hz;++frame) { step(); retire(); }
            CHECK(retire_updates==hz);
            for(unsigned i=0;i<64;++i) CHECK(ppc::rd32(c,object+256*i+264)==31);
            uc::game_pacing_set_paused(true); step(); retire();
            CHECK(ppc::rd32(c,object+264)==31);
            // A replacement registered by the original body must retain its new counter.
            ppc::wr32(c,object+264,1); retire_replace=true; retire();
            CHECK(ppc::rd32(c,object+264)==2); retire_replace=false;
            uc::game_pacing_set_paused(false);
            ppc::wr32(c,object+264,1); ppc::wr32(c,object+16,0); retire_complete=true;
            uc::game_pacing_init(hz>30);
            for(unsigned frame=0;frame<hz/30;++frame) { step(); retire(); CHECK(ppc::rd32(c,object+16)==0); }
            step(); retire(); CHECK(ppc::rd32(c,object+16)==~0u);
        }
        retire_complete=false; measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    CHECK(std::fabs(actor()-1.f/30.f)<1e-6f);
    {
        const uint32_t object=0x80050000;
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            ppc::wr32(c,object+28,90); target_updates=0;
            auto update=[&](uint32_t caller=0x801297B0) {
                c.r[1]=0x817D0000; c.r[3]=object; c.lr=caller;
                hle_TargetRefreshStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==caller);
                if(caller==0x801297B0) ppc::wr32(c,object+28,ppc::rd32(c,object+28)-1);
            };
            for(unsigned frame=0;frame<hz*3;++frame) {
                run(0x804D3318,2,hz>30 ? 1 : 2); update();
                CHECK(int32_t(ppc::rd32(c,object+28))>=0);
            }
            CHECK(ppc::rd32(c,object+28)==0 && target_updates==hz*3);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz>30 ? 1 : 2); update();
            CHECK(ppc::rd32(c,object+28)==0);
            ppc::wr32(c,object+28,17); update(0x80100000); CHECK(ppc::rd32(c,object+28)==17);
            uc::game_pacing_set_paused(false);
        }
        measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {   // Sound frame callbacks keep nominal fade/stop durations; audio-buffer callbacks are outside this wrapper.
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz; sound_ticks=0;
            auto update=[&] {
                c.r[1]=0x817D0000; c.r[3]=0x80050000; c.lr=0x80100000;
                hle_SoundFrameStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
            };
            for(unsigned frame=0;frame<hz;++frame) { run(0x804D3318,2,hz>30 ? 1 : 2); update(); }
            CHECK(sound_ticks==30);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz>30 ? 1 : 2); update(); CHECK(sound_ticks==30);
            uc::game_pacing_set_paused(false);
            uc::game_pacing_set_mode({true,0}); measured_step=0.1f; run(0x804D3318,2,1); update(); CHECK(sound_ticks==33);
        }
        measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {
        // Home menu timeouts use nominal frames; button edges survive skipped draws and fire once on catch-up.
        constexpr uint32_t table=0x80051000,input=0x80052000;
        for(unsigned i=0;i<64;i+=4) ppc::wr32(c,table+i,0);
        ppc::wr32(c,0x80050000+16,0);
        ppc::wr32(c,table,input); ppc::wr32(c,table+12,2);
        auto clear_input=[&] { for(unsigned off : {0u,4u,8u,96u,100u,104u}) ppc::wr32(c,input+off,0); };
        auto update=[&](bool initialize=false) {
            c.r[1]=0x817D0000; c.r[3]=0x80050000; c.r[4]=initialize ? 0 : table; c.lr=0x80100000;
            hle_HomeMenuStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            home_ticks=home_presses=home_releases=home_classic=0; clear_input();
            for(unsigned frame=0;frame<hz;++frame) {
                run(0x804D3318,2,hz>30 ? 1 : 2);
                // A one-render-frame click, shorter than one nominal update at high FPS.
                ppc::wr32(c,input+4,frame==0 ? 1 : 0);
                ppc::wr32(c,input+8,frame==0 ? 1 : 0);
                ppc::wr32(c,input+100,frame==0 ? 8 : 0);
                update();
                CHECK(ppc::rd32(c,input+4)==(frame==0 ? 1u : 0u));
            }
            CHECK(home_ticks==30 && home_presses==1 && home_releases==1 && home_classic==1);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz>30 ? 1 : 2); update(); CHECK(home_ticks==30);
            update(true); CHECK(home_ticks==31); // Initialization still executes while no simulation tick is due.
            uc::game_pacing_set_paused(false); uc::game_pacing_set_mode({true,0}); measured_step=0.1f;
            run(0x804D3318,2,1); ppc::wr32(c,input+4,1); update();
            CHECK(home_ticks==34 && home_presses==2 && ppc::rd32(c,input+4)==1);
        }
        uc::game_pacing_init(true); measured_step=1.f/60; clear_input();
        run(0x804D3318,2,1); ppc::wr32(c,input+4,1); update(); // Latch, then recreate the menu.
        update(true); clear_input(); run(0x804D3318,2,1);
        const auto before=home_presses; update(); CHECK(home_presses==before);
        measured_step=0.1f; run(0x804D3318,2,1);
        ppc::wr32(c,input+4,7); ppc::wr32(c,input+100,9); home_fail=true;
        bool threw=false; try { update(); } catch(const std::runtime_error&) { threw=true; }
        home_fail=false; CHECK(threw && ppc::rd32(c,input+4)==7 && ppc::rd32(c,input+100)==9);
        clear_input(); home_complete=true; const auto completed_at=home_ticks;
        update(); CHECK(home_ticks==completed_at+1); // Stop catch-up at the same completion boundary as the caller.
        home_complete=false; ppc::wr32(c,0x80050000+16,0);
        uc::game_pacing_init(true); measured_step=1.f/60;
        run(0x804D3318,2,1); ppc::wr32(c,input+4,1); update();
        ppc::wr32(c,table,0); clear_input(); run(0x804D3318,2,1); update(); // Disconnect discards a pending click.
        ppc::wr32(c,table,input); const auto reconnect=home_presses;
        run(0x804D3318,2,1); update(); run(0x804D3318,2,1); update(); CHECK(home_presses==reconnect);
        // Menus can request a nominal 60 Hz clock; preserve that rate too.
        uc::game_pacing_init(false); measured_step=1.f/60; home_ticks=0;
        for(unsigned frame=0;frame<60;++frame) { run(0x804D3318,1,1); update(); }
        CHECK(home_ticks==60);
        clear_input(); measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {
        constexpr uint32_t record=0x80050000;
        auto retry=[&](bool initial=false) {
            // Original caller decrements before calling, then cancels retries on success.
            if(!initial) {
                const auto left=ppc::rd32(c,record+72); if(!left) return;
                ppc::wr32(c,record+72,left-1);
            }
            c.r[1]=0x817D0000; c.r[3]=record; c.lr=initial ? 0x80100000 : 0x8004A6FC;
            hle_SoundRetryStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==(initial ? 0x80100000u : 0x8004A6FCu));
            if(c.r[3]) ppc::wr32(c,record+72,0);
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            ppc::wr32(c,record+72,6); sound_retries=0; retry_ready=false;
            for(unsigned frame=0;frame<hz/5;++frame) { run(0x804D3318,2,hz>30 ? 1 : 2); retry(); }
            CHECK(sound_retries==6 && ppc::rd32(c,record+72)==0);
            // A resource becoming ready after 100 ms must still get its retry at high FPS.
            uc::game_pacing_init(hz>30); ppc::wr32(c,record+72,6); sound_retries=0;
            for(unsigned frame=0;frame<hz/5;++frame) {
                retry_ready=frame>=hz/10; run(0x804D3318,2,hz>30 ? 1 : 2); retry();
            }
            CHECK(sound_retries==4 && ppc::rd32(c,record+72)==0);
            retry_ready=false; ppc::wr32(c,record+72,6); const auto before=sound_retries;
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz>30 ? 1 : 2); retry();
            CHECK(sound_retries==before && ppc::rd32(c,record+72)==6);
            retry(true); CHECK(sound_retries==before+1); // New sound requests keep their original entry behavior.
            uc::game_pacing_set_paused(false); uc::game_pacing_set_mode({true,0}); measured_step=0.1f;
            run(0x804D3318,2,1); retry(); CHECK(ppc::rd32(c,record+72)==3);
            ppc::wr32(c,record+72,1); retry(); CHECK(ppc::rd32(c,record+72)==0);
        }
        measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {   // Generated pacing fixes (game/pacing_fixes.inc): identity at the nominal rate and on pause, exact templates otherwise.
        using uc::PacingTemplate;
        auto apply=[&](const uc::PacingFix& fix,uint32_t lr,double in) {
            c.r[1]=0x817D0000; c.lr=lr; c.f[1]=c.ps1[1]=in; fix_seen=-1;
            uc::pacing_apply_record(c,fix,64);
            CHECK(c.r[1]==0x817D0000 && c.lr==lr);
            return c.f[1];
        };
        const uc::PacingFix fixes[]={{0x80170000,0x80170100,PacingTemplate::scale,1,-1},{0x80170000,0x80170100,PacingTemplate::scale,1,1},
                                     {0x80170000,0x80170100,PacingTemplate::scale_ret,1,1},{0x80170000,0x80170100,PacingTemplate::pow_alpha,1,0}};
        for(bool paused : {false,true}) {
            uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); uc::game_pacing_set_paused(paused); run(0x804D3318,2,2);
            for(const auto& fix:fixes) { CHECK(apply(fix,0x80170100,0.5)==fix_return && fix_seen==0.5); }   // scale 1 or pause: identity
            uc::game_pacing_set_paused(false);
        }
        uc::game_pacing_init(true); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/480; run(0x804D3318,2,1);
        const double s=uc::game_step_scale();
        CHECK(std::fabs(s-1.0/16)<1e-6);
        CHECK(apply(fixes[0],0x80170100,0.5)==fix_return && std::fabs(fix_seen-0.5/s)<1e-6);
        CHECK(apply(fixes[1],0x80170100,0.5)==fix_return && std::fabs(fix_seen-0.5*s)<1e-6);
        CHECK(std::fabs(apply(fixes[2],0x80170100,0.5)-fix_return*s)<1e-6 && fix_seen==0.5);
        CHECK(std::fabs(apply(fixes[3],0x80170100,0.5)-(1-std::pow(1-fix_return,s)))<1e-6);
        for(const auto& fix:fixes) { CHECK(apply(fix,0x80170200,0.5)==fix_return && fix_seen==0.5); }   // other callers unchanged
        for(const auto& fix:uc::pacing_fix_table()) {   // every generated record: identity at the nominal rate
            uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
            c.r[1]=0x817D0000; c.lr=fix.lr; c.f[fix.reg]=c.ps1[fix.reg]=0.5; fix_seen=-1;
            uc::pacing_apply_record(c,fix,64);
            CHECK(c.r[1]==0x817D0000 && c.lr==fix.lr && (fix.reg!=1 || fix_seen==0.5));
        }
        measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {   // Animation crossfades: weight += rate * elapsed * playback, and playback is already frames per update; keep the 30 Hz fade duration.
        auto fade=[&](double elapsed,uint32_t caller=0x8016975C) {
            c.r[1]=0x817D0000; c.lr=caller; c.f[1]=c.ps1[1]=elapsed;
            hle_CrossfadeStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==caller);
            return crossfade_step;
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            run(0x804D3318,2,hz>30 ? 1 : 2);
            const double scale=uc::game_step_scale(),playback=scale;   // the game sets playback to nominal frames per update
            double weight=0;
            for(unsigned frame=0;frame<hz/10;++frame) weight+=3.0*fade(1.0/hz)*playback;   // 0.1 s of updates
            CHECK(std::fabs(weight-0.3)<1e-3);   // rate 3/s: 0.3 after 0.1 s at every update rate
            CHECK(std::fabs(fade(0.5,0x80100000)-0.5)<1e-9);   // other callers unchanged
        }
        uc::game_pacing_set_paused(true); run(0x804D3318,2,1);
        CHECK(fade(0)==0);   // pause: no division by a zero scale
        uc::game_pacing_set_paused(false);
        measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    {   // Stream-start waits in 8017A7B0 time out after 30 nominal frames, not 30 updates.
        constexpr uint32_t object=0x80060000,counter=object+0x10000+7488;
        auto wait=[&](uint32_t caller=0x8017AC74) {
            c.r[1]=0x817D0000; c.r[31]=object; c.lr=caller;
            hle_SoundWaitStep(c);
            CHECK(c.r[1]==0x817D0000 && c.r[31]==object && c.lr==caller);
            if(!c.r[3]) return false;
            ppc::wr32(c,counter,ppc::rd32(c,counter)-1);   // caller's decrement at 8017AC84
            return int32_t(ppc::rd32(c,counter))>0;
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); ppc::wrf32(c,0x804D3334,60); measured_step=1.f/hz;
            for(uint32_t caller : {0x8017AC74u,0x8017AF5Cu}) {
                ppc::wr32(c,counter,30); stream_busy=true;
                unsigned frames=0;
                do { run(0x804D3318,2,hz>30 ? 1 : 2); ++frames; } while(wait(caller) && frames<hz*2);
                CHECK(frames==hz);   // one second of nominal 30 Hz frames at every update rate
            }
            ppc::wr32(c,counter,30);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz>30 ? 1 : 2); wait();
            CHECK(ppc::rd32(c,counter)==30);   // pause keeps the remaining wait
            uc::game_pacing_set_paused(false);
            run(0x804D3318,2,hz>30 ? 1 : 2);
            ppc::wr32(c,counter,17); c.r[1]=0x817D0000; c.r[31]=object; c.lr=0x80105ED4; hle_SoundWaitStep(c);
            CHECK(ppc::rd32(c,counter)==17);   // other callers keep the original query
            stream_busy=false; ppc::wr32(c,counter,30); CHECK(!wait() && ppc::rd32(c,counter)==30);
        }
        stream_busy=false; measured_step=0; uc::game_pacing_init(false); ppc::wrf32(c,0x804D3334,59.94f); run(0x804D3318,2,2);
    }
    auto physics=[&] {
        c.r[1]=0x817D0000; c.lr=0x80159C08;
        c.r[3]=0x804A197C; c.r[4]=0; c.r[5]=4; c.r[6]=0;
        c.f[1]=2.f/59.94f;
        hle_PhysicsStep(c);
        CHECK(c.r[1]==0x817D0000 && c.lr==0x80159C08);
    };
    auto hit=[&] {
        c.r[1]=0x817D0000; c.lr=0x8010093C; c.r[3]=0x804A197C; c.f[1]=120;
        hle_PhysicsHit(c);
        CHECK(c.r[1]==0x817D0000 && c.lr==0x8010093C);
    };
    auto flight=[&](uint32_t mode) {
        flight_mode=mode; c.r[1]=0x817D0000; c.lr=0x801D27B0;
        c.r[2]=0x804DA000; c.r[3]=0x80005000; c.r[4]=0x80004000;
        ppc::wrf32(c,c.r[2]-18452,0.5f);
        ppc::wrf32(c,c.r[4],5.f);
        hle_FlightStep(c);
        CHECK(c.r[1]==0x817D0000 && c.lr==0x801D27B0);
        return ppc::rdf32(c,0x80004000);
    };
    auto landing=[&](float dt,uint32_t caller=0x80200DFC,float height=170.01f) {
        c.r[1]=0x817D0000; c.lr=caller; c.r[5]=0x80004000; c.r[6]=0x80004020;
        ppc::wrf32(c,c.r[5]+4,height+2.f);
        ppc::wrf32(c,c.r[6]+4,height+2.f-120.f*dt);
        hle_WorldRaycast(c);
        CHECK(c.r[1]==0x817D0000 && c.lr==caller);
        return c.r[3]!=0;
    };
    auto flight_path=[&](uint32_t mode) {
        flight_mode=mode; c.r[1]=0x817D0000; c.lr=0x801D2264;
        c.r[3]=0x80005000; c.r[4]=0x80006000; c.f[1]=5.f;
        hle_FlightPathStep(c);
        CHECK(c.r[1]==0x817D0000 && c.lr==0x801D2264);
        return float(c.f[1]);
    };
    CHECK(flight_path(5)==5.f);
    CHECK(landing(2.f/59.94f));
    CHECK(!landing(2.f/59.94f,0x80200DFC,180.f)); // Still airborne: do not snap to a distant floor.
    CHECK(flight(5)==5.f); // Original chapter 2 displacement per nominal frame.
    hit_impulse=0; hit(); physics();
    CHECK(std::fabs(hit_impulse-1.001001f)<1e-5f);
    physics_elapsed=0; physics_runs=0;
    for(int i=0;i<30;++i) physics();
    CHECK(physics_runs==30 && std::fabs(physics_elapsed-1.001001f)<1e-5f);
    {   // original mode: every call is a nominal frame, the wrapper adds nothing
        hud_fired=0; ppc::wr32(c,0x90005000+7752,0);
        for(int i=0;i<10;++i) { c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x90005000; hle_HudNameDelay(c); CHECK(c.lr==0x80100000); }
        CHECK(hud_fired==1 && ppc::rd32(c,0x90005000+7752)==11);
    }
    run(0x804D3318,2,3,2); // Original mode always waits at least once.
    uc::game_pacing_init(true);
    run(0x804D3318,2,1);
    CHECK(uc::game_pacing_stats().calls==1 && uc::game_pacing_stats().adjusted==1 && uc::game_pacing_stats().retraces==1);
    {   // 60 Hz step: one retrace is half of the nominal 1/29.97 s frame
        const float scale=uc::game_step_scale();
        CHECK(std::fabs(scale-0.5f)<0.01f);
        CHECK(std::fabs(actor()-scale/30.f)<1e-6f);   // the per-call step shrinks with the real step
        CHECK(std::fabs(flight(5)*60.f-5.f*29.97f)<0.001f);
        actor_elapsed_time=true;
        CHECK(std::fabs(actor()-1.f/60.f)<1e-6f); // Animated models already supplied elapsed time; scaling again slows AI/movement.
        actor_elapsed_time=false;
        hit_impulse=0; hit(); physics();
        CHECK(std::fabs(hit_impulse-1.001001f)<1e-5f);
        physics_elapsed=0; physics_runs=0;
        for(int i=0;i<60;++i) physics();
        CHECK(physics_runs==60 && std::fabs(physics_elapsed-1.f)<1e-5f);
    }
    {   // the HUD name delay follows nominal frames: about half a frame per call here
        hud_fired=0; ppc::wr32(c,0x90005000+7752,0);
        int calls_to_fire=0;
        for(int i=1;i<=40;++i) { c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x90005000; hle_HudNameDelay(c); CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000); if(hud_fired && !calls_to_fire) calls_to_fire=i; }
        CHECK(hud_fired==1 && calls_to_fire>=19 && calls_to_fire<=21 && ppc::rd32(c,0x90005000+7752)==11);   // names wait ten nominal frames, not ten calls
    }
    {   // prop integrator at the 60 Hz step: every call runs (it is only skipped above 60 Hz), the original return value comes back
        prop_runs=0; ppc::wr32(c,0x90006000+84,5);
        for(int i=0;i<10;++i) { c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x90006000; hle_PropStep(c); CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000 && c.r[3]==6); }
        CHECK(prop_runs==10);
    }
    run(0x804D3318,1,1); run(0x804D3318,3,3); run(0x80003000,2,2);
    CHECK(uc::game_pacing_stats().calls==3 && uc::game_pacing_stats().adjusted==1);
    run(0x804D3318,2,1,1); // GPU work already consumed the target retrace; no second wait.
    c.r[1]=0x817D0000; c.r[3]=0x804D3318; ppc::wr32(c,c.r[3]+0x28,2);
    active_object=c.r[3]; expected_target=2; fail=true;
    bool rejected=false; try { hle_GameFrameWait(c); } catch(const std::runtime_error&) { rejected=true; }
    CHECK(rejected && ppc::rd32(c,0x804D3340)==2);
    c.r[1]=0x817D0000; c.r[3]=0xCC000000;
    rejected=false; try { hle_GameFrameWait(c); } catch(const std::exception&) { rejected=true; }
    CHECK(rejected && c.r[1]==0x817D0000);
    c.r[1]=0x817FFFFC; c.r[3]=0x804D3318;
    rejected=false; try { hle_GameFrameWait(c); } catch(const std::exception&) { rejected=true; }
    CHECK(rejected && c.r[1]==0x817FFFFC);
    fail=false;
    {   // the frame rate limit picks the update mode (startup and the in-game menu)
        const auto is=[](uc::PacingMode m,bool unlock,uint32_t hz) { return m.unlock_60==unlock && m.game_hz==hz; };
        CHECK(is(uc::pacing_for_limit(std::nullopt,0,false),false,0));   // no limit chosen: the original 30 Hz
        CHECK(is(uc::pacing_for_limit(std::nullopt,0,true),true,0));     // the old "60 FPS game update" toggle
        CHECK(is(uc::pacing_for_limit(30,0,false),false,0));
        CHECK(is(uc::pacing_for_limit(60,0,false),true,0));               // 31..60: retrace-locked 60 Hz
        CHECK(is(uc::pacing_for_limit(144,0,false),false,144));           // above 60: timer step at the limit
        CHECK(is(uc::pacing_for_limit(0,0,false),false,480));             // unlimited: the 480 Hz ceiling
        CHECK(is(uc::pacing_for_limit(1000,0,false),false,480));
        CHECK(is(uc::pacing_for_limit(144,240,false),false,240));         // --game-hz wins
    }
    {   // live mode change: picked up at the next frame wait
        uc::game_pacing_init(false);
        uc::game_pacing_set_mode({true,0});
        run(0x804D3318,2,1);
        CHECK(uc::game_pacing_stats().adjusted==1);
        uc::game_pacing_set_mode({false,0});
        run(0x804D3318,2,2);
        CHECK(uc::game_pacing_stats().adjusted==1 && uc::game_step_scale()==1.f);
    }
    {   // pause: the game clock steps by zero, so everything scaled by the step stands still; resume restores the step
        CHECK(!uc::game_pacing_paused());
        uc::game_pacing_set_paused(true); CHECK(uc::game_pacing_paused());
        run(0x804D3318,2,2);
        CHECK(ppc::rdf32(c,0x804D3318+8)==0.f && ppc::rdf32(c,0x804D3318+4)==0.f && uc::game_step_scale()==0.f && actor()==0.f);
        physics_elapsed=0; physics_runs=0; hit(); physics();
        CHECK(physics_runs==0 && physics_elapsed==0.f && hit_force==0.f); // ODE rejects a zero-length solver step.
        CHECK(flight(5)==0.f && flight(16)==0.f);
        CHECK(flight_path(5)==0.f && flight_path(16)==0.f);
        CHECK(!landing(0));
        prop_runs=0; ppc::wr32(c,0x90008000+84,7);
        for(int i=0;i<3;++i) {
            c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x90008000;
            hle_PropStep(c);
            CHECK(c.r[3]==7 && c.r[1]==0x817D0000 && c.lr==0x80100000);
        }
        CHECK(prop_runs==0); // A newly encountered spring must also remain still during pause.
        run(0x80003000,2,2);   // other clocks are left alone
        CHECK(std::fabs(ppc::rdf32(c,0x80003000+8)-2.f/60)<1e-6f);
        uc::game_pacing_set_paused(false);
        run(0x804D3318,2,2);
        CHECK(std::fabs(ppc::rdf32(c,0x804D3318+8)-2.f/60)<1e-6f && uc::game_step_scale()==1.f);
    }
    for(unsigned hz : {120u,240u,480u}) {
        uc::game_pacing_init(true);
        measured_step=1.f/hz;
        run(0x804D3318,2,1);
        ppc::wrf32(c,0x804D3318+4,measured_step);
        CHECK(landing(measured_step)); // The +2 origin offset must not leave the probe wholly above a grounded monkey.
        CHECK(!landing(measured_step,0x801C41C8)); // Other world raycasts retain their supplied geometry.
        CHECK(!landing(measured_step,0x80200DFC,180.f));
        for(uint32_t mode : {0u,5u,15u,16u,20u,21u}) {
            CHECK(std::fabs(flight(mode)*hz-5.f*29.97f)<0.001f); // Equal distance per second, including modes already scaled by the guest.
            CHECK(std::fabs(flight_path(mode)*hz-5.f*29.97f)<0.001f);
        }
        physics_elapsed=0; physics_runs=0;
        for(unsigned i=0;i<hz;++i) physics();
        CHECK(physics_runs==hz && std::fabs(physics_elapsed-1.f)<1e-5f);
        hit_impulse=0; hit(); physics();
        CHECK(std::fabs(hit_impulse-1.001001f)<1e-5f);
        actor_elapsed_time=true;
        CHECK(std::fabs(actor()-1.f/hz)<1e-6f);
        actor_elapsed_time=false;
        CHECK(std::fabs(actor()-uc::game_step_scale()/30.f)<1e-6f);
        c.r[3]=0x80004000; c.lr=0x8016B250; ppc::wrf32(c,c.r[3]+16,1.5f);
        hle_FixedPlaybackStep(c);
        CHECK(c.f[1]==1.5 && c.ps1[1]==1.5 && c.lr==0x8016B250); // Other playback-property consumers keep the original rate.
    }
    {   // Execute the original lighting/UV recurrences against coefficients supplied at the frame boundary.
        ppc::wrf32(c,0x804D3334,60.f);
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            ppc::wrf32(c,0x806C6158,0.92f); ppc::wrf32(c,0x806C6444,0.02f);
            ppc::wrf64(c,0x806C6B50,0.008); ppc::wrf32(c,0x806C8A18,0.2f);
            float light=1,uv=0,texture=0,history=0,ui_velocity=0;
            ppc::wrf32(c,0x806C9CF4,-98.f);
            float texture_layers[10][2];
            for(auto& layer:texture_layers) layer[0]=layer[1]=0.9f;
            ppc::wrf32(c,0x806C6B90,0.00056f); ppc::wrf32(c,0x806C6B8C,0.005f);
            ppc::wrf32(c,0x806C6B98,0.001f); ppc::wrf32(c,0x806C6B94,0.01f);
            unsigned history_samples=0;
            for(unsigned i=0;i<hz;++i) {
                run(0x804D3318,2,hz==30 ? 2 : 1);
                light*=ppc::rdf32(c,0x806C6158);
                uv+=ppc::rdf32(c,0x806C6444);
                ui_velocity+=ppc::rdf32(c,0x806C9CF4)/2.f;
                texture=float(double(texture)+ppc::rdf64(c,0x806C6B50));
                if(texture>1) texture-=1;
                history+=ppc::rdf32(c,0x806C8A18);
                while(history>1) { history-=1; ++history_samples; }
                for(unsigned layer=0;layer<10;++layer) {
                    texture_layers[layer][0]-=std::fma(ppc::rdf32(c,0x806C6B90),float(layer),ppc::rdf32(c,0x806C6B8C));
                    texture_layers[layer][1]-=std::fma(ppc::rdf32(c,0x806C6B98),float(layer),ppc::rdf32(c,0x806C6B94));
                }
            }
            CHECK(std::fabs(light-std::pow(0.92f,30))<1e-5f);
            CHECK(std::fabs(uv-0.6f)<1e-5f);
            CHECK(std::fabs(ui_velocity+1470.f)<0.01f);
            CHECK(std::fabs(texture-0.24f)<1e-5f);
            CHECK(std::fabs(float(history_samples)+history-6.f)<1e-4f);
            for(unsigned layer=0;layer<10;++layer) {
                CHECK(std::fabs(texture_layers[layer][0]-(0.9f-30*std::fma(0.00056f,float(layer),0.005f)))<2e-5f);
                CHECK(std::fabs(texture_layers[layer][1]-(0.9f-30*std::fma(0.001f,float(layer),0.01f)))<2e-5f);
            }
            ppc::wrf32(c,0x806C4EA8,0.12f);
            float slow_motion=1;
            for(unsigned i=0;i<hz/6;++i) {
                run(0x804D3318,2,hz==30 ? 2 : 1);
                slow_motion=std::max(0.005f,slow_motion-ppc::rdf32(c,0x806C4EA8));
            }
            CHECK(std::fabs(slow_motion-0.4f)<1e-5f);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1);
            CHECK(ppc::rdf32(c,0x806C6158)==1 && ppc::rdf32(c,0x806C6444)==0);
            CHECK(ppc::rdf64(c,0x806C6B50)==0 && ppc::rdf32(c,0x806C8A18)==0);
            CHECK(ppc::rdf32(c,0x806C4EA8)==0);
            CHECK(ppc::rdf32(c,0x806C9CF4)==0);
            CHECK(ppc::rdf32(c,0x806C6B90)==0 && ppc::rdf32(c,0x806C6B8C)==0);
            CHECK(ppc::rdf32(c,0x806C6B98)==0 && ppc::rdf32(c,0x806C6B94)==0);
            uc::game_pacing_set_paused(false);
            uc::game_pacing_set_mode({false,0}); run(0x804D3318,2,2);
            CHECK(ppc::rdf32(c,0x806C6158)==0.92f && ppc::rdf32(c,0x806C6444)==0.02f);
            CHECK(ppc::rdf64(c,0x806C6B50)==0.008 && ppc::rdf32(c,0x806C8A18)==0.2f);
            CHECK(ppc::rdf32(c,0x806C4EA8)==0.12f);
            CHECK(ppc::rdf32(c,0x806C9CF4)==-98.f);
            CHECK(ppc::rdf32(c,0x806C6B90)==0.00056f && ppc::rdf32(c,0x806C6B8C)==0.005f);
            CHECK(ppc::rdf32(c,0x806C6B98)==0.001f && ppc::rdf32(c,0x806C6B94)==0.01f);
        }
    }
    {   // Weapon history length is constant while fade still advances on every update.
        const uint32_t object=0x80050000;
        c.r[2]=0x806CC700; c.r[13]=0x806CA620;
        ppc::wrf32(c,c.r[2]-30472,0); ppc::wrf32(c,c.r[2]-30464,1); ppc::wrf32(c,c.r[2]-30388,0.25f);
        ppc::wr32(c,c.r[13]-27464,0x80061000);
        ppc::wr32(c,object+1172,0); ppc::wr32(c,0x80060000+6536,0);
        for(unsigned hz : {30u,60u,120u,240u,480u}) for(bool active : {false,true}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz; weapon_active=active; weapon_samples=0;
            ppc::wrf32(c,object+1184,0.5f); ppc::wr32(c,object+1180,0);
            auto update=[&] {
                c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=object;
                for(unsigned reg=27;reg<=31;++reg) c.r[reg]=reg*9;
                for(unsigned reg=30;reg<=31;++reg) { c.f[reg]=reg+0.25; c.ps1[reg]=reg+0.5; }
                hle_WeaponTrailStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
                for(unsigned reg=27;reg<=31;++reg) CHECK(c.r[reg]==reg*9);
                for(unsigned reg=30;reg<=31;++reg) CHECK(c.f[reg]==reg+0.25 && c.ps1[reg]==reg+0.5);
            };
            for(unsigned frame=0;frame<hz;++frame) {
                run(0x804D3318,2,hz==30 ? 2 : 1); ppc::wrf32(c,0x804D331C,measured_step); update();
            }
            CHECK(weapon_samples==30 && ppc::rd32(c,object+1180)==6);
            CHECK(std::fabs(ppc::rdf32(c,object+1184)-(active ? 0.75f : 0.25f))<1e-5f);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1);
            const auto before=ppc::rd32(c,object+1184); update();
            CHECK(weapon_samples==30 && ppc::rd32(c,object+1184)==before);
            uc::game_pacing_set_paused(false);
            measured_step=0.1f; uc::game_pacing_set_mode({true,0}); run(0x804D3318,2,1);
            ppc::wrf32(c,0x804D331C,measured_step); update(); CHECK(weapon_samples==33);
        }
    }
    {   // Timeline event comparisons see elapsed nominal frames, with original ABI and default event scale intact.
        const uint32_t object=0x80050000;
        c.r[2]=0x806CC700; ppc::wrf32(c,c.r[2]-13524,1); ppc::wrf32(c,c.r[2]-13520,0);
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            ppc::wrf32(c,object+42312,0); timeline_events=0;
            auto update=[&] {
                c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=object;
                for(unsigned reg=21;reg<=31;++reg) c.r[reg]=reg*7;
                for(unsigned reg=29;reg<=31;++reg) { c.f[reg]=reg+0.25; c.ps1[reg]=reg+0.5; }
                hle_TimelineStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
                for(unsigned reg=21;reg<=31;++reg) CHECK(c.r[reg]==reg*7);
                for(unsigned reg=29;reg<=31;++reg) CHECK(c.f[reg]==reg+0.25 && c.ps1[reg]==reg+0.5);
            };
            for(unsigned frame=0;frame<hz;++frame) {
                run(0x804D3318,2,hz==30 ? 2 : 1); update();
                CHECK(timeline_events==unsigned(std::floor((frame+1)*30.0/hz/10)));
            }
            CHECK(ppc::rdf32(c,object+42312)==30);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1); update();
            CHECK(ppc::rdf32(c,object+42312)==30 && timeline_events==3);
            uc::game_pacing_set_paused(false);
        }
    }
    {   // Angular drag has the same duration for positive and negative rotation, including pause and exception restoration.
        const uint32_t object=0x80050000;
        for(unsigned hz : {30u,60u,120u,240u,480u}) for(float direction : {-1.f,1.f}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            ppc::wrf32(c,object+68,direction*10); ppc::wrf32(c,object+108,0.1f);
            auto update=[&] {
                c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=object;
                hle_UiPhysicsStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
                CHECK(ppc::rdf32(c,object+108)==0.1f);
            };
            for(unsigned frame=0;frame<hz;++frame) { run(0x804D3318,2,hz==30 ? 2 : 1); update(); }
            CHECK(std::fabs(ppc::rdf32(c,object+68)-direction*7)<0.0002f);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1);
            const auto before=ppc::rd32(c,object+68); update();
            CHECK(ppc::rd32(c,object+68)==before);
            ui_fail=true;
            try { update(); CHECK(false); } catch(const std::runtime_error&) {}
            ui_fail=false; CHECK(ppc::rdf32(c,object+108)==0.1f);
            uc::game_pacing_set_paused(false);
        }
    }
    {   // The second model-history path divides two dt-scaled quantities; retain elapsed time after that division.
        c.r[2]=0x806CC700;
        ppc::wrf32(c,c.r[2]-9528,1.f); ppc::wrf32(c,c.r[2]-9524,0.f);
        ppc::wrf32(c,0x806C6EF8,10.f);
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            float phase[2]={0,0}; unsigned samples[2]={0,0};
            for(unsigned frame=0;frame<hz;++frame) {
                run(0x804D3318,2,hz==30 ? 2 : 1);
                ppc::wrf32(c,0x804D331C,measured_step);
                c.f[27]=20.f*measured_step; c.f[28]=10.f*measured_step;
                for(unsigned child=0;child<2;++child) {
                    c.r[3]=0x80041000; c.lr=0x801929B0;
                    hle_ModelHistoryIdentity(c);
                    CHECK(ppc::rdf32(c,0x80041000)==1 && ppc::rdf32(c,0x80041004)==0);
                    phase[child]+=float(c.f[28])/float(c.f[27]);
                    while(phase[child]>1) { phase[child]-=1; ++samples[child]; }
                }
            }
            for(unsigned child=0;child<2;++child) CHECK(std::fabs(samples[child]+phase[child]-15.f)<1e-4f);
            c.f[28]=123; c.lr=0x80100000; hle_ModelHistoryIdentity(c); CHECK(c.f[28]==123);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1);
            c.r[3]=0x80041000; c.lr=0x801929B0; hle_ModelHistoryIdentity(c); CHECK(c.f[28]==0);
            uc::game_pacing_set_paused(false);
        }
    }
    {   // Trail samples retain their duration; camera projection still runs between sample boundaries.
        c.r[13]=0x806CA620;
        const auto update=[&] {
            c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x80050000;
            hle_TrailHistoryStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000 && c.r[3]==1);
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            trail_samples=trail_projections=0; ppc::wr32(c,c.r[13]-26960,0);
            for(unsigned frame=0;frame<hz;++frame) {
                run(0x804D3318,2,hz==30 ? 2 : 1); update();
                CHECK(ppc::rd32(c,c.r[13]-26960)==0);
            }
            CHECK(trail_samples==30 && trail_projections==hz);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1); update();
            CHECK(trail_samples==30 && trail_projections==hz+1);
            trail_fail=true; bool threw=false;
            try { update(); } catch(const std::runtime_error&) { threw=true; }
            CHECK(threw && ppc::rd32(c,c.r[13]-26960)==0); trail_fail=false;
            uc::game_pacing_set_paused(false);
        }
        uc::game_pacing_init(true); measured_step=0.1f; trail_samples=trail_projections=0;
        run(0x804D3318,2,1); update(); CHECK(trail_samples==3 && trail_projections==3);
        ppc::wr32(c,c.r[13]-26960,7); update();
        CHECK(trail_samples==3 && trail_projections==4 && ppc::rd32(c,c.r[13]-26960)==7);
    }
    {   // Rumble pattern bytes last one nominal frame each, including pause and catch-up.
        const auto update=[&] {
            c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x80050000;
            hle_RumbleStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
            CHECK(c.r[3]==unsigned(ppc::rd8(c,0x80050000)!=0));
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            rumble_ticks=0; ppc::wr8(c,0x80050000,1);
            for(unsigned frame=0;frame<hz;++frame) { run(0x804D3318,2,hz==30 ? 2 : 1); update(); }
            CHECK(rumble_ticks==30);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1); update();
            CHECK(rumble_ticks==30);
            ppc::wr8(c,0x80050000,0); update(); CHECK(rumble_ticks==30);
            uc::game_pacing_set_paused(false);
        }
        uc::game_pacing_init(true); measured_step=0.1f; rumble_ticks=0; ppc::wr8(c,0x80050000,1);
        run(0x804D3318,2,1); update(); CHECK(rumble_ticks==3);
    }
    {   // Staggered actor actions keep their original three-frame delay while normal updates remain smooth.
        const auto update=[&] {
            c.r[1]=0x817D0000; c.lr=0x80100000; c.r[31]=987; c.r[3]=0x80050000;
            hle_DeferredActorStep(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000 && c.r[31]==987);
        };
        for(unsigned hz : {30u,60u,120u,240u,480u}) {
            uc::game_pacing_init(hz>30); measured_step=1.f/hz;
            ppc::wr32(c,0x80050000+8488,2); deferred_fired=deferred_updates=0;
            for(unsigned frame=1;frame<=hz/10;++frame) {
                run(0x804D3318,2,hz==30 ? 2 : 1); update();
                CHECK(deferred_fired==(frame==hz/10 ? 1u : 0u));
                CHECK(deferred_updates==frame);
            }
            CHECK(ppc::rd32(c,0x80050000+8488)==0xFFFFFFFF);
            update(); CHECK(deferred_fired==1);
            ppc::wr32(c,0x80050000+8488,0);
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1); update();
            CHECK(deferred_fired==1 && ppc::rd32(c,0x80050000+8488)==0);
            uc::game_pacing_set_paused(false);
        }
        uc::game_pacing_init(true); measured_step=0.1f;
        ppc::wr32(c,0x80050000+8488,2); deferred_fired=0;
        run(0x804D3318,2,1); update(); CHECK(deferred_fired==1);
    }
    {   // A camera history delay measures nominal frames, including fractional and multi-frame updates.
        const uint32_t history=0x80040000,input=0x80041000;
        const auto seed=[&](unsigned delay) {
            ppc::wr32(c,history+12,0); ppc::wr32(c,history+400,0);
            ppc::wr8(c,history+404,1); ppc::wr32(c,history+408,delay);
            for(unsigned slot=0;slot<32;++slot) for(unsigned axis=0;axis<3;++axis)
                ppc::wrf32(c,history+16+slot*12+axis*4,float(int(slot)-31)*(axis+1));
        };
        const auto sample=[&](float time,unsigned delay) {
            for(unsigned axis=0;axis<3;++axis) ppc::wrf32(c,input+axis*4,time*(axis+1));
            c.r[1]=0x817D0000; c.r[3]=history; c.r[4]=input; c.r[31]=987; c.lr=0x80123456;
            hle_CameraHistoryStep(c);
            CHECK(c.r[1]==0x817D0000 && c.r[31]==987 && c.lr==0x80123456);
            const float lag=float(delay ? delay-1 : 31);
            for(unsigned axis=0;axis<3;++axis) CHECK(std::fabs(camera_output[axis]-(time-lag)*(axis+1))<0.001f);
        };
        ppc::wrf32(c,0x804D3334,60.f);
        for(unsigned hz : {30u,60u,120u,240u,480u}) for(unsigned delay : {0u,1u,4u,18u,31u}) {
            uc::game_pacing_init(hz>30); seed(delay); measured_step=1.f/hz;
            for(unsigned i=1;i<=hz*2;++i) {
                run(0x804D3318,2,hz==30 ? 2 : 1);
                sample(float(i)*30/hz,delay);
            }
            CHECK(ppc::rd32(c,history+400)==(60&31));
            uc::game_pacing_set_paused(true); run(0x804D3318,2,hz==30 ? 2 : 1);
            sample(60,delay); CHECK(ppc::rd32(c,history+400)==(60&31));
            uc::game_pacing_set_paused(false);
        }
        uc::game_pacing_init(true); seed(18); float elapsed=0;
        for(unsigned repeat=0;repeat<20;++repeat) for(float step : {0.25f,0.5f,1.25f,0.125f,2.875f}) {
            measured_step=step/30; run(0x804D3318,2,1); elapsed+=step;
            sample(elapsed,18);
        }
    }
    // The falling bat adds a per-nominal-frame velocity to position, but already integrates gravity with dt.
    auto fall_add=[&](uint32_t caller) {
        c.r[3]=c.r[4]=0x80007000; c.r[5]=0x80007100; c.lr=caller;
        hle_VectorAddStep(c);
        CHECK(c.r[3]==0x80007000 && c.r[4]==0x80007000 && c.r[5]==0x80007100 && c.lr==caller);
    };
    ppc::wrf32(c,0x804D3334,60.f);
    for(unsigned hz : {30u,60u,120u,240u,480u}) {
        uc::game_pacing_init(hz>30);
        measured_step=1.f/hz; run(0x804D3318,2,hz==30 ? 2 : 1);
        ppc::wrf32(c,0x804D3318+4,measured_step);
        for(unsigned axis=0;axis<3;++axis) ppc::wrf32(c,0x80007000+axis*4,0);
        {
            const uint32_t world=0x80030000,body=0x80030100;
            ppc::wr32(c,world,body); ppc::wr32(c,body+4,0); ppc::wr32(c,body+24,16);
            ppc::wrf32(c,body+328,0.0025f); ppc::wrf32(c,body+332,0.0025f);
            ppc::wr32(c,body+340,10); ppc::wr32(c,body+348,10);
            ppc::wrf32(c,body+336,0); ppc::wrf32(c,body+344,0);
            unsigned steps=0;
            while(!(ppc::rd32(c,body+24)&4) && steps<1000) {
                c.r[3]=world; c.f[1]=measured_step/4; hle_PhysicsAutoDisable(c); ++steps;
            }
            CHECK(steps==11*(hz/30)); // Captured bodies use 10 quiet steps and a zero seconds threshold.
            CHECK(std::fabs(ppc::rdf32(c,body+344)+11.f/120)<1e-6f);
            auto sleep_step=[&] { c.r[3]=world; c.f[1]=measured_step/4; hle_PhysicsAutoDisable(c); };
            auto wake=[&] {
                ppc::wr32(c,body+24,16); ppc::wr32(c,body+348,10);
                ppc::wrf32(c,body+344,0);
            };
            wake(); sleep_step();
            ppc::wrf32(c,body+248,1); sleep_step();
            CHECK(ppc::rd32(c,body+348)==10 && ppc::rdf32(c,body+344)==0);
            ppc::wrf32(c,body+248,0);
            for(unsigned i=0;i<hz/30;++i) sleep_step();
            CHECK(ppc::rd32(c,body+348)==9); // Motion clears any fractional quiet period.
            wake(); sleep_step();
            c.r[1]=0x817D0000; c.r[3]=body; c.lr=0x80100000;
            hle_PhysicsBodyDestroy(c);
            CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000);
            // Reusing an address with exactly the old guest bytes must not inherit its fractional period.
            for(unsigned i=1;i<hz/30;++i) sleep_step();
            CHECK(ppc::rd32(c,body+348)==(hz==30 ? 9u : 10u));
            wake(); ppc::wrf32(c,body+336,0.2f); ppc::wrf32(c,body+344,0.2f);
            for(unsigned i=0;i<11*(hz/30);++i) sleep_step();
            CHECK(!(ppc::rd32(c,body+24)&4)); // Seconds threshold remains an independent gate.
            ppc::wrf32(c,body+336,0);
        }
        ppc::wrf32(c,0x80007100,5); ppc::wrf32(c,0x80007104,-3); ppc::wrf32(c,0x80007108,2);
        for(unsigned i=0;i<hz;++i) fall_add(0x801C387C);
        CHECK(std::fabs(ppc::rdf32(c,0x80007000)-150.f)<0.005f);
        CHECK(std::fabs(ppc::rdf32(c,0x80007004)+90.f)<0.005f);
        CHECK(std::fabs(ppc::rdf32(c,0x80007008)-60.f)<0.005f);
        CHECK(ppc::rdf32(c,0x80007100)==5.f && ppc::rdf32(c,0x80007104)==-3.f && ppc::rdf32(c,0x80007108)==2.f);
        // The subsequent gravity addition and every unrelated vector addition retain their original values.
        fall_add(0x801C38F4);
        CHECK(std::fabs(ppc::rdf32(c,0x80007000)-155.f)<0.005f);
        // A nominal interpolation step must have the same residual after any subdivision.
        for(float progress : {0.f,0.01f,0.25f,0.5f,0.75f,1.f}) {
            c.f[1]=progress; c.lr=0x80123456; hle_JumpBlendStep(c);
            const float original=float(c.f[1]);
            double residual=1;
            for(unsigned i=0;i<hz/30;++i) {
                c.f[1]=progress; c.lr=0x80200B8C; hle_JumpBlendStep(c);
                residual*=1-c.f[1]; CHECK(c.lr==0x80200B8C);
            }
            CHECK(std::fabs(residual-(1-original))<1e-6);
        }
        for(uint16_t state : {31,32,33,34}) {
            const uint32_t actor=0x80009000;
            ppc::wr16(c,actor+8174,state); ppc::wrf32(c,actor+8176,0.5f);
            ppc::wrf32(c,actor+8208,0); ppc::wrf32(c,actor+8672,0); ppc::wrf32(c,actor+8668,100);
            for(unsigned i=0;i<hz;++i) {
                c.r[1]=0x817D0000; c.r[3]=actor; c.lr=0x801C4F10; hle_FlightPhaseStep(c);
                CHECK(c.r[1]==0x817D0000 && c.lr==0x801C4F10 && ppc::rdf32(c,actor+8176)==0.5f);
            }
            CHECK(std::fabs(ppc::rdf32(c,actor+8208)-(state==32 || state==34 ? 15.f : 1.f))<1e-5f);
            CHECK(std::fabs(ppc::rdf32(c,actor+8672)-1.f)<1e-5f);
        }
        if(hz==480) {
            uc::game_pacing_set_paused(true); run(0x804D3318,2,1);
            const auto quiet_count=ppc::rd32(c,0x80030100+348);
            const auto quiet_time=ppc::rd32(c,0x80030100+344);
            const auto body_flags=ppc::rd32(c,0x80030100+24);
            c.r[3]=0x80030000; c.f[1]=0;
            hle_PhysicsAutoDisable(c);
            CHECK(ppc::rd32(c,0x80030100+348)==quiet_count && ppc::rd32(c,0x80030100+344)==quiet_time);
            CHECK(ppc::rd32(c,0x80030100+24)==body_flags);
            const float before=ppc::rdf32(c,0x80007000);
            fall_add(0x801C387C); CHECK(ppc::rdf32(c,0x80007000)==before);
            c.f[1]=1; c.lr=0x80200B8C; hle_JumpBlendStep(c); CHECK(c.f[1]==0);
            c.r[3]=0x80009000; ppc::wr16(c,c.r[3]+8174,32); ppc::wrf32(c,c.r[3]+8208,3);
            hle_FlightPhaseStep(c); CHECK(ppc::rdf32(c,0x80009000+8208)==3);
            c.r[3]=0x80009000; phase_fail=true; rejected=false;
            try { hle_FlightPhaseStep(c); } catch(const std::runtime_error&) { rejected=true; }
            CHECK(rejected && ppc::rdf32(c,0x80009000+8176)==0.5f); phase_fail=false;
            uc::game_pacing_set_paused(false);
        }
    }
}
