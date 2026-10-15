#include "check.h"
#include "test_state.h"
#include "input_probe.h"
#include "ppc_fixture.h"
#include <bit>
int main() {
    uc::InputProbeSnapshot s; s.pause_state=2; s.game_mode=3; s.tutorial_loaded=true; s.tutorial_state=0x00100000;
    s.menu_pages={0,1,0,7}; s.players[0].weapon_words[3]=12; s.hud_life=0.6f; s.scene_address=0x80BDF0A0; s.scene_valid=true; s.scene_words[3]=0x3F000000; /* 0.5 */ s.view_valid=true; s.view={1,0,0,10, 0,1,0,20, 0,0,1,30};
    s.enemy_count=2; s.enemies[0]={160,0x80200000,{500.f,-0.5f,1.5f},true,0x803F2B60,10.f}; s.enemies[1]={161,0x80201000,{497.5f,-0.5f,-9.f},false,0x803F2B60,-5.f}; s.ui_states[6]=5; s.ui_valid=true; s.players[0].valid=true; s.players[0].address=0x80010000; s.players[0].mode_630=1; s.players[0].state_794=1;
    uc::test_state_publish(s,2); CHECK(uc::test_state_latest().frame==0);   // disabled: nothing published
    uc::test_state_enable(true);
    uc::test_state_publish(s,2); uc::test_state_publish(s,1);
    auto f=uc::test_state_latest();
    CHECK(f.frame==2 && f.context==1 && f.ui[6]==5 && f.pause==2 && f.scene_mode==3 && f.tutorial==0x00100000 && f.player_mode==1);
    for(int i=0;i<300;++i) uc::test_state_action("Reload",true);
    auto actions=uc::test_state_take_actions(); CHECK(actions.size()==256 && actions.back().name=="Reload" && actions.back().frame==2);
    CHECK(uc::test_state_take_actions().empty());
    const auto json=uc::test_frame_json(f,4,2.5,30,60,0);
    CHECK(json.find(R"("type":"frame")")!=std::string::npos && json.find(R"("n":2)")!=std::string::npos);
    CHECK(json.find(R"("context":"menu")")!=std::string::npos && json.find(R"("speed":4)")!=std::string::npos);
    CHECK(json.find(R"("ui":[0,0,0,0,0,0,5,0,0,0,0,0])")!=std::string::npos);
    CHECK(json.find(R"("player":{"valid":true,"address":2147549184,)")!=std::string::npos);
    CHECK(f.enemies.size()==2 && f.enemies[0].id==160 && f.enemies[1].root[2]==-9.f && f.magazine==12 && f.view_valid);
    CHECK(json.find(R"("magazine":12,"life":0.60,"damage":0.500,"scene":2159931552})")!=std::string::npos);
    CHECK(json.find(R"("view":[1,0,0,10,0,1,0,20,0,0,1,30])")!=std::string::npos);
    CHECK(json.find(R"("enemies":[{"id":160,"cls":2151623520,"root":[500.000,-0.500,1.500],"head":true,"hp":10.0},{"id":161,"cls":2151623520,"root":[497.500,-0.500,-9.000],"head":false,"hp":-5.0}])")!=std::string::npos);
    uc::TestFrameState none; none.view_valid=false; const auto bare=uc::test_frame_json(none,1,0,0,0,0);
    CHECK(bare.find(R"("view":null)")!=std::string::npos && bare.find(R"("enemies":[])")!=std::string::npos && bare.find(R"("life":-1.00)")!=std::string::npos);
    CHECK(f.menu[1]==1 && json.find(R"("menu":[0,1,0,7])")!=std::string::npos);
    // --god: damage float zeroed, magazine and grenades topped up, only while enabled and only for valid objects.
    {
        Fix fixture; auto& c=fixture.c;
        uc::InputProbeSnapshot g; g.scene_valid=true; g.scene_address=0x80100000; g.players[0].valid=true; g.players[0].address=0x80010000;
        ppc::wr32(c,0x80100000+0x6C00,std::bit_cast<uint32_t>(30.0f)); ppc::wr32(c,0x80010000+0x4D4,3); ppc::wr32(c,0x8056F338,2);
        uc::test_state_apply_god(c,g);
        CHECK(ppc::rd32(c,0x80100000+0x6C00)==std::bit_cast<uint32_t>(30.0f) && ppc::rd32(c,0x80010000+0x4D4)==3 && ppc::rd32(c,0x8056F338)==2);   // off by default
        uc::test_state_set_god(true);
        uc::test_state_apply_god(c,g);
        CHECK(ppc::rd32(c,0x80100000+0x6C00)==0 && ppc::rd32(c,0x80010000+0x4D4)==99 && ppc::rd32(c,0x8056F338)==9);
        uc::InputProbeSnapshot none; ppc::wr32(c,0x80010000+0x4D4,3); uc::test_state_apply_god(c,none);   // nothing valid: nothing written
        CHECK(ppc::rd32(c,0x80010000+0x4D4)==3);
        uc::test_state_set_god(false);
    }
    // --skip-tutorials: entry of the original tutorial opener (r3 = tutorial object) gets the "disabled" gate byte and the closed state.
    {
        Fix fixture; auto& c=fixture.c;
        const uint32_t obj=0x804B5CC8+0x234C;
        c.r[3]=obj; ppc::wr8(c,obj+4,1); ppc::wr8(c,obj+5,1); ppc::wr32(c,obj+0x48,0);
        CHECK(uc::test_state_skip_tutorial(c)==-1 && ppc::rd8(c,obj+4)==1 && ppc::rd32(c,obj+0x48)==0);   // off by default: nothing written (-1 = not applied)
        uc::test_state_set_skip_tutorials(true);
        const int gate=uc::test_state_skip_tutorial(c);
        CHECK(gate==1 && ppc::rd8(c,obj+4)==0 && ppc::rd8(c,obj+5)==1 && ppc::rd32(c,obj+0x48)==0x00100000);
        uc::test_state_unskip_tutorial(c,obj,gate);   // at exit of the opener the gate byte is the game's again; the state stays closed
        CHECK(ppc::rd8(c,obj+4)==1 && ppc::rd32(c,obj+0x48)==0x00100000);
        c.r[3]=0; ppc::wr8(c,obj+4,1); CHECK(uc::test_state_skip_tutorial(c)==-1 && ppc::rd8(c,obj+4)==1);   // null object: nothing written
        uc::test_state_set_skip_tutorials(false);
    }
    return 0;
}
