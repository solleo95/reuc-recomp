#include "check.h"
#include "dispatch.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include <limits>
void hle_GXBegin(ppc::Ctx&); void hle_GXCopyDisp(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&); void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&); void hle_GXLoadTexMtxImm(ppc::Ctx&);
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
            for(const auto& item:packet->draws) CHECK(item.uniforms.tex_rows[0][3]==wanted);
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
        ppc::wrf32(c,0x80002028,std::numeric_limits<float>::quiet_NaN());
        c.r[3]=0x80002000; c.r[4]=0; c.r[5]=0; rejected=false;
        try { hle_GXLoadTexMtxImm(c); } catch(const gx::Error&) { rejected=true; }
        CHECK(rejected); draw(); take(.25f);
    });
    sched.run(thread);
}
