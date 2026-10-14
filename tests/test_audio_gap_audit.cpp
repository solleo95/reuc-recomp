#include "check.h"
#include "audio_gap_audit.h"
#include "ppc_fixture.h"
#include <array>
int main() {
    Fix f; auto& c=f.c; constexpr uint32_t pb=0x80001000;
    ppc::wr16(c,pb+0x10,1); ppc::wr16(c,pb+0x70,10);
    ppc::wr16(c,pb+0x6E,1); ppc::wr16(c,pb+8,2);
    ppc::wr32(c,pb+0x0C,3); ppc::wr32(c,pb+0xA6,65536);
    uc::audio_gap_audit_enable(true);
    auto key=uc::audio_gap_voice_key(c,pb);
    CHECK(key.format==10 && key.loop==1 && key.src==2 && key.control==3);
    std::array<int32_t,192> before{},after{}; after[0]=-200; after[1]=300;
    uc::audio_gap_audit_voice(key,before,after);
    uc::audio_gap_audit_voice(key,before,before);
    auto rows=uc::audio_gap_audit_rows();
    CHECK(rows.size()==1 && rows[0].blocks==2 && rows[0].audible_blocks==1 && rows[0].peak==300);
    ppc::wr16(c,pb+0x70,123);
    CHECK(uc::audio_gap_voice_key(c,pb).handling==uc::AudioHandling::UnsupportedFormat);
    ppc::wr16(c,pb+0x70,10); ppc::wr32(c,pb+0x0C,0x100);
    CHECK(uc::audio_gap_voice_key(c,pb).handling==uc::AudioHandling::NoMainRoute);
    for(uint32_t i=0;i<1000;++i) { key.control=i; uc::audio_gap_audit_voice(key,before,after); }
    CHECK(uc::audio_gap_audit_rows().size()<=uc::kAudioAuditMaxRows);
    c.r[3]=0; c.r[5]=20;
    ppc::wr32(c,0x8065C490,0x80004000);
    ppc::wr32(c,0x800048BC,uint32_t(-1));
    uc::audio_gap_audit_wpad(c,0x802E3C10);
    ppc::wr32(c,0x800048BC,0); ppc::wr32(c,0x800048DC,1);
    uc::audio_gap_audit_wpad(c,0x802E3C94);
    auto speaker=uc::audio_gap_speaker_stats();
    CHECK(speaker.queries==1 && speaker.sends==1 && speaker.bytes==20 && speaker.disconnected==1);
    c.r[3]=4;uc::audio_gap_audit_wpad(c,0x802E3C94);
    CHECK(uc::audio_gap_speaker_stats().invalid==1);
    c.r[3]=0x80005000;c.r[4]=0;c.r[5]=uint32_t(-1);c.lr=0x80201234;
    for(size_t i=0;i<9;++i)ppc::wr8(c,c.r[3]+uint32_t(i),"menu_test"[i]);
    ppc::wr8(c,c.r[3]+9,0);
    uc::audio_gap_audit_request(c);
    auto requests=uc::audio_gap_requests();
    CHECK(requests.size()==1 && requests[0].name=="menu_test" && requests[0].caller==0x80201234);
    CHECK(requests[0].valid && requests[0].channel==uint32_t(-1));
    ppc::wr8(c,c.r[3],'X');
    CHECK(uc::audio_gap_requests()[0].name=="menu_test");
    c.r[3]=0xFFFFFFF0;uc::audio_gap_audit_request(c);
    CHECK(!uc::audio_gap_requests()[1].valid);
    for(size_t i=0;i<200;++i)uc::audio_gap_audit_request(c);
    CHECK(uc::audio_gap_requests().size()==128);
    c.lr=0x8004C394;c.r[3]=0x8045EA64;c.r[4]=0x80005000;
    { auto scope=uc::audio_start_scope<0x803A6794>(c);c.r[3]=42; }
    auto starts=uc::audio_gap_starts();
    CHECK(starts.size()==1 && starts[0].result==42 && starts[0].object==0x8045EA64);
    CHECK(starts[0].argument==0x80005000 && starts[0].caller==0x8004C394);
    { auto scope=uc::audio_start_scope<0x80000000>(c); }
    CHECK(uc::audio_gap_starts().size()==1);
    for(size_t i=0;i<600;++i)uc::audio_gap_audit_start(starts[0]);
    CHECK(uc::audio_gap_starts().size()==512);
    uc::audio_gap_audit_enable(false);
    uc::audio_gap_audit_voice(key,before,after);
    CHECK(uc::audio_gap_audit_rows().empty());
    uc::audio_gap_audit_request(c);CHECK(uc::audio_gap_requests().empty());
    { auto scope=uc::audio_start_scope<0x803A6794>(c); }
    CHECK(uc::audio_gap_starts().empty());
}
