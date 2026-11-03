// Exact oracle of 80394374, including its observable quantized stack slots.
#include "leaf_fault_guard.h"
#include "ppc_fixture.h"
#include "rollback_memory.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
using namespace ppc;
void hle_SinCosFIdx(Ctx&);
void hle_Leaf_80394374(Ctx&);
void hle_MatrixEulerFinish(Ctx&);
static void reference(Ctx& c) {
    op_fabs(c,3,1,false); op_stwu(c,1,1,-16); op_lfs(c,0,2,-4936);
    for(;;) {
        op_fcmpo(c,0,3,0); op_cror(c,2,1,2);
        if(!crbit(c,2)) break;
        poll(c); op_fsubs(c,3,3,0,false);
    }
    op_psq_st(c,3,1,12,1,3); op_lhz(c,0,1,12); op_sth(c,0,1,8); op_psq_l(c,2,1,8,1,3);
    op_addis(c,6,0,-32701); op_lfs(c,0,2,-4932); op_rlwinm(c,0,0,4,20,27,false);
    op_addi(c,6,6,24120); op_fsubs(c,4,3,2,false); op_add(c,5,6,0,false); op_fcmpo(c,0,1,0);
    op_lfs(c,3,5,8); op_lfs(c,1,5,12); op_fmuls(c,3,4,3,false); op_lfsx(c,2,6,0);
    op_fmuls(c,1,4,1,false); op_lfs(c,0,5,4); op_fadds(c,2,2,3,false); op_fadds(c,0,0,1,false);
    if(crbit(c,0)) op_fneg(c,2,2,false);
    op_stfs(c,2,3,0); op_stfs(c,0,4,0); op_addi(c,1,1,16);
}
static void reference_euler(Ctx& c) {
    op_lfs(c,1,1,20); op_or(c,31,3,31,false); op_lfs(c,0,2,-4888);
    op_fneg(c,1,1,false); op_stfs(c,1,31,32);
    op_lfs(c,2,1,8); op_lfs(c,1,1,16); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,0);
    op_lfs(c,2,1,12); op_lfs(c,1,1,16); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,16);
    op_lfs(c,2,1,16); op_lfs(c,1,1,28); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,36);
    op_lfs(c,2,1,16); op_lfs(c,1,1,24); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,40);
    op_lfs(c,2,1,28); op_lfs(c,1,1,8); op_lfs(c,4,1,24); op_fmuls(c,5,2,1,false);
    op_lfs(c,3,1,12); op_lfs(c,1,1,20); op_fmuls(c,2,4,3,false);
    op_fmuls(c,1,5,1,false); op_fsubs(c,1,1,2,false); op_stfs(c,1,31,4);
    op_lfs(c,1,1,20); op_fmuls(c,1,2,1,false); op_fsubs(c,1,1,5,false); op_stfs(c,1,31,24);
    op_lfs(c,2,1,24); op_lfs(c,1,1,8); op_lfs(c,4,1,28); op_fmuls(c,5,2,1,false);
    op_lfs(c,3,1,12); op_lfs(c,1,1,20); op_fmuls(c,2,4,3,false);
    op_fmuls(c,1,5,1,false); op_fadds(c,1,2,1,false); op_stfs(c,1,31,8);
    op_lfs(c,1,1,20); op_fmuls(c,1,2,1,false); op_stfs(c,0,31,12); op_stfs(c,0,31,28);
    op_fadds(c,1,5,1,false); op_stfs(c,0,31,44); op_stfs(c,1,31,20);
    op_psq_l(c,31,1,72,0,0); op_lfd(c,31,1,64);
    op_psq_l(c,30,1,56,0,0); op_lfd(c,30,1,48);
    op_lwz(c,31,1,44); op_lwz(c,0,1,84); op_mtspr(c,0,8); op_addi(c,1,1,80);
}
static bool same(const Ctx& a,const Ctx& b) {
    return !std::memcmp(a.r,b.r,sizeof a.r) && !std::memcmp(a.f,b.f,sizeof a.f)
        && !std::memcmp(a.ps1,b.ps1,sizeof a.ps1) && !std::memcmp(a.cr,b.cr,sizeof a.cr)
        && !std::memcmp(a.gqr,b.gqr,sizeof a.gqr) && !std::memcmp(a.spr,b.spr,sizeof a.spr)
        && a.xer_ca==b.xer_ca && a.xer_ov==b.xer_ov && a.xer_so==b.xer_so
        && a.lr==b.lr && a.ctr==b.ctr && a.msr==b.msr && a.fpscr==b.fpscr
        && a.reserve_addr==b.reserve_addr && a.poll_count==b.poll_count
        && a.native_depth==b.native_depth && a.mem==b.mem;
}
int main() {
    Fix f; std::mt19937_64 rng(0x80394374);
    std::array<uint8_t,0x8000> input1,input2,wanted1,wanted2;
    std::array<uint8_t,4096> table,wanted_table;
    for(auto& v:input1) v=uint8_t(rng());
    for(auto& v:input2) v=uint8_t(rng());
    for(unsigned i=0;i<table.size();i+=4) {
        const float value=float(int(rng()%200001)-100000)/100000.f;
        uint32_t bits; std::memcpy(&bits,&value,4); bits=_byteswap_ulong(bits);
        std::memcpy(table.data()+i,&bits,4);
    }
    for(unsigned n=0;n<4000;++n) {
        Ctx initial{}; initial.mem=f.c.mem;
        for(unsigned i=0;i<32;++i) {
            initial.r[i]=uint32_t(rng());
            uint64_t bits=rng(); std::memcpy(&initial.f[i],&bits,8);
            bits=rng(); std::memcpy(&initial.ps1[i],&bits,8);
        }
        for(auto& cr:initial.cr) cr=uint8_t(rng()&15);
        initial.lr=uint32_t(rng()); initial.ctr=uint32_t(rng()); initial.fpscr=uint32_t(rng());
        initial.xer_ca=1; initial.xer_so=1;
        initial.r[2]=0x80017000;
        const std::array<uint32_t,5> stacks{0x80014060,0x90014060,0xC0014060,
            0x80435E48,initial.r[2]-4936+8};
        initial.r[1]=stacks[n%stacks.size()];
        const std::array<uint32_t,7> outputs{0x80011000,0x90011FFF,0x80011001,
            initial.r[1]-8,initial.r[2]-4936,0x80435E40,0xC0011000};
        initial.r[3]=outputs[(n/5)%outputs.size()];
        initial.r[4]=initial.r[3]+(n%3==0?0:n%3==1?1:4);
        initial.gqr[0]=n%7==0?0x00060007:0;
        const std::array<uint32_t,5> formats{0x00050005,0,0x00040004,0x00070007,0x01050105};
        initial.gqr[3]=n%4==0?formats[(n/4)%formats.size()]:0x00050005;
        initial.f[1]=double(int(rng()%5100001)-2550000)/10000;
        if(n%11==0 && n%5<3) initial.f[1]=300.25;
        if(n%17==0) initial.f[1]=-0.0;
        if(n%5==3) initial.f[1]=0.25; // frame aliases the selected LUT entry
        std::memcpy(hp(initial,0x80010000),input1.data(),input1.size());
        std::memcpy(hp(initial,0x90010000),input2.data(),input2.size());
        std::memcpy(hp(initial,0x80435E38),table.data(),table.size());
        wr32(initial,initial.r[2]-4936,0x43800000); wr32(initial,initial.r[2]-4932,0);
        if(n%31==0) {
            const uint64_t bits=0x7FF8123456789ABCull;
            std::memcpy(&initial.f[1],&bits,8);
        }
        std::array<uint8_t,0x8000> before1,before2;
        std::array<uint8_t,4096> before_table;
        std::memcpy(before1.data(),hp(initial,0x80010000),before1.size());
        std::memcpy(before2.data(),hp(initial,0x90010000),before2.size());
        std::memcpy(before_table.data(),hp(initial,0x80435E38),before_table.size());
        auto want=initial,got=initial;
        reference(want);
        std::memcpy(wanted1.data(),hp(initial,0x80010000),wanted1.size());
        std::memcpy(wanted2.data(),hp(initial,0x90010000),wanted2.size());
        std::memcpy(wanted_table.data(),hp(initial,0x80435E38),wanted_table.size());
        std::memcpy(hp(initial,0x80010000),before1.data(),before1.size());
        std::memcpy(hp(initial,0x90010000),before2.data(),before2.size());
        std::memcpy(hp(initial,0x80435E38),before_table.data(),before_table.size());
        hle_SinCosFIdx(got);
        CHECK(same(want,got));
        CHECK(!std::memcmp(wanted1.data(),hp(initial,0x80010000),wanted1.size()));
        CHECK(!std::memcmp(wanted2.data(),hp(initial,0x90010000),wanted2.size()));
        CHECK(!std::memcmp(wanted_table.data(),hp(initial,0x80435E38),wanted_table.size()));
    }
    for(unsigned n=0;n<4000;++n) {
        Ctx initial{}; initial.mem=f.c.mem;
        for(unsigned i=0;i<32;++i) {
            initial.r[i]=uint32_t(rng());
            uint64_t bits=rng(); std::memcpy(&initial.f[i],&bits,8);
            bits=rng(); std::memcpy(&initial.ps1[i],&bits,8);
        }
        initial.r[1]=n%13==0?0xC0014000:n%11==0?0x90014000:0x80014000;
        initial.r[2]=0x80017000; initial.gqr[0]=n%7==0?0x00060007:0;
        const std::array<uint32_t,8> outputs{0x80011000,0x90011FE0,0x80011FFF,initial.r[1]+8,
            initial.r[1]+48,initial.r[1]+84,initial.r[2]-4888,0xC0011000};
        initial.r[31]=outputs[n%outputs.size()];
        std::memcpy(hp(initial,0x80010000),input1.data(),input1.size());
        std::memcpy(hp(initial,0x90010000),input2.data(),input2.size());
        for(unsigned i=8;i<=28;i+=4) {
            const float value=float(int(rng()%200001)-100000)/100000.f;
            uint32_t bits; std::memcpy(&bits,&value,4);
            if(n%8==0) bits=std::array<uint32_t,10>{0,0x80000000,1,0x007FFFFF,0x00800000,
                0x7F800000,0xFF800000,0x7FC12345,0x7F800001,0x7F7FFFFF}[(n/8+i)%10];
            wr32(initial,initial.r[1]+i,bits);
        }
        wr32(initial,initial.r[2]-4888,n%9==0?0x7FC12345:0);
        std::array<uint8_t,0x8000> before1,before2;
        std::memcpy(before1.data(),hp(initial,0x80010000),before1.size());
        std::memcpy(before2.data(),hp(initial,0x90010000),before2.size());
        auto want=initial,got=initial;
        reference_euler(want);
        std::memcpy(wanted1.data(),hp(initial,0x80010000),wanted1.size());
        std::memcpy(wanted2.data(),hp(initial,0x90010000),wanted2.size());
        std::memcpy(hp(initial,0x80010000),before1.data(),before1.size());
        std::memcpy(hp(initial,0x90010000),before2.data(),before2.size());
        hle_MatrixEulerFinish(got);
        CHECK(same(want,got));
        CHECK(!std::memcmp(wanted1.data(),hp(initial,0x80010000),wanted1.size()));
        CHECK(!std::memcmp(wanted2.data(),hp(initial,0x90010000),wanted2.size()));
    }
    {
        uc::RollbackMemory memory(f.c.mem);
        f.c.r[1]=0x80014060; f.c.r[2]=0x80017000;
        f.c.r[3]=0x90011FFF; f.c.r[4]=0x90012003; f.c.f[1]=-123.75;
        f.c.gqr[3]=0x00050005;
        wr32(f.c,f.c.r[2]-4936,0x43800000); wr32(f.c,f.c.r[2]-4932,0);
        const auto before=memory.capture(true); const auto stamp=gx::watch_stamp();
        hle_SinCosFIdx(f.c);
        CHECK(gx::written_since(0x90011FFF,1,stamp) && gx::written_since(0x90012006,1,stamp));
        CHECK(!gx::written_since(0x80018000,4,stamp));
        CHECK(!uc::RollbackMemory::difference(*before,*memory.capture(true)).empty());
        memory.restore(*before);
        CHECK(uc::RollbackMemory::difference(*before,*memory.capture(true)).empty());
    }
    {
        uc::RollbackMemory memory(f.c.mem);
        f.c.r[1]=0x80014000; f.c.r[2]=0x80017000; f.c.r[31]=0x90011FE0; f.c.gqr[0]=0;
        for(unsigned i=8;i<=28;i+=4) wr32(f.c,f.c.r[1]+i,0x3F000000);
        wr32(f.c,f.c.r[2]-4888,0);
        const auto before=memory.capture(true); const auto stamp=gx::watch_stamp();
        hle_MatrixEulerFinish(f.c);
        CHECK(gx::written_since(0x90011FE0,1,stamp) && gx::written_since(0x9001200F,1,stamp));
        CHECK(!gx::written_since(0x80018000,4,stamp));
        CHECK(!uc::RollbackMemory::difference(*before,*memory.capture(true)).empty());
        memory.restore(*before);
        CHECK(uc::RollbackMemory::difference(*before,*memory.capture(true)).empty());
    }
    for(uint32_t out:{0xFFFFFFE0u,0x817FFFF0u,0x93FFFFF0u}) {
        Ctx initial{}; initial.mem=f.c.mem;
        initial.r[1]=0x80014000; initial.r[2]=0x80017000; initial.r[31]=out;
        for(unsigned i=8;i<=28;i+=4) wr32(initial,initial.r[1]+i,0x3F000000);
        wr32(initial,initial.r[2]-4888,0);
        auto want=initial,got=initial;
        CHECK(!call_guarded(reference_euler,want)); CHECK(!call_guarded(hle_MatrixEulerFinish,got));
        CHECK(same(want,got));
    }
    // Preserve existing native fallback fault state as well as valid PPC behavior above.
    for(uint32_t out:{0xFFFFFFE0u,0x817FFFF0u,0x93FFFFF0u}) {
        Ctx initial{}; initial.mem=f.c.mem;
        initial.r[1]=0x80014060; initial.r[2]=0x80017000;
        initial.r[3]=out; initial.r[4]=out+4; initial.f[1]=0.25; initial.gqr[3]=0x00050005;
        wr32(initial,initial.r[2]-4936,0x43800000); wr32(initial,initial.r[2]-4932,0);
        auto want=initial,got=initial;
        const bool expected=call_guarded(hle_Leaf_80394374,want);
        CHECK(call_guarded(hle_SinCosFIdx,got)==expected);
        CHECK(same(want,got));
    }
    std::puts("Game math: SinCos 4000 + Euler 4000 exact cases, rollback and native fault parity passed");
}
