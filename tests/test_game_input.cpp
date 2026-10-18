#include "check.h"
#include "game_input.h"
#include "input_probe.h"
#include "ppc_fixture.h"
#include "dispatch.h"
#include <bit>
static uint32_t actor_address=0x80010000;
static unsigned reloads=0,grenades=0,knives=0;
static uint32_t reserve=30,grenade_count=3;
static float knife_x=uc::kKnifeKeyDirectionX,knife_y=uc::kKnifeKeyDirectionY;
static uc::NativeInput* redirect_input=nullptr;
static void original_update(ppc::Ctx& c) {
    if(redirect_input) {
        auto* source=redirect_input;redirect_input=nullptr;
        const uint32_t next=0x80040000;
        ppc::wr32(c,c.r[13]-27464,next);ppc::wr32(c,next+0x794,1);
        ppc::wrf32(c,next+0x4F8+0xEC,1);ppc::wr32(c,next+0x48+0x48C,10);ppc::wr32(c,next+0x48+0x490,1);
        uc::game_input_sample(c,{});source->host_button({uc::PhysicalKind::key,21},true);source->host_button({uc::PhysicalKind::key,21},false);uc::game_input_sample(c,{});
    }
    c.r[1]=ppc::rd32(c,c.r[1]); c.r[3]=0x12345678;
}
static void capacity(ppc::Ctx& c) { c.r[3]=15; }
static void inventory(ppc::Ctx& c) { c.r[3]=c.r[5]==3 ? grenade_count : reserve; }
static void reload(ppc::Ctx& c) { ++reloads; CHECK(c.r[3]==actor_address+0x48); ppc::wr32(c,c.r[3]+0x48C,15); ppc::wr32(c,c.r[3]+0x490,2); }
static void grenade(ppc::Ctx& c) { ++grenades; --grenade_count; ppc::wrf32(c,c.r[3]+0xF0,.5f); }
static void knife(ppc::Ctx& c) {
    ++knives; CHECK(c.r[3]==actor_address+0x4F8 && c.r[4]==0);
    CHECK(ppc::rd32(c,c.r[3]+4+0x70)==2);
    CHECK(ppc::rdf32(c,c.r[3]+4+0xC)==knife_x && ppc::rdf32(c,c.r[3]+4+0x10)==knife_y);   // processed vector = stroke direction on screen
    ppc::wrf32(c,c.r[3]+0xF4,.25f);
}
static uint64_t bit(uc::Action a) { return uint64_t(1)<<unsigned(a); }
int main() {
    uc::InputProbeSnapshot s;
    CHECK(uc::game_input_context(s)==uc::InputContext::inactive);
    s.ui_valid=s.ui_enabled=true; s.ui_states.fill(uint32_t(-1));
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.players[0].valid=true; s.players[0].player_790=0; s.players[0].state_794=1;
    s.ui_states[2]=s.ui_states[3]=5; s.ui_states[11]=0;
    CHECK(uc::game_input_context(s)==uc::InputContext::gameplay);
    s.ui_states[6]=6; CHECK(uc::game_input_context(s)==uc::InputContext::gameplay);
    s.game_mode_valid=true; s.game_mode=3; CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.game_mode=0;
    s.pause_state=2; CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.pause_state=1;
    s.tutorial_loaded=true; s.tutorial_state=0x40000000;
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.tutorial_state=0x00100000; CHECK(uc::game_input_context(s)==uc::InputContext::gameplay);
    s.tutorial_loaded=false;
    s.scene_valid=true; s.scene_words[1]=1;
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.scene_words[1]=0;
    s.scene_words[0]=std::bit_cast<uint32_t>(.1f);
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.scene_words[0]=0;
    s.fade_words={std::bit_cast<uint32_t>(.25f),0,std::bit_cast<uint32_t>(1.f),std::bit_cast<uint32_t>(1.f),0x01000000,std::bit_cast<uint32_t>(1.f)};
    CHECK(uc::game_input_context(s)==uc::InputContext::gameplay);
    s.fade_words[0]=std::bit_cast<uint32_t>(1.f);
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.fade_words[0]=0; s.fade_words[1]=0x01000000;
    CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    s.scene_valid=false;
    s.ui_states[6]=uint32_t(-1); s.players[0].mode_630=2;
    CHECK(uc::game_input_context(s)==uc::InputContext::qte);
    s.ui_states[4]=7; CHECK(uc::game_input_context(s)==uc::InputContext::menu);
    uc::ActionFrame frame; frame.held=frame.pressed=bit(uc::Action::confirm);
    auto sample=uc::game_input_buttons(uc::InputContext::menu,frame);
    CHECK(sample.held==uc::kButtonA && sample.pressed==uc::kButtonA);
    frame.held=frame.pressed=bit(uc::Action::back);
    CHECK(uc::game_input_buttons(uc::InputContext::menu,frame).pressed==0x1000); // Original menu binding11.
    frame.held=frame.pressed=bit(uc::Action::interact)|bit(uc::Action::fire);
    sample=uc::game_input_buttons(uc::InputContext::gameplay,frame);
    CHECK(sample.held==(uc::kButtonA|uc::kButtonB));
    frame.held=0; frame.pressed=bit(uc::Action::reload)|bit(uc::Action::grenade)|bit(uc::Action::knife);
    CHECK(uc::game_input_buttons(uc::InputContext::gameplay,frame).pressed==0); // Semantic actions never become fake motion/button combos.
    frame.held=frame.pressed=bit(uc::Action::pause);
    CHECK(uc::game_input_buttons(uc::InputContext::gameplay,frame).pressed==0x10);
    frame.held=frame.pressed=bit(uc::Action::back);
    CHECK(uc::game_input_buttons(uc::InputContext::menu,frame).pressed==0x1000);
    frame={}; frame.released=bit(uc::Action::pause)|bit(uc::Action::fire);
    CHECK(uc::game_input_buttons(uc::InputContext::menu,frame).released==0x410);
    frame.held=bit(uc::Action::confirm); frame.released|=bit(uc::Action::interact);
    CHECK(!(uc::game_input_buttons(uc::InputContext::menu,frame).released&uc::kButtonA));
    Fix f; auto& c=f.c; const uint32_t binding=0x8044A808;
    ppc::wr32(c,binding+4+7*4,uc::kButtonB); ppc::wr8(c,binding+0x54+7,0);
    uc::NativeInput input; uc::game_input_init(&input);
    input.context(uc::InputContext::gameplay); input.focus(true);
    const uint32_t actor=0x80010000; c.r[13]=0x806CA620;
    c.r[30]=actor; // Original selection caller8006846C retains its parent in r30.
    ppc::wr32(c,c.r[13]-27464,actor); ppc::wr32(c,actor+0x790,0); ppc::wr32(c,actor+0x794,1);
    ppc::wr8(c,0x804B5CC8+0x1D4A0,1);
    for(unsigned i=0;i<12;++i)ppc::wr32(c,0x804B5CC8+0x1D4B0+i*4,uint32_t(-1));
    ppc::wr32(c,0x804B5CC8+0x1D4B0+2*4,5);
    input.host_button({uc::PhysicalKind::mouse,3},true);
    uc::InputSample raw; raw.valid=true; raw.x=.3f; raw.y=-.2f;
    auto mapped=uc::game_input_sample(c,raw);
    CHECK(mapped.held==uc::kButtonB && mapped.pressed==uc::kButtonB && mapped.x==.3f && mapped.y==-.2f);
    input.host_button({uc::PhysicalKind::mouse,3},false);
    mapped=uc::game_input_sample(c,raw); CHECK(mapped.held==0 && mapped.released==uc::kButtonB);
    input.host_wheel(2); mapped=uc::game_input_sample(c,raw);
    CHECK(mapped.pressed&uc::kNativeWeaponStep);
    for(unsigned i=0;i<2;++i) {
        c.r[3]=binding; c.r[4]=4; c.lr=0x80068510; hle_GameActionMask(c); CHECK(c.r[3]==uc::kNativeWeaponStep);
        c.r[3]=binding; c.r[4]=5; c.lr=0x8006854C; hle_GameActionMask(c); CHECK(c.r[3]==0);
    }
    c.r[3]=binding; c.r[4]=4; c.lr=0x80068510; hle_GameActionMask(c); CHECK(c.r[3]==0);
    input.host_wheel(1); uc::game_input_sample(c,raw); input.focus(false); input.focus(true);
    c.r[3]=binding; c.r[4]=4; c.lr=0x80068510; hle_GameActionMask(c); CHECK(c.r[3]==0);
    ppc::wr32(c,0x804B5CC8+0x1D4B0+4*4,7);
    uc::game_input_sample(c,raw);
    input.host_button({uc::PhysicalKind::mouse,3},true);
    mapped=uc::game_input_sample(c,raw); CHECK(mapped.pressed==0x1000);
    c.r[3]=binding; c.r[4]=7; c.lr=0x801AB590; hle_GameActionMask(c); CHECK(c.r[3]==0);
    c.r[3]=binding; c.r[4]=7; c.lr=0x80001000; hle_GameActionMask(c); CHECK(c.r[3]==uc::kButtonB);
    ppc::wr8(c,binding+0x54+7,1); c.r[3]=binding; c.r[4]=7; hle_GameActionMask(c); CHECK(c.r[3]==0);
    const uc::FnEntry entries[]={{0x8006C26C,original_update},{0x8006D980,reload},{0x801AB354,original_update},
        {0x801AB860,grenade},{0x801ABA00,knife},{0x801BD014,inventory},{0x801D431C,capacity},{0x80171D3C,original_update}};
    uc::dispatch_init(entries,std::size(entries),0x8006C26C,0x801D4320);
    ppc::wr32(c,0x804B5CC8+0x1D4B0+4*4,uint32_t(-1));
    uc::game_input_sample(c,raw); input.host_button({uc::PhysicalKind::mouse,3},false);
    ppc::wrf32(c,actor+0x4F8+0xEC,1);
    ppc::wr32(c,actor+0x48+0x480,0); ppc::wr32(c,actor+0x48+0x484,0);
    ppc::wr32(c,actor+0x48+0x48C,10); ppc::wr32(c,actor+0x48+0x490,1);
    const auto weapon_update=[&](unsigned allow=1) {c.r[1]=0x817D0000;c.lr=0x80100000;c.r[3]=actor+0x48;c.r[4]=allow;hle_GameWeaponUpdate(c);CHECK(c.r[1]==0x817D0000 && c.r[3]==0x12345678 && c.lr==0x80100000);};
    const auto owner_update=[&](unsigned allow=1) {c.r[1]=0x817D0000;c.lr=0x80100000;c.r[3]=actor+0x4F8;c.r[4]=allow;hle_GameActionOwnerUpdate(c);CHECK(c.r[1]==0x817D0000 && c.r[3]==0x12345678 && c.lr==0x80100000);};
    const auto key_press=[&](uint16_t code) {input.host_button({uc::PhysicalKind::key,code},true);input.host_button({uc::PhysicalKind::key,code},false);uc::game_input_sample(c,raw);};
    key_press(21); weapon_update(); CHECK(reloads==1 && ppc::rd32(c,actor+0x48+0x490)==2);
    key_press(21); weapon_update(); CHECK(reloads==1); // Reload animation in progress.
    ppc::wr32(c,actor+0x48+0x490,1); key_press(21); weapon_update(); CHECK(reloads==1); // Full magazine.
    ppc::wr32(c,actor+0x48+0x48C,10); ppc::wr32(c,actor+0x48+0x484,1); reserve=1;
    key_press(21); weapon_update(); CHECK(reloads==1); // No refill available.
    reserve=30; key_press(21); input.focus(false); input.focus(true); weapon_update(); CHECK(reloads==1);
    key_press(10); owner_update(); CHECK(grenades==1 && grenade_count==2);
    key_press(10); owner_update(); CHECK(grenades==1); // Original grenade cooldown.
    ppc::wrf32(c,actor+0x4F8+0xF0,0); grenade_count=0; key_press(10); owner_update(); CHECK(grenades==1);
    input.host_button({uc::PhysicalKind::key,25},true); uc::game_input_sample(c,raw);
    owner_update(); CHECK(knives==1); owner_update(); CHECK(knives==1);
    ppc::wrf32(c,actor+0x4F8+0xF4,0); owner_update(); CHECK(knives==2); // Held V repeats only at original readiness.
    input.focus(false); input.focus(true); ppc::wrf32(c,actor+0x4F8+0xF4,0); owner_update(); CHECK(knives==2);
    input.host_motion(0,-60); uc::game_input_sample(c,raw); owner_update(); CHECK(knives==2);
    input.host_button({uc::PhysicalKind::mouse,1},true);
    input.host_button({uc::PhysicalKind::key,25},true);
    input.host_motion(0,-60); uc::game_input_sample(c,raw);
    knife_x=0; knife_y=-1; owner_update(); CHECK(knives==3); // Gesture direction wins simultaneous held V.
    input.focus(false); input.focus(true);
    ppc::wrf32(c,actor+0x4F8+0xF4,0);
    input.host_button({uc::PhysicalKind::mouse,1},false); input.host_button({uc::PhysicalKind::key,25},false);
    input.host_button({uc::PhysicalKind::mouse,1},true); input.host_motion(30,40);
    uc::game_input_sample(c,raw); owner_update(0); CHECK(knives==3);
    owner_update(); CHECK(knives==3); // Unavailable one-shot must never fire after the modal gate opens.
    input.focus(false); input.focus(true); grenade_count=3;
    input.host_button({uc::PhysicalKind::mouse,1},false);
    ppc::wrf32(c,actor+0x4F8+0xF0,0);
    input.host_button({uc::PhysicalKind::mouse,1},true);
    input.host_button({uc::PhysicalKind::mouse,3},true);
    input.host_button({uc::PhysicalKind::key,10},true);
    mapped=uc::game_input_sample(c,raw);
    CHECK(!(mapped.held&uc::kButtonB) && !(mapped.pressed&uc::kButtonB));
    owner_update(); CHECK(grenades==2 && grenade_count==2); // G and native chord spend one grenade.
    input.focus(false); input.focus(true);
    auto cleared=uc::default_bindings(); cleared[size_t(uc::Action::grenade)].clear(); cleared[size_t(uc::Action::knife)].clear(); input.bindings(cleared);
    input.host_button({uc::PhysicalKind::mouse,1},false); input.host_button({uc::PhysicalKind::mouse,3},false); input.host_button({uc::PhysicalKind::key,10},false);
    uc::game_input_sample(c,raw);
    ppc::wrf32(c,actor+0x4F8+0xF0,0); ppc::wrf32(c,actor+0x4F8+0xF4,0);
    input.host_button({uc::PhysicalKind::mouse,1},true); input.host_button({uc::PhysicalKind::mouse,3},true);
    input.host_button({uc::PhysicalKind::key,10},true); input.host_button({uc::PhysicalKind::key,25},true);
    input.host_motion(10,0); mapped=uc::game_input_sample(c,raw);
    CHECK(mapped.held==(uc::kButtonA|uc::kButtonB));
    c.r[3]=binding; c.r[4]=7; c.lr=0x801AB590; hle_GameActionMask(c); CHECK(c.r[3]==0);
    owner_update(); CHECK(grenades==2 && knives==3 && grenade_count==2);
    input.focus(false); input.focus(true); input.bindings(uc::default_bindings()); uc::game_input_sample(c,raw);
    input.host_button({uc::PhysicalKind::key,10},false);
    key_press(10); ppc::wr32(c,actor+0x630,1); uc::game_input_sample(c,raw);
    owner_update(); CHECK(grenades==2); // QTE transition cancels published gameplay actions.
    ppc::wr32(c,actor+0x630,0); uc::game_input_sample(c,raw); owner_update(); CHECK(grenades==2);
    input.host_button({uc::PhysicalKind::key,25},false); input.host_button({uc::PhysicalKind::mouse,1},false);
    input.host_button({uc::PhysicalKind::mouse,1},true); input.host_motion(-60,0);
    input.host_button({uc::PhysicalKind::key,25},true); uc::game_input_sample(c,raw);
    knife_x=-1; knife_y=0; owner_update(); CHECK(knives==4); // Gesture wins even when V arrives after motion.
    input.focus(false); input.focus(true);
    auto remapped=uc::default_bindings();
    remapped[size_t(uc::Action::interact)]={{uc::BindingKind::physical,{uc::PhysicalKind::key,9},{}}};
    remapped[size_t(uc::Action::fire)]={{uc::BindingKind::physical,{uc::PhysicalKind::key,11},{}}};
    input.bindings(remapped); uc::game_input_sample(c,raw);
    ppc::wrf32(c,actor+0x4F8+0xF0,0);
    input.host_button({uc::PhysicalKind::key,9},true); input.host_button({uc::PhysicalKind::key,11},true);
    mapped=uc::game_input_sample(c,raw); CHECK(mapped.held==uc::kButtonA && !(mapped.pressed&uc::kButtonB));
    owner_update(); CHECK(grenades==3 && grenade_count==1); // Native chord follows logical rebinding.
    input.focus(false); input.focus(true);
    ppc::wr32(c,c.r[13]-27304,0x80020000);
    ppc::wrf32(c,0x8044A8E8+0x88+8,1); ppc::wr8(c,0x8044A8E8+0x88+0xC,1); ppc::wrf32(c,0x8044A8E8+0x88+0x10,1);
    ppc::wrf32(c,0x8044A8E8+0x468+8,1); ppc::wr8(c,0x8044A8E8+0x468+0xC,1); ppc::wrf32(c,0x8044A8E8+0x468+0x10,1);
    uc::game_input_sample(c,raw); key_press(10);
    ppc::wr32(c,0x80020000+0x6BD0,1); uc::game_input_sample(c,raw);
    ppc::wr32(c,0x80020000+0x6BD0,0); uc::game_input_sample(c,raw);
    ppc::wrf32(c,actor+0x4F8+0xF0,0); owner_update(); CHECK(grenades==3 && grenade_count==1);
    ppc::wr32(c,actor+0x630,1); ppc::wr32(c,actor+0x798,2); uc::game_input_sample(c,raw);
    input.host_motion(32,0,1000000000); input.host_motion(-32,0,1030000000); uc::game_input_sample(c,raw);
    c.r[3]=actor+0x5F8; c.r[4]=0; c.lr=0x800672C0; hle_GameMotionCount(c);
    CHECK(c.r[3]==1 && ppc::rd32(c,actor+0x5F8+0x10)==1 && ppc::rd32(c,actor+0x630)==1);
    c.r[3]=actor+0x5F8; hle_GameMotionCount(c); CHECK(c.r[3]==1); // No gesture means no progress.
    input.host_motion(32,0,1060000000); uc::game_input_sample(c,raw); input.focus(false); input.focus(true);
    c.r[3]=actor+0x5F8; hle_GameMotionCount(c); CHECK(c.r[3]==1);
    ppc::wr32(c,actor+0x798,0); uc::game_input_sample(c,raw);
    input.host_motion(32,0,2000000000); input.host_motion(-32,0,2030000000); uc::game_input_sample(c,raw);
    ppc::wr32(c,actor+0x798,2); uc::game_input_sample(c,raw);
    c.r[3]=actor+0x5F8; hle_GameMotionCount(c); CHECK(c.r[3]==1); // Gesture outside challenge window discarded.
    input.host_motion(32,0,2060000000); uc::game_input_sample(c,raw);
    c.r[3]=actor+0x5F8; hle_GameMotionCount(c); CHECK(c.r[3]==1); // New window cannot inherit half an outside gesture.
    ppc::wr32(c,actor+0x630,2);ppc::wr32(c,actor+0x79C,3);uc::game_input_sample(c,raw);
    input.host_motion(-32,0,2090000000);uc::game_input_sample(c,raw);
    c.r[3]=actor+0x5F8;c.lr=0x800673A0;hle_GameMotionCount(c);CHECK(c.r[3]==1); // A new challenge target resets partial motion.
    input.host_motion(32,0,2120000000);uc::game_input_sample(c,raw);
    ppc::wr32(c,actor+0x79C,6);uc::game_input_sample(c,raw);
    c.r[3]=actor+0x5F8;hle_GameMotionCount(c);CHECK(c.r[3]==1); // Already-published progress belongs to its original target.
    c.r[1]=0x817D0000; c.r[3]=actor+0x5F8; c.lr=0x8006C4B8;
    hle_GameMotionCount(c); CHECK(c.r[3]==0x12345678 && c.r[1]==0x817D0000 && c.lr==0x8006C4B8);
    c.r[3]=0xCC008000; c.lr=0x800672C0; bool bad_motion_object=false;
    try {hle_GameMotionCount(c);}catch(const std::exception&){bad_motion_object=true;}
    CHECK(bad_motion_object && c.r[1]==0x817D0000); // Invalid shared-helper object never reaches a write.
    ppc::wr32(c,actor+0x630,0);ppc::wr32(c,actor+0x48+0x490,1);ppc::wr32(c,actor+0x48+0x48C,10);
    ppc::wr32(c,actor+0x48+0x484,0);ppc::wrf32(c,actor+0x4F8+0xEC,1);uc::game_input_sample(c,raw);
    redirect_input=&input;weapon_update();CHECK(reloads==1); // A yielding original body cannot consume the new actor's request.
    actor_address=0x80040000;c.r[1]=0x817D0000;c.r[3]=actor_address+0x48;c.r[4]=1;c.lr=0x80100000;
    hle_GameWeaponUpdate(c);CHECK(reloads==2 && ppc::rd32(c,actor_address+0x48+0x490)==2);
    input.host_wheel(1);uc::game_input_sample(c,raw);
    c.r[3]=binding;c.r[4]=4;c.lr=0x80068510;c.r[30]=actor;hle_GameActionMask(c);CHECK(c.r[3]==0);
    c.r[3]=binding;c.r[4]=4;c.r[30]=actor_address;hle_GameActionMask(c);CHECK(c.r[3]==uc::kNativeWeaponStep);
    c.r[3]=actor+0x5F8;c.r[4]=0;c.lr=0x800673A0;hle_GameMotionCount(c);CHECK(c.r[3]==1); // Old actor never falls back to sensor-driven QTE progress.
    uc::game_input_shutdown();
}
