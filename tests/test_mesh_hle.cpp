#include "check.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "dispatch.h"
void hle_ResMeshSetup(ppc::Ctx&);
void hle_ResMeshDraw(ppc::Ctx&);
void hle_GXCopyDisp(ppc::Ctx&);
void hle_GXSetArray(ppc::Ctx&);
void hle_ResColorApply(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&);
void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&);
static void flush_recording(ppc::Ctx&) { for(uint32_t i=0;i<8;++i) uc::gx_fifo_write(4,0); }
int main() {
    static const uc::FnEntry functions[]={{0x80284504,flush_recording}};
    uc::dispatch_init(functions,1,0x80284504,0x80284508);
    Fix f; uc::Sched sched(f.c.mem); uc::os_hle_init(sched,f.c.mem,0x817F0000);
    gx::Packets packets; uc::gx_hle_init(sched,f.c.mem,&packets);
    auto* thread=sched.create(0,16,[&](ppc::Ctx& c) {
        c.r[2]=0x803F0000; c.r[13]=0x80400000;
        ppc::wr32(c,c.r[2]-8968,0x80001000);
        ppc::wr32(c,0x80002000,0x80002200);
        ppc::wr32(c,0x8000221C,128); ppc::wr32(c,0x80002220,0x100);
        ppc::wr32(c,0x80002228,32); ppc::wr32(c,0x8000222C,0x300);
        auto cp=[&](uint32_t at,uint8_t tag,uint32_t value) { ppc::wr8(c,0x80002318+at,8); ppc::wr8(c,0x80002319+at,tag); ppc::wr32(c,0x8000231A+at,value); };
        cp(10,0x50,2<<9); cp(16,0x60,0);
        ppc::wr8(c,0x8000232E,0x10); ppc::wr32(c,0x8000232F,0x1008);
        cp(32,0x70,9); cp(38,0x80,0); cp(44,0x90,0);
        cp(50,0xA0,0x3000); cp(56,0xB0,12);
        for(uint32_t i=0;i<3;++i) { ppc::wrf32(c,0x80003000+i*12,float(i+1)); ppc::wrf32(c,0x80003004+i*12,2); ppc::wrf32(c,0x80003008+i*12,3); }
        const float matrix[]={1,0,0,5,0,1,0,6,0,0,1,7};
        for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80003500+i*4,matrix[i]);
        c.r[3]=21; c.r[4]=0x80003500; c.r[5]=48; hle_GXSetArray(c);
        for(uint32_t i=0;i<9;++i) ppc::wrf32(c,0x80003800+i*4,i%4==0 ? 1.f : 0.f);
        c.r[3]=22; c.r[4]=0x80003800; c.r[5]=36; hle_GXSetArray(c);
        ppc::wr8(c,0x80002524,0x20); ppc::wr32(c,0x80002525,0xB000);
        ppc::wr8(c,0x80002529,0x28); ppc::wr32(c,0x8000252A,0x8400);
        ppc::wr8(c,0x8000252E,0x90); ppc::wr16(c,0x8000252F,3);
        ppc::wr8(c,0x80002531,0); ppc::wr8(c,0x80002532,1); ppc::wr8(c,0x80002533,2);
        c.r[3]=0x80002000; c.r[5]=0; hle_ResMeshSetup(c);
        CHECK(ppc::rd32(c,0x80001014)==(2<<9));
        ppc::wr8(c,0x80002534,0xFF); bool rejected=false;
        try { hle_ResMeshDraw(c); } catch(const std::exception&) { rejected=true; }
        CHECK(rejected); ppc::wr8(c,0x80002534,0);
        const float viewport[]={0,0,320,240,0,1};
        for(uint32_t i=0;i<6;++i) ppc::wrf32(c,0x80001544+i*4,viewport[i]);
        ppc::wr32(c,0x80001148,0x20000000u|342u<<12|342);
        ppc::wr32(c,0x8000114C,0x21000000u|501u<<12|581);
        hle_ResMeshDraw(c); ppc::wrf32(c,0x80003000,99);
        ppc::wrf32(c,0x8000154C,640);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        auto packet=packets.try_take(); CHECK(packet && packet->draws.size()==1);
        CHECK(packet->draws[0].vertices.size()==3 && packet->draws[0].vertices[0].position[0]==1 && packet->draws[0].vertices[2].position[0]==3);
        CHECK((packet->draws[0].indices==std::vector<uint32_t>{0,1,2}));
        CHECK(packet->draws[0].transforms.position_rows[0][3]==5);
        CHECK(packet->draws[0].state.viewport && (*packet->draws[0].state.viewport)[2]==320);
        CHECK(packet->draws[0].state.scissor && (*packet->draws[0].state.scissor==std::array<int32_t,4>{0,0,160,240}));
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);

        // Same asset retains fresh pose/viewport while geometry is reused.
        ppc::wrf32(c,0x80003000,1);
        ppc::wrf32(c,0x8000350C,25);
        const auto hits_before=uc::gx_stats().geometry_hits;
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws.size()==1);
        CHECK(uc::gx_stats().geometry_hits==hits_before+1);
        CHECK(packet->draws[0].transforms.position_rows[0][3]==25);
        CHECK((*packet->draws[0].state.viewport)[2]==640);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        ppc::wrf32(c,0x8000350C,5);

        // Mutable array bytes and encoded indices invalidate the whole mesh.
        auto misses_before=uc::gx_stats().geometry_misses;
        ppc::wrf32(c,0x80003000,99);
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws[0].vertices[0].position[0]==99);
        CHECK(uc::gx_stats().geometry_misses==misses_before+1);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        misses_before=uc::gx_stats().geometry_misses;
        ppc::wr8(c,0x80002531,2);
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws[0].vertices[0].position[0]==3);
        CHECK(uc::gx_stats().geometry_misses==misses_before+1);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        ppc::wr8(c,0x80002531,0);
        ppc::wr8(c,0x80002534,0xFF);
        c.r[3]=0x80002000; rejected=false;
        try { hle_ResMeshDraw(c); } catch(const std::exception&) { rejected=true; }
        CHECK(rejected && !packets.try_take());
        ppc::wr8(c,0x80002534,0);

        ppc::wr32(c,0x80006000,0x80005000);
        auto color_field=[&](uint32_t at,uint8_t tag,uint32_t value) { ppc::wr8(c,0x80005000+at,0x61); ppc::wr32(c,0x80005001+at,uint32_t(tag)<<24|value); };
        for(uint32_t i=0;i<3;++i) {
            color_field(i*20,uint8_t(0xE2+i*2),0x7FFu|(255u<<12));
            for(uint32_t repeat=0;repeat<3;++repeat) color_field(i*20+5+repeat*5,uint8_t(0xE3+i*2),7u|(9u<<12));
        }
        for(uint32_t i=0;i<4;++i) { color_field(64+i*10,uint8_t(0xE0+i*2),0x800000u|13u|(255u<<12)); color_field(69+i*10,uint8_t(0xE1+i*2),0x800000u|17u|(19u<<12)); }
        c.r[3]=0x80006000; hle_ResColorApply(c);
        color_field(64,0xE0,0x800000u|99u); ppc::wr8(c,0x80005069,0xFF);
        rejected=false; try { hle_ResColorApply(c); } catch(const std::exception&) { rejected=true; }
        CHECK(rejected);
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet);
        CHECK(packet->draws[0].uniforms.registers[1][0]==-1.f/255 && packet->draws[0].uniforms.konst[0][0]==13.f/255);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);

        // Recorded native colors own values even if the source asset changes later.
        ppc::wr8(c,0x80005069,0);
        ppc::wr32(c,0x80001000,0x00010000); ppc::wr32(c,0x800015FC,0);
        ppc::wr8(c,0x800015F9,1);
        c.r[3]=0x80007000; c.r[4]=128; hle_GXBeginDisplayList(c);
        c.r[3]=0x80006000; hle_ResColorApply(c);
        hle_GXEndDisplayList(c); const auto recorded_size=c.r[3]; CHECK(recorded_size==32);
        color_field(64,0xE0,0x800000u|66u);
        c.r[3]=0x80006000; hle_ResColorApply(c);
        c.r[3]=0x80007000; c.r[4]=recorded_size; hle_GXCallDisplayList(c);
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws[0].uniforms.konst[0][0]==99.f/255);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);

        // Adjacent triangle records share one native draw until a matrix binding changes.
        ppc::wr32(c,0x80002228,64);
        ppc::wr8(c,0x80002534,0x98); ppc::wr16(c,0x80002535,4);
        for(uint32_t i=0;i<4;++i) ppc::wr8(c,0x80002537+i,uint8_t(i%3));
        ppc::wr8(c,0x8000253B,0x20); ppc::wr32(c,0x8000253C,0x1B000);
        ppc::wr8(c,0x80002540,0x90); ppc::wr16(c,0x80002541,3);
        for(uint32_t i=0;i<3;++i) ppc::wr8(c,0x80002543+i,uint8_t(i));
        for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80003530+i*4,matrix[i]);
        ppc::wrf32(c,0x8000353C,15);
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws.size()==2);
        CHECK(packet->draws[0].vertices.size()==7);
        CHECK((packet->draws[0].indices==std::vector<uint32_t>{0,1,2,3,4,5,5,4,6}));
        CHECK(packet->draws[0].transforms.position_rows[0][3]==5);
        CHECK(packet->draws[1].vertices.size()==3 && packet->draws[1].transforms.position_rows[0][3]==15);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);

        // Indexed texture matrix load (GX_LOAD_INDX_C, texmtx 30 at XF 0x78) applies to the draws after it.
        const float texture_matrix[]={1,0,0,0.25f,0,1,0,0,0,0,1,0};
        for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80003900+i*4,texture_matrix[i]);
        c.r[3]=23; c.r[4]=0x80003900; c.r[5]=48; hle_GXSetArray(c);
        ppc::wr32(c,0x80001080,30u<<6);
        ppc::wr8(c,0x80002546,0x30); ppc::wr32(c,0x80002547,0xB078);
        ppc::wr8(c,0x8000254B,0x90); ppc::wr16(c,0x8000254C,3);
        for(uint32_t i=0;i<3;++i) ppc::wr8(c,0x8000254E+i,uint8_t(i));
        c.r[3]=0x80002000; hle_ResMeshDraw(c);
        c.r[3]=0x80004000; c.r[4]=0; hle_GXCopyDisp(c);
        packet=packets.try_take(); CHECK(packet && packet->draws.size()==3);
        CHECK(packet->draws[1].uniforms.tex_rows[0][3]==0 && packet->draws[2].uniforms.tex_rows[0][3]==0.25f);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
    });
    sched.run(thread); CHECK(uc::gx_stats().draws==11);
}
