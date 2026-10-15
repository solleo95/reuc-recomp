#include "check.h"
#include "input_probe.h"
#include "ppc_fixture.h"
#include <algorithm>
#include <cmath>
#include <limits>
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
    ppc::wr32(c,0x804BB408,1); ppc::wr32(c,0x804C0574,1); ppc::wr32(c,0x804D0994,5);   // menu page-alive flags
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
    CHECK(snapshot.menu_pages[0]==1 && snapshot.menu_pages[1]==0 && snapshot.menu_pages[2]==1 && snapshot.menu_pages[3]==5);
    CHECK(snapshot.bindings_valid && snapshot.masks[0]==uc::kButtonA && snapshot.disabled[0]==1);
    CHECK(snapshot.players[0].valid && snapshot.players[0].address==actor);
    CHECK(snapshot.players[0].mode_630==2 && snapshot.players[0].state_794==1 && snapshot.players[0].flags_798==0x1234);
    CHECK(snapshot.players[0].qte_target_79C==8);
    CHECK(snapshot.players[0].weapon_words[0]==7 && snapshot.players[0].weapon_words[3]==12);
    // Actor table (id/pointer pairs): players and the two pool classes are skipped, other classes are enemies.
    {
        const uint32_t table=0x804421C4,zombie=0x80200000,hurt=0x80201000,pool=0x80202000;
        ppc::wr32(c,actor+0x10,0x803F2388);
        for(auto [object,flag]:{std::pair{zombie,1.0f},{hurt,0.1f}}) {
            ppc::wr32(c,object+0x10,0x803F2B60); ppc::wrf32(c,object+0x1058,flag); ppc::wrf32(c,object+0x3A4,object==zombie ? 10.f : -5.f);
            ppc::wrf32(c,object+0xF48,object==zombie ? 500.f : 497.5f); ppc::wrf32(c,object+0xF4C,-0.5f); ppc::wrf32(c,object+0xF50,object==zombie ? 1.5f : -9.f);
        }
        ppc::wr32(c,pool+0x10,0x803F1A40);
        const std::pair<uint32_t,uint32_t> entries[]={{2,actor},{160,zombie},{161,hurt},{5,pool},{6,0},{7,0xCC008000}};
        for(size_t i=0;i<std::size(entries);++i) { ppc::wr32(c,table+i*8,entries[i].first); ppc::wr32(c,table+i*8+4,entries[i].second); }
        for(unsigned r=0;r<3;++r) for(unsigned k=0;k<4;++k) ppc::wrf32(c,0x8044B4E0+r*16+k*4,r==k ? 1.f : (k==3 ? float(r+1)*10 : 0.f));
        ppc::wrf32(c,0x804B7960,0.6f);
        const auto s=uc::input_probe_snapshot(c);
        CHECK(s.enemy_count==2 && s.enemies[0].id==160 && s.enemies[0].address==zombie && s.enemies[1].id==161);
        CHECK(s.enemies[0].root[0]==500.f && s.enemies[0].root[1]==-0.5f && s.enemies[0].root[2]==1.5f && s.enemies[0].head_intact && !s.enemies[1].head_intact && s.enemies[0].actor_class==0x803F2B60 && s.enemies[0].health==10.f && s.enemies[1].health==-5.f);
        CHECK(s.view_valid && s.view[0]==1.f && s.view[3]==10.f && s.view[11]==30.f);
        CHECK(std::abs(s.hud_life-0.6f)<1e-6f);
        ppc::wrf32(c,0x8044B4E0,2.f);   // not a rotation: the camera is rejected, not guessed
        CHECK(!uc::input_probe_snapshot(c).view_valid);
        ppc::wr32(c,table+12,0);        // a freed slot disappears from the list
        CHECK(uc::input_probe_snapshot(c).enemy_count==1 && uc::input_probe_snapshot(c).enemies[0].id==161);
        ppc::wr32(c,table+12,zombie);
        ppc::wrf32(c,zombie+0xF48,std::numeric_limits<float>::quiet_NaN());   // garbage positions are skipped
        CHECK(uc::input_probe_snapshot(c).enemy_count==1 && uc::input_probe_snapshot(c).enemies[0].id==161);
        // Actors with ids beyond the table (the roof zombies, id 600+) are reachable only through the actor list: object+8 = next object + 8.
        const uint32_t late=0x80203000;
        ppc::wr32(c,late,600); ppc::wr32(c,late+0x10,0x803F2B60); ppc::wrf32(c,late+0x1058,1.f); ppc::wrf32(c,late+0x3A4,20.f);
        ppc::wrf32(c,late+0xF48,550.f); ppc::wrf32(c,late+0xF4C,41.f); ppc::wrf32(c,late+0xF50,3.f);
        ppc::wr32(c,actor+8,hurt+8); ppc::wr32(c,hurt+8,late+8); ppc::wr32(c,late+8,0);
        const auto listed=uc::input_probe_snapshot(c);
        CHECK(listed.enemy_count==2 && listed.enemies[0].id==161 && listed.enemies[1].id==600 && listed.enemies[1].address==late && listed.enemies[1].health==20.f);   // 161 is in both table and list: once
        ppc::wr32(c,late+8,actor+8);   // a cycle ends the walk
        CHECK(uc::input_probe_snapshot(c).enemy_count==2);
        ppc::wr32(c,actor+8,0);
    }
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
