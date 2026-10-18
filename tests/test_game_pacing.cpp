#include "check.h"
#include "game_pacing.h"
#include "dispatch.h"
#include "ppc_fixture.h"
#include <cmath>
#include <stdexcept>

static uint32_t expected_target;
static uint32_t active_object,expected_elapsed,wait_calls;
static bool fail;
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
    ppc::wrf32(c,object+8,float(c.r[3])/60);
    c.lr=ppc::rd32(c,c.r[1]+36); c.r[31]=ppc::rd32(c,c.r[1]+28);
    c.r[1]=ppc::rd32(c,c.r[1]); c.r[3]=123;
}
// Original actor step getter body (800C7910): returns the playback speed times the nominal frame time, here a plain 1/30 s.
static void actor_step_body(ppc::Ctx& c) {
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000 && c.r[1]==0x817D0000-48 && c.lr==0x80100000);   // caller stack/LR intact, prologue lowered
    c.f[1]=double(1.0f/30.0f); c.r[1]+=48;
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
int main() {
    Fix fixture; auto& c=fixture.c;
    const uc::FnEntry entries[]={{0x80158208,original_tail},{0x8026E194,retrace},{0x800C7910,actor_step_body},{0x8014D36C,hud_body}};
    uc::dispatch_init(entries,4,0x800C7910,0x803A0000);
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
    CHECK(std::fabs(actor()-1.f/30.f)<1e-6f);
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
    }
    {   // the HUD name delay follows nominal frames: about half a frame per call here
        hud_fired=0; ppc::wr32(c,0x90005000+7752,0);
        int calls_to_fire=0;
        for(int i=1;i<=40;++i) { c.r[1]=0x817D0000; c.lr=0x80100000; c.r[3]=0x90005000; hle_HudNameDelay(c); CHECK(c.r[1]==0x817D0000 && c.lr==0x80100000); if(hud_fired && !calls_to_fire) calls_to_fire=i; }
        CHECK(hud_fired==1 && calls_to_fire>=19 && calls_to_fire<=21 && ppc::rd32(c,0x90005000+7752)==11);   // names wait ten nominal frames, not ten calls
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
}
