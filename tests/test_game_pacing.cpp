#include "check.h"
#include "game_pacing.h"
#include "dispatch.h"
#include "ppc_fixture.h"
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
int main() {
    Fix fixture; auto& c=fixture.c;
    const uc::FnEntry entries[]={{0x80158208,original_tail},{0x8026E194,retrace}};
    uc::dispatch_init(entries,2,0x80158208,0x8026E198);
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
    run(0x804D3318,2,2);
    CHECK(uc::game_pacing_stats().calls==1 && uc::game_pacing_stats().adjusted==0);
    run(0x804D3318,2,3,2); // Original mode always waits at least once.
    uc::game_pacing_init(true);
    run(0x804D3318,2,1);
    CHECK(uc::game_pacing_stats().calls==1 && uc::game_pacing_stats().adjusted==1 && uc::game_pacing_stats().retraces==1);
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
