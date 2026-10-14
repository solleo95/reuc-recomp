#include "check.h"
#include "input_probe.h"
#include "ppc_fixture.h"
#include <algorithm>
#include <cstring>
int main() {
    ppc::Ctx empty{};
    uc::input_probe_init(false); uc::input_probe_sample(empty,{});
    CHECK(uc::input_probe_stats().samples==0);
    Fix f; auto& c=f.c; c.r[13]=0x80500000; c.r[3]=1;
    const uint32_t actor=0x80010000;
    ppc::wr32(c,c.r[13]-27464,actor);
    const uint32_t scene=0x80100000;
    ppc::wr32(c,c.r[13]-27304,scene); ppc::wr32(c,scene+0x6BD0,3);
    ppc::wr32(c,scene+0x6BEC,2);
    ppc::wr8(c,0x804B5CC8+0x1D4A0,1);
    for(unsigned i=0;i<12;++i)ppc::wr32(c,0x804B5CC8+0x1D4B0+i*4,uint32_t(-1));
    ppc::wr32(c,0x804B5CC8+0x1D4B0+3*4,2);
    ppc::wr32(c,0x80596460+0x14C,3);
    ppc::wr32(c,0x80596460+0x148,2); ppc::wr32(c,0x805965C0+0x148,99);
    ppc::wr8(c,0x804B5CC8+0x234C+5,1); ppc::wr32(c,0x804B5CC8+0x234C+0x48,0x40000000);
    ppc::wr32(c,actor+0x790,0); ppc::wr32(c,actor+0x630,2);
    ppc::wr32(c,actor+0x794,1);
    ppc::wr32(c,actor+0x798,0x1234);
    ppc::wr32(c,actor+0x79C,8);
    ppc::wr32(c,actor+0x48+0x480,7); ppc::wr32(c,actor+0x48+0x48C,12);
    ppc::wr32(c,actor+0x48+0x490,1);
    ppc::wr32(c,0x8044A808+4,uc::kButtonA);
    ppc::wr8(c,0x8044A808+0x54,1);
    auto snapshot=uc::input_probe_snapshot(c);
    CHECK(snapshot.scene_valid && snapshot.scene_address==scene && snapshot.scene_words[1]==3 && snapshot.scene_words[2]==2);
    CHECK(snapshot.ui_valid && snapshot.ui_enabled && snapshot.ui_states[3]==2 && snapshot.ui_states[0]==uint32_t(-1));
    CHECK(snapshot.game_mode_valid && snapshot.game_mode==3 && snapshot.pause_state==2);
    CHECK(snapshot.tutorial_loaded && snapshot.tutorial_state==0x40000000);
    CHECK(snapshot.bindings_valid && snapshot.masks[0]==uc::kButtonA && snapshot.disabled[0]==1);
    CHECK(snapshot.players[0].valid && snapshot.players[0].address==actor);
    CHECK(snapshot.players[0].mode_630==2 && snapshot.players[0].state_794==1 && snapshot.players[0].flags_798==0x1234);
    CHECK(snapshot.players[0].qte_target_79C==8);
    CHECK(snapshot.players[0].weapon_words[0]==7 && snapshot.players[0].weapon_words[3]==12);
    const auto saved=c; unsigned char bytes[0x7A0]; std::memcpy(bytes,c.mem+(actor&0x3FFFFFFF),sizeof(bytes));
    uc::input_probe_init(true); uc::input_probe_sample(c,{}); uc::input_probe_sample(c,{});
    CHECK(uc::input_probe_stats().samples==2 && uc::input_probe_stats().records==1);
    ppc::wr32(c,actor+0x798,0x10001234); uc::input_probe_sample(c,{});
    CHECK(uc::input_probe_stats().changes==2 && uc::input_probe_stats().records==1);
    ppc::wr32(c,actor+0x798,0x1234);
    ppc::wr32(c,actor+0x48+0x490,2); uc::input_probe_sample(c,{});
    CHECK(uc::input_probe_stats().records==2);
    ppc::wr32(c,actor+0x48+0x490,1); uc::input_probe_sample(c,{});
    CHECK(uc::input_probe_stats().records==3);
    for(unsigned i=0;i<150;++i) { ppc::wr32(c,actor+0x630,i); uc::input_probe_sample(c,{}); }
    CHECK(uc::input_probe_stats().records==128 && uc::input_probe_stats().changes>128);
    CHECK(std::equal(std::begin(c.r),std::end(c.r),std::begin(saved.r)) && c.lr==saved.lr);
    ppc::wr32(c,actor+0x630,2);
    CHECK(std::memcmp(bytes,c.mem+(actor&0x3FFFFFFF),sizeof(bytes))==0);
    ppc::wr32(c,c.r[13]-27464,0xCC008000);
    CHECK(!uc::input_probe_snapshot(c).players[0].valid);
    ppc::wr32(c,c.r[13]-27464,0x817FFFF0);
    CHECK(!uc::input_probe_snapshot(c).players[0].valid);
    c.r[13]=8; CHECK(!uc::input_probe_snapshot(c).players[0].valid);
    uc::input_probe_init(false); uc::input_probe_sample(empty,{});
    CHECK(uc::input_probe_stats().samples==0);
}
