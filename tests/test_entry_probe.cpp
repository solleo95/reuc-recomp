#include "check.h"
#include "ppc_fixture.h"
#include "entry_probe.h"
#include <cstring>
int main() {
    Fix fixture;auto& c=fixture.c;c.r[3]=0x80002000;c.r[4]=0x80003000;
    ppc::wr32(c,c.r[4],0x80004000);ppc::wr32(c,0x8000400C,0x800FA84C);
    ppc::wr32(c,c.r[3]+0x630,1);
    const auto before=c;
    uc::entry_probe_init(false);uc::game_event_probe(c);CHECK(uc::entry_probe_stats().events==0);
    uc::entry_probe_init(true);uc::game_event_probe(c);
    CHECK(uc::entry_probe_stats().events==1 && uc::entry_probe_stats().records==1);
    uc::game_event_probe(c);CHECK(uc::entry_probe_stats().events==2 && uc::entry_probe_stats().records==1);
    CHECK(std::memcmp(&before,&c,sizeof(c))==0 && ppc::rd32(c,c.r[3]+0x630)==1);
    c.r[4]=0xCC008000;uc::game_event_probe(c);CHECK(uc::entry_probe_stats().invalid==1);
    c.r[4]=0x80003000;
    for(uint32_t i=0;i<80;++i) {ppc::wr32(c,c.r[3]+0x630,i);uc::game_event_probe(c);CHECK(ppc::rd32(c,c.r[3]+0x630)==i);}
    CHECK(uc::entry_probe_stats().records==64);
    std::memcpy(c.mem+0x2000+0xB8,"tex1",5);ppc::wr8(c,0x800020B5,64);
    uc::picture_entry_probe(c,0x8038BC60);CHECK(uc::picture_probe_stats().records==1);
    CHECK(ppc::rd8(c,0x800020B5)==64 && std::memcmp(c.mem+0x20B8,"tex1",5)==0);
    uc::picture_entry_probe(c,0x8038BC60);CHECK(uc::picture_probe_stats().records==1);
    uc::picture_entry_probe(c,0x1234);CHECK(uc::picture_probe_stats().calls==2);
    c.r[4]=0x80006000;c.r[14]=0x80005000;
    ppc::wr32(c,c.r[4],0x524C5954);ppc::wr32(c,c.r[4]+8,128);
    std::memcpy(c.mem+0x5000,"ACT_R_Hshake.brlyt",18);
    const auto layout_before=c;uc::layout_entry_probe(c);
    CHECK(std::memcmp(&layout_before,&c,sizeof(c))==0 && ppc::rd32(c,c.r[4]+8)==128);
    c.r[4]=0xCC008000;uc::layout_entry_probe(c); // Probe cannot turn a bad resource into a runtime trap.
    uc::entry_probe_init(false);return 0;
}
