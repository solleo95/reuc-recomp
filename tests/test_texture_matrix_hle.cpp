#include "check.h"
#include "dispatch.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include <limits>
void hle_GXBegin(ppc::Ctx&); void hle_GXCopyDisp(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&); void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&); void hle_GXLoadTexMtxImm(ppc::Ctx&);
void hle_GXLoadPosMtxImm(ppc::Ctx&); void hle_GXLoadPosMtxIndx(ppc::Ctx&); void hle_GXSetArray(ppc::Ctx&);
void hle_GXSetMatrixIndex(ppc::Ctx&); void hle_ResResetMatrixIndices(ppc::Ctx&); void hle_ResTexMatrixIndices(ppc::Ctx&);
static void flush(ppc::Ctx&) { for(int i=0;i<8;++i) uc::gx_fifo_write(4,0); }
int main() {
    Fix f;
    static const uc::FnEntry functions[]={{0x80284504,flush}};
    uc::dispatch_init(functions,1,0x80284504,0x80284508);
    uc::Sched sched(f.c.mem); uc::os_hle_init(sched,f.c.mem,0x817F0000);
    gx::Packets packets; uc::gx_hle_init(sched,f.c.mem,&packets);
    auto* thread=sched.create(0,16,[&](ppc::Ctx& c) {
        c.r[2]=0x803F0000;
        ppc::wr32(c,c.r[2]-8968,0x80001000); ppc::wr32(c,0x80001000,0x00010000);
        ppc::wr32(c,0x80001014,1<<9); ppc::wr32(c,0x8000101C,9);
        ppc::wr8(c,0x800015F9,1);
        auto load=[&](float offset,uint32_t type=1) {
            for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80002000+i*4,i==0 || i==5 || i==10 ? 1.f : i==3 ? offset : 0.f);
            c.r[3]=0x80002000; c.r[4]=0; c.r[5]=type; hle_GXLoadTexMtxImm(c);
        };
        auto draw=[&] { c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c); for(int i=0;i<9;++i) uc::gx_fifo_write(4,0); };
        auto begin=[&](uint32_t address) { c.r[3]=address; c.r[4]=256; hle_GXBeginDisplayList(c); };
        auto end=[&] { hle_GXEndDisplayList(c); return c.r[3]; };
        auto play=[&](uint32_t address,uint32_t length) { c.r[3]=address; c.r[4]=length; hle_GXCallDisplayList(c); };
        auto take=[&](float wanted,size_t count=1) {
            c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
            auto packet=packets.try_take(); CHECK(packet && packet->draws.size()==count);
            for(const auto& item:packet->draws) CHECK(item.uniforms->tex_rows[0][3]==wanted);
            CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        };
        load(.25f); begin(0x80003000); draw(); const auto inherited=end();
        load(.75f); play(0x80003000,inherited); take(.75f);
        begin(0x80004000); load(.5f); draw(); const auto owned=end();
        ppc::wrf32(c,0x8000200C,99); load(.875f);
        play(0x80004000,owned); draw(); take(.5f,2);
        begin(0x80005000); play(0x80004000,owned); draw(); const auto nested=end();
        load(.125f); play(0x80005000,nested); take(.5f,2);
        begin(0x80006000); load(.25f); play(0x80004000,owned); const auto failed=end();
        load(.875f);
        const auto old_byte=ppc::rd8(c,0x80004000); ppc::wr8(c,0x80004000,old_byte^1);
        bool rejected=false;
        try { play(0x80006000,failed); } catch(const gx::Error&) { rejected=true; }
        CHECK(rejected); ppc::wr8(c,0x80004000,old_byte); draw(); take(.875f);
        ppc::wr32(c,0x80001080,60u<<6);
        begin(0x80007000); draw(); const auto identity=end();
        ppc::wr32(c,0x80001080,0); load(.25f); play(0x80007000,identity); take(0);
        // A non-finite word is guest data (an animation that went wrong): loaded as the hardware does, the coordinates it feeds come out non-finite.
        ppc::wrf32(c,0x80002028,std::numeric_limits<float>::quiet_NaN());   // word 10 = row 2, column 2
        c.r[3]=0x80002000; c.r[4]=0; c.r[5]=0; hle_GXLoadTexMtxImm(c);
        draw(); c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
        auto nan_packet=packets.try_take(); CHECK(nan_packet && nan_packet->draws.size()==1);
        CHECK(std::isnan(nan_packet->draws[0].uniforms->tex_rows[2][2]));
        CHECK(packets.complete(nan_packet->serial)); uc::gx_completed(nan_packet->serial);
        // A 3x4 load keeps its third row; post matrices (ids 64+) and the raw texgen words reach the draw.
        for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80002000+i*4,float(i+1));
        c.r[3]=0x80002000; c.r[4]=30; c.r[5]=0; hle_GXLoadTexMtxImm(c);
        c.r[3]=0x80002000; c.r[4]=67; c.r[5]=0; hle_GXLoadTexMtxImm(c);
        ppc::wr32(c,0x80001080,30u<<6); ppc::wr32(c,0x800010C8,6); ppc::wr32(c,0x800010E8,3|256);
        draw(); c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
        auto packet=packets.try_take(); CHECK(packet && packet->draws.size()==1);
        const auto& u=packet->draws[0].uniforms;
        CHECK((u->tex_rows[2]==std::array<float,4>{9,10,11,12}) && u->tex_gens[0]==6 && u->tex_posts[0]==0x103);
        CHECK((u->post_rows[0]==std::array<float,4>{1,2,3,4}) && (u->post_rows[2]==std::array<float,4>{9,10,11,12}));
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        // A raw call-display-list command (0x40, address, length) written to the FIFO loads texgen 1 and its post word from the list's XF loads.
        const uint8_t texgen_list[]={0x10,0,0,0x10,0x41,0,0,0x50,0x86, 0x10,0,0,0x10,0x51,0,0,0x01,0x03};   // XF 0x1041 = 0x5086, XF 0x1051 = 0x103
        for(size_t i=0;i<sizeof texgen_list;++i) ppc::wr8(c,0x80009000+uint32_t(i),texgen_list[i]);
        uc::gx_fifo_write(1,0x40); uc::gx_fifo_write(4,0x80009000); uc::gx_fifo_write(4,sizeof texgen_list);
        draw(); c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
        auto raw_packet=packets.try_take(); CHECK(raw_packet && raw_packet->draws.size()==1);
        CHECK(raw_packet->draws[0].uniforms->tex_gens[1]==0x5086 && raw_packet->draws[0].uniforms->tex_posts[1]==0x103 && raw_packet->draws[0].uniforms->tex_gens[0]==6);
        CHECK(packets.complete(raw_packet->serial)); uc::gx_completed(raw_packet->serial);
        // Reflection texgen and per-vertex selectors read the same XF rows as position matrices.
        auto position=[&](float offset) {
            for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80002000+i*4,float(i)+offset);
            c.r[3]=0x80002000; c.r[4]=9; hle_GXLoadPosMtxImm(c);
        };
        auto check_alias=[&](float offset) {
            ppc::wr32(c,0x80001080,9u<<6); ppc::wr32(c,0x80001014,(1<<9)|2);
            c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c);
            for(int i=0;i<3;++i) { uc::gx_fifo_write(1,10); for(int j=0;j<3;++j) uc::gx_fifo_write(4,0); }
            c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
            auto p=packets.try_take(); CHECK(p && p->draws.size()==1);
            const auto& d=p->draws[0];
            CHECK(d.uniforms->tex_rows[0][3]==offset+3);
            CHECK(d.texture_bank && d.texture_bank->rows[10*3][3]==offset+7);
            CHECK(d.transforms->position_rows[9][3]==offset+3);
            CHECK(packets.complete(p->serial)); uc::gx_completed(p->serial);
            ppc::wr32(c,0x80001014,1<<9);
        };
        position(10); check_alias(10);
        begin(0x8000A000); position(20); const auto position_list=end();
        check_alias(10); play(0x8000A000,position_list); check_alias(20);
        // Indexed loads, including recorded loads, read their source at execution time.
        c.r[3]=21; c.r[4]=0x80002000; c.r[5]=48; hle_GXSetArray(c);
        begin(0x8000B000); c.r[3]=0; c.r[4]=9; hle_GXLoadPosMtxIndx(c); const auto indexed_list=end();
        for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80002000+i*4,float(i)+30);
        play(0x8000B000,indexed_list); check_alias(30);
        position(10); for(uint32_t i=0;i<12;++i) ppc::wrf32(c,0x80002000+i*4,float(i)+30);
        c.r[3]=0; c.r[4]=9; hle_GXLoadPosMtxIndx(c); check_alias(30);
        // A 2x4 texture load overwrites two shared rows, preserving the third.
        for(uint32_t i=0;i<8;++i) ppc::wrf32(c,0x80002000+i*4,float(i)+40);
        c.r[3]=0x80002000; c.r[4]=9; c.r[5]=1; hle_GXLoadTexMtxImm(c); check_alias(40);
        draw(); c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyDisp(c);
        auto partial=packets.try_take(); CHECK(partial && partial->draws[0].uniforms->tex_rows[2][3]==41);
        CHECK(partial->draws[0].transforms->position_rows[11][3]==41);
        CHECK(packets.complete(partial->serial)); uc::gx_completed(partial->serial);
        // NW4R bypasses the SDK cache; SDK flushes and reset still replace the actual XF selectors.
        ppc::wr32(c,0x80001080,60u<<6);
        for(uint32_t i=0;i<8;++i) ppc::wr32(c,0x80002100+i*4,i==0 ? 9 : 60);
        c.r[3]=0x80002100; hle_ResTexMatrixIndices(c); draw(); take(43);
        CHECK(ppc::rd32(c,0x80001080)==60u<<6);
        c.r[3]=0; hle_GXSetMatrixIndex(c); draw(); take(0);
        begin(0x8000C000); c.r[3]=0x80002100; hle_ResTexMatrixIndices(c); draw(); const auto selectors_list=end();
        draw(); take(0); play(0x8000C000,selectors_list); draw(); take(43,2);
        hle_ResResetMatrixIndices(c); draw(); take(0);
        // GXSetTexCoordGen2 defers its SDK matrix-index write until the next GXBegin.
        ppc::wr32(c,0x80001080,9u<<6); ppc::wr32(c,0x800015FC,0x04000002);
        draw(); take(43);
        CHECK(ppc::rd32(c,0x800015FC)==2);
        hle_ResResetMatrixIndices(c); draw(); take(0); // The old pending SDK write must not override a newer NW4R command.
        ppc::wr32(c,0x800015FC,0);
        begin(0x8000D000); c.r[3]=0x80002100; hle_ResTexMatrixIndices(c); const auto child_selectors=end();
        begin(0x8000E000); play(0x8000D000,child_selectors); draw(); const auto parent_selectors=end();
        play(0x8000E000,parent_selectors); draw(); take(43,2);
        hle_ResResetMatrixIndices(c);
        const auto child_byte=ppc::rd8(c,0x8000D000); ppc::wr8(c,0x8000D000,child_byte^1);
        rejected=false; try { play(0x8000E000,parent_selectors); } catch(const gx::Error&) { rejected=true; }
        CHECK(rejected); draw(); take(0);
        ppc::wr8(c,0x8000D000,child_byte);
    });
    sched.run(thread);
}
