// Exact architectural oracle for all three entries of game function 80137F78.
#include "leaf_fault_guard.h"
#include "ppc_fixture.h"
#include "rollback_memory.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <random>
using namespace ppc;
void hle_MatrixTranspose4(Ctx&);
void hle_MatrixTranspose4Resume(Ctx&);
void hle_MatrixTranspose4Return(Ctx&);
static void reference(Ctx& c,int entry) {
    if(entry==0) {
        op_stwu(c,1,1,-128); op_mfspr(c,0,8); op_stw(c,0,1,132);
        op_stfd(c,31,1,112); op_psq_st(c,31,1,120,0,0);
        op_stfd(c,30,1,96); op_psq_st(c,30,1,104,0,0); op_addi(c,11,1,96);
        c.lr=0x80137F9C;
        for(int r=26;r<32;++r) op_stw(c,r,11,-4*(32-r));
    }
    if(entry!=2) {
    op_lfs(c,30,3,60); op_lfs(c,31,3,44); op_lfs(c,13,3,28); op_lfs(c,12,3,12);
    op_lfs(c,11,3,56); op_lfs(c,10,3,40); op_lfs(c,9,3,24); op_lfs(c,8,3,8);
    op_lfs(c,7,3,52); op_lfs(c,6,3,36); op_lfs(c,5,3,20); op_lfs(c,4,3,4);
    op_lfs(c,3,3,48); op_lfs(c,2,3,32); op_lfs(c,1,3,16); op_lfs(c,0,3,0);
    op_stfs(c,1,1,12); op_stfs(c,0,1,8); op_lwz(c,27,1,12); op_stfs(c,2,1,16);
    op_lwz(c,26,1,8); op_stfs(c,3,1,20); op_lwz(c,28,1,16); op_stfs(c,4,1,24);
    op_lwz(c,29,1,20); op_stfs(c,5,1,28); op_lwz(c,30,1,24); op_stfs(c,6,1,32);
    op_lwz(c,31,1,28); op_stfs(c,7,1,36); op_lwz(c,12,1,32); op_stfs(c,8,1,40);
    op_lwz(c,11,1,36); op_stfs(c,9,1,44); op_lwz(c,10,1,40); op_stfs(c,10,1,48);
    op_lwz(c,9,1,44); op_stfs(c,11,1,52); op_lwz(c,8,1,48); op_stfs(c,12,1,56);
    op_lwz(c,7,1,52); op_stfs(c,13,1,60); op_lwz(c,6,1,56); op_stfs(c,31,1,64);
    op_lwz(c,5,1,60); op_stfs(c,30,1,68); op_lwz(c,4,1,64); op_lwz(c,0,1,68);
    op_stw(c,26,3,0); op_stw(c,27,3,4); op_stw(c,28,3,8); op_stw(c,29,3,12);
    op_stw(c,30,3,16); op_stw(c,31,3,20); op_stw(c,12,3,24); op_stw(c,11,3,28);
    op_stw(c,10,3,32); op_stw(c,9,3,36); op_stw(c,8,3,40); op_stw(c,7,3,44);
    op_stw(c,6,3,48); op_stw(c,5,3,52); op_stw(c,4,3,56); op_stw(c,0,3,60);
        op_psq_l(c,31,1,120,0,0); op_lfd(c,31,1,112);
        op_psq_l(c,30,1,104,0,0); op_lfd(c,30,1,96); op_addi(c,11,1,96);
        c.lr=0x801380B4;
        for(int r=26;r<32;++r) op_lwz(c,r,11,-4*(32-r));
    }
    op_lwz(c,0,1,132); op_mtspr(c,0,8); op_addi(c,1,1,128);
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
static void reference_main(Ctx& c) { reference(c,0); }
int main() {
    Fix f;
    std::mt19937_64 rng(0x80137F78);
    std::array<uint8_t,0x6000> input1,want1;
    std::array<uint8_t,0x3000> input2,want2;
    for(auto& v:input1) v=uint8_t(rng());
    for(auto& v:input2) v=uint8_t(rng());
    constexpr std::array<uint32_t,12> edges{0,0x80000000,1,0x007FFFFF,0x00800000,0x7F7FFFFF,
        0x7F800000,0xFF800000,0x7FC12345,0x7F800001,0xFFC54321,0x3F800000};
    const std::array<void(*)(Ctx&),3> functions{hle_MatrixTranspose4,hle_MatrixTranspose4Resume,hle_MatrixTranspose4Return};
    unsigned cases=0;
    for(int entry=0;entry<3;++entry) for(unsigned n=0;n<1000;++n) {
        Ctx initial{}; initial.mem=f.c.mem;
        for(int i=0;i<32;++i) {
            initial.r[i]=uint32_t(rng());
            const uint64_t bits=rng(); std::memcpy(&initial.f[i],&bits,8);
            const uint64_t pair=rng(); std::memcpy(&initial.ps1[i],&pair,8);
        }
        for(auto& cr:initial.cr) cr=uint8_t(rng()&15);
        initial.lr=uint32_t(rng()); initial.ctr=uint32_t(rng()); initial.fpscr=uint32_t(rng());
        initial.xer_ca=1; initial.xer_so=1; initial.poll_count=n;
        initial.gqr[0]=(n%8==0)?0x00060007u:0;
        initial.r[1]=entry==0?0x80014060:0x80013FE0;
        const uint32_t frame=entry==0?initial.r[1]-128:initial.r[1];
        const std::array<uint32_t,7> addresses{0x80011000,0x90011FE0,0x80011FFF,frame+8,frame+12,frame+128,0xC0011000};
        initial.r[3]=addresses[n%addresses.size()];
        std::memcpy(hp(initial,0x80010000),input1.data(),input1.size());
        std::memcpy(hp(initial,0x90010000),input2.data(),input2.size());
        for(unsigned i=0;i<16;++i) {
            float value=float(int(rng()%200000)-100000)/32.f;
            uint32_t bits; std::memcpy(&bits,&value,4);
            if(n%4==0) bits=edges[(n/4+i)%edges.size()];
            wr32(initial,initial.r[3]+4*i,bits);
        }
        std::array<uint8_t,0x6000> before1;
        std::array<uint8_t,0x3000> before2;
        std::memcpy(before1.data(),hp(initial,0x80010000),before1.size());
        std::memcpy(before2.data(),hp(initial,0x90010000),before2.size());
        auto want=initial; reference(want,entry);
        std::memcpy(want1.data(),hp(want,0x80010000),want1.size());
        std::memcpy(want2.data(),hp(want,0x90010000),want2.size());
        std::memcpy(hp(initial,0x80010000),before1.data(),before1.size());
        std::memcpy(hp(initial,0x90010000),before2.data(),before2.size());
        auto got=initial; functions[entry](got);
        if(!same(want,got) || std::memcmp(want1.data(),hp(got,0x80010000),want1.size())
            || std::memcmp(want2.data(),hp(got,0x90010000),want2.size())) {
            std::fprintf(stderr,"transpose differs: entry %d case %u source %08X\n",entry,n,initial.r[3]);
            CHECK(false);
        }
        ++cases;
    }
    {
        uc::RollbackMemory memory(f.c.mem);
        memory.capture(true);
        for(int entry=0;entry<3;++entry) for(uint32_t source : {0x80011FE0u,0x90011FE0u}) {
            const auto base=f.c.mem; f.c={}; f.c.mem=base;
            f.c.r[1]=entry==0?0x80014060:0x80013FE0; f.c.r[3]=source;
            const uint32_t frame=entry==0?f.c.r[1]-128:f.c.r[1];
            std::memset(hp(f.c,frame),0xA5,136); uc::rollback_note_range(frame,136);
            for(unsigned i=0;i<16;++i) wr32(f.c,source+4*i,0x3F000000u+i*0x10000);
            for(unsigned i=0;i<32;++i) { f.c.f[i]=i+0.25; f.c.ps1[i]=i+0.5; }
            const auto before=memory.capture(true);
            const auto stamp=gx::watch_stamp();
            functions[entry](f.c);
            const auto after=memory.capture(true);
            if(entry!=2) {
                CHECK(!uc::RollbackMemory::difference(*before,*after).empty());
                CHECK(gx::written_since(source,1,stamp) && gx::written_since(source+63,1,stamp));
                CHECK(!gx::written_since(0x80018000,4,stamp));
            } else CHECK(uc::RollbackMemory::difference(*before,*after).empty());
            memory.restore(*before);
            CHECK(uc::RollbackMemory::difference(*before,*memory.capture(true)).empty());
        }
    }
    for(uint32_t source : {0xFFFFFFE0u,0x817FFFF0u,0x93FFFFF0u}) {
        Ctx initial{}; initial.mem=f.c.mem; initial.r[1]=0x80014060; initial.r[3]=source;
        initial.lr=0x12345678;
        for(unsigned i=0;i<32;++i) { initial.f[i]=i+0.25; initial.ps1[i]=i+0.5; }
        auto want=initial,got=initial;
        CHECK(!call_guarded(reference_main,want));
        CHECK(!call_guarded(hle_MatrixTranspose4,got));
        CHECK(same(want,got));
    }
    std::printf("transpose: %u exact register/memory cases, rollback and fault boundaries passed\n",cases);
}

