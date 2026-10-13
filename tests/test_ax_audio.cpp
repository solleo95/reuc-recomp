#include "check.h"
#include "ax_audio.h"
#include "ppc_fixture.h"
#include <array>
#include <cstring>
#include <chrono>
#include <thread>
int main() {
    Fix f; auto& c=f.c; constexpr uint32_t pb=0x80001000;
    auto setup=[&](uint16_t format,uint32_t start,uint32_t end) {
        std::memset(c.mem+0x1000,0,320);
        ppc::wr16(c,pb+0x10,1); ppc::wr32(c,pb+0x0C,5);
        ppc::wr16(c,pb+0x14,0x8000); ppc::wr16(c,pb+0x18,0x4000);
        ppc::wr16(c,pb+0x6A,0x8000); ppc::wr16(c,pb+0x70,format);
        ppc::wr32(c,pb+0x7A,start); ppc::wr32(c,pb+0x76,end);
        ppc::wr32(c,pb+0xA6,0x10000);
    };
    std::array<int32_t,192> mix{};
    ppc::wr16(c,0x80002000,1000); ppc::wr16(c,0x80002002,uint16_t(-1000));
    setup(10,0x1000,0x1001); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1000 && mix[1]==500 && mix[2]==-1000 && mix[3]==-500);
    CHECK(mix[4]==0 && ppc::rd16(c,pb+0x10)==0);
    mix={}; setup(10,0x1000,0x1001); ppc::wr32(c,pb+0x0C,2);
    uc::mix_ax_voice(c,pb,mix); CHECK(mix[0]==0 && mix[1]==500);
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x1000);
    uc::mix_ax_voice(c,pb,mix); CHECK(mix[190]==-1000 && ppc::rd16(c,pb+0x10)==1);
    // Streaming switches to a later buffer; loop address need not precede the old end.
    for(uint32_t i=0;i<96;++i) ppc::wr16(c,0x80002200+i*2,uint16_t(2000+i));
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x1100);
    uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1000 && mix[2]==-1000 && mix[4]==2000 && mix[6]==2001);
    CHECK(ppc::rd32(c,pb+0x7A)==0x115E && ppc::rd16(c,pb+0x10)==1);
    mix={}; setup(10,0x1100,0x1001);
    uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==2000 && mix[190]==2095 && ppc::rd16(c,pb+0x10)==1);
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x66000000);
    bool bad_loop=false;
    try { uc::mix_ax_voice(c,pb,mix); } catch(const std::exception&) { bad_loop=true; }
    CHECK(bad_loop);
    mix={}; ppc::wr8(c,0x80002000,0); ppc::wr8(c,0x80002001,0x1F);
    setup(0,0x4002,0x4003); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1 && mix[2]==-1 && ppc::rd16(c,pb+0x10)==0);
    mix={};
    for (uint32_t i=0;i<16;++i) ppc::wr8(c,0x80002000+i,0x11);
    ppc::wr8(c,0x80002000,0); ppc::wr8(c,0x80002008,1);
    setup(0,0x4002,0x401F); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1 && mix[26]==1 && mix[28]==2 && mix[54]==2 && mix[56]==0);
    CHECK(ppc::rd16(c,pb+0x10)==0);
    mix={}; ppc::wr8(c,0x80002000,0x80); setup(25,0x2000,0x2000); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==-32768 && mix[1]==-16384);
    setup(123,0x2000,0x2000); bool rejected=false;
    try { uc::mix_ax_voice(c,pb,mix); } catch (const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);

    uc::Sched sched(c.mem);
    uint64_t blocks=0;
    uc::audio_hle_init(sched,[&](std::span<const int16_t> pcm) { CHECK(pcm.size()==192); ++blocks; });
    auto* audio_thread=sched.create(0,16,[&](ppc::Ctx& active) {
        active.r[13]=0x80400000;
        ppc::wr32(active,active.r[13]-25524,0);
        ppc::wr32(active,active.r[13]-25636,0);
        hle_AIStartDMA(active);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sched.safe_point();
        // Each due 3ms interval owns 96 PCM samples, even after a delayed safe point.
        CHECK(blocks>=10);
        CHECK(uc::audio_stats().frames==blocks);
        uc::audio_hle_shutdown();
    },&c);
    sched.run(audio_thread);
}
