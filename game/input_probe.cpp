#include "input_probe.h"
#include "gx_vertices.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>
namespace {
bool enabled;
uc::InputProbeStats stats;
std::optional<uc::InputProbeSnapshot> previous;
uint32_t word(std::span<const std::byte> bytes,size_t offset) {
    uint32_t result=0;
    for(size_t i=0;i<4;++i) result=(result<<8)|std::to_integer<uint8_t>(bytes[offset+i]);
    return result;
}
float real(std::span<const std::byte> bytes,size_t offset) { return std::bit_cast<float>(word(bytes,offset)); }
bool orthonormal(const std::array<float,12>& m) {
    for(auto v:m)if(!std::isfinite(v)||std::abs(v)>1e7f)return false;
    const auto dot=[&](int a,int b){ return m[a*4]*m[b*4]+m[a*4+1]*m[b*4+1]+m[a*4+2]*m[b*4+2]; };
    return std::abs(dot(0,0)-1)<1e-2f && std::abs(dot(1,1)-1)<1e-2f && std::abs(dot(2,2)-1)<1e-2f && std::abs(dot(0,1))<1e-2f && std::abs(dot(0,2))<1e-2f && std::abs(dot(1,2))<1e-2f;
}
std::span<const std::byte> read(const gx::Memory& memory,uint32_t address,size_t size) {
    if(!address)return {};
    try { return memory.read(address,size); }
    catch(const gx::Error&) { return {}; } // Not-yet-created and unmapped objects remain unavailable in the probe.
}
}
namespace uc {
InputProbeSnapshot input_probe_snapshot(const ppc::Ctx& c) {
    InputProbeSnapshot result;
    if(!c.mem)return result;
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    if(auto bytes=read(memory,0x80596460+0x148,8); !bytes.empty()) {
        result.game_mode_valid=true; result.pause_state=word(bytes,0); result.game_mode=word(bytes,4);
    }
    if(auto bytes=read(memory,0x804B5CC8,0x1D4E0); !bytes.empty()) {
        result.ui_valid=true; result.ui_enabled=std::to_integer<uint8_t>(bytes[0x1D4A0])!=0;
        for(size_t i=0;i<12;++i)result.ui_states[i]=word(bytes,0x1D4B0+i*4);
        for(size_t i=0;i<4;++i) { static const size_t offsets[4]={0x804BB408-0x804B5CC8,0x804BF4C0-0x804B5CC8,0x804C0574-0x804B5CC8,0x804D0994-0x804B5CC8}; result.menu_pages[i]=word(bytes,offsets[i]); }
        result.hud_life=real(bytes,0x804B7960-0x804B5CC8);
        result.tutorial_loaded=std::to_integer<uint8_t>(bytes[0x234C+5])!=0;
        result.tutorial_state=word(bytes,0x234C+0x48); // HUD+22B8, original TutorialBusy8024F558.
    }
    if(auto bytes=read(memory,0x8044A808,0x68); !bytes.empty()) {
        result.bindings_valid=true;
        for(size_t i=0;i<20;++i) { result.masks[i]=word(bytes,4+i*4); result.disabled[i]=std::to_integer<uint8_t>(bytes[0x54+i]); }
    }
    if(c.r[13]<27464)return result;
    if(auto pointer=read(memory,c.r[13]-27304,4); !pointer.empty()) {
        result.scene_address=word(pointer,0);
        if(auto bytes=read(memory,result.scene_address,0x6C18); !bytes.empty()) {
            result.scene_valid=true;
            const size_t offsets[]={0x6BCC,0x6BD0,0x6BEC,0x6C00,0x6C14};
            for(size_t i=0;i<5;++i)result.scene_words[i]=word(bytes,offsets[i]);
        }
    }
    if(auto bytes=read(memory,0x8044A8E8,0x47C); !bytes.empty())
        for(size_t i=0;i<2;++i) for(size_t j=0;j<3;++j)result.fade_words[i*3+j]=word(bytes,(i ? 0x468 : 0x88)+8+j*4);
    if(auto bytes=read(memory,0x8044B4E0,48); !bytes.empty()) {
        for(size_t i=0;i<12;++i)result.view[i]=real(bytes,i*4);
        result.view_valid=orthonormal(result.view);
    }
    // Actor table: (id,pointer) pairs; class pointer at +10. Classes 803F2388 (players) and 803F1A40/803F7EA0 (pools) are not enemies.
    const auto add_enemy=[&](uint32_t id,uint32_t address) {
        if(!address || result.enemy_count>=result.enemies.size())return;
        for(uint32_t i=0;i<result.enemy_count;++i)if(result.enemies[i].address==address)return;
        const auto kind=read(memory,address+0x10,4);
        if(kind.empty())return;
        const uint32_t actor_class=word(kind,0);
        if(!actor_class || actor_class==0x803F2388 || actor_class==0x803F1A40 || actor_class==0x803F7EA0)return;
        const auto position=read(memory,address+0xF48,12),head=read(memory,address+0x1058,4),health=read(memory,address+0x3A4,4);
        if(position.empty() || head.empty() || health.empty())return;
        InputProbeEnemy enemy{id,address,{real(position,0),real(position,4),real(position,8)},real(head,0)>0.5f,actor_class,real(health,0)};
        bool sane=true; for(auto v:enemy.root)sane&=std::isfinite(v) && std::abs(v)<1e6f;
        if(sane)result.enemies[result.enemy_count++]=enemy;
    };
    if(auto table=read(memory,0x804421C4,8*200); !table.empty())
        for(size_t i=0;i<200;++i)add_enemy(word(table,i*8),word(table,i*8+4));
    const auto pointers=read(memory,c.r[13]-27464,8);
    if(pointers.empty())return result;
    for(size_t i=0;i<2;++i) {
        auto& player=result.players[i]; player.address=word(pointers,i*4);
        if(auto bytes=read(memory,player.address,0x7A0); !bytes.empty()) {
            player.valid=true;
            player.mode_630=word(bytes,0x630); player.player_790=word(bytes,0x790); player.state_794=word(bytes,0x794); player.flags_798=word(bytes,0x798);
            player.qte_target_79C=word(bytes,0x79C);
            for(size_t w=0;w<6;++w)player.weapon_words[w]=word(bytes,0x48+0x480+w*4);
        }
    }
    // Actors created after the table filled (ids 200+, e.g. the roof zombies) are only in the actor list: object+8 holds the next object + 8.
    std::vector<uint32_t> visited{result.players[0].address};
    for(uint32_t object=result.players[0].address;object && visited.size()<512;) {
        const auto link=read(memory,object+8,4);
        if(link.empty() || word(link,0)<8)break;
        object=word(link,0)-8;
        if(std::find(visited.begin(),visited.end(),object)!=visited.end())break;
        visited.push_back(object);
        if(const auto id=read(memory,object,4); !id.empty())add_enemy(word(id,0),object);
    }
    return result;
}
void input_probe_init(bool active) { enabled=active; stats={}; previous.reset(); }
InputProbeStats input_probe_stats() { return stats; }
void input_probe_sample(const ppc::Ctx& c,const InputSample& sample) {
    if(!enabled)return;
    ++stats.samples;
    const auto snapshot=input_probe_snapshot(c);
    if(previous && *previous==snapshot && !sample.pressed && !sample.released)return;
    bool important=!previous || snapshot.masks!=previous->masks || snapshot.disabled!=previous->disabled ||
        snapshot.game_mode!=previous->game_mode || snapshot.game_mode_valid!=previous->game_mode_valid ||
        snapshot.pause_state!=previous->pause_state ||
        snapshot.tutorial_loaded!=previous->tutorial_loaded || snapshot.tutorial_state!=previous->tutorial_state ||
        snapshot.ui_states!=previous->ui_states || snapshot.ui_enabled!=previous->ui_enabled ||
        snapshot.scene_address!=previous->scene_address || snapshot.scene_valid!=previous->scene_valid ||
        snapshot.scene_words[1]!=previous->scene_words[1] || snapshot.scene_words[2]!=previous->scene_words[2];
    if(previous) for(size_t i=0;i<2;++i) {
        const auto& a=snapshot.players[i]; const auto& b=previous->players[i];
        important|=a.address!=b.address || a.valid!=b.valid || a.mode_630!=b.mode_630 || a.state_794!=b.state_794 ||
            a.weapon_words[0]!=b.weapon_words[0] || a.weapon_words[1]!=b.weapon_words[1] || a.weapon_words[3]!=b.weapon_words[3] ||
            (a.weapon_words[4]==2)!=(b.weapon_words[4]==2);
    }
    previous=snapshot; ++stats.changes;
    if(!important && !sample.pressed && !sample.released)return;
    if(stats.records>=128)return;
    ++stats.records;
    std::fprintf(stderr,"[input-probe-context] scene %08X valid %u words",snapshot.scene_address,snapshot.scene_valid);
    for(auto value:snapshot.scene_words)std::fprintf(stderr," %08X",value);
    std::fprintf(stderr," fades"); for(auto value:snapshot.fade_words)std::fprintf(stderr," %08X",value);
    std::fprintf(stderr," ui %u:%u",snapshot.ui_valid,snapshot.ui_enabled);
    for(auto value:snapshot.ui_states)std::fprintf(stderr," %08X",value);
    std::fprintf(stderr," game-mode %u:%08X pause-state %08X",snapshot.game_mode_valid,snapshot.game_mode,snapshot.pause_state);
    std::fprintf(stderr," tutorial %u:%08X",snapshot.tutorial_loaded,snapshot.tutorial_state);
    std::fprintf(stderr,"\n");
    std::fprintf(stderr,"[input-probe] sample %llu r13 %08X held %08X pressed %08X released %08X masks",
        (unsigned long long)stats.samples,c.r[13],sample.held,sample.pressed,sample.released);
    for(auto mask:snapshot.masks)std::fprintf(stderr," %08X",mask);
    std::fprintf(stderr," disabled");
    for(auto flag:snapshot.disabled)std::fprintf(stderr," %u",flag);
    for(size_t i=0;i<2;++i) {
        const auto& p=snapshot.players[i];
        std::fprintf(stderr," p%zu %08X valid %u mode630 %08X player790 %08X state794 %08X flags798 %08X weapon480",i,p.address,p.valid,p.mode_630,p.player_790,p.state_794,p.flags_798);
        for(auto value:p.weapon_words)std::fprintf(stderr," %08X",value);
    }
    std::fprintf(stderr,"\n");
}
}
