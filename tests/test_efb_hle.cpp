#include "check.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "dispatch.h"
#include <bit>
void hle_GXSetTexCopySrc(ppc::Ctx&);
void hle_GXSetTexCopyDst(ppc::Ctx&);
void hle_GXCopyTex(ppc::Ctx&);
void hle_GXLoadTexObjPreLoaded(ppc::Ctx&);
void hle_GXBegin(ppc::Ctx&);
void hle_GXCopyDisp(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&);
void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&);
static void flush(ppc::Ctx&) { for(int i=0;i<8;++i) uc::gx_fifo_write(4,0); }
int main() {
    Fix f; uc::Sched sched(f.c.mem); uc::os_hle_init(sched,f.c.mem,0x817F0000);
    gx::Packets packets; uc::gx_hle_init(sched,f.c.mem,&packets);
    auto* thread=sched.create(0,16,[&](ppc::Ctx& c) {
        c.r[2]=0x803F0000; c.r[13]=0x80400000;
        ppc::wr32(c,c.r[2]-8968,0x80001000);
        c.r[3]=0; c.r[4]=0; c.r[5]=640; c.r[6]=360; hle_GXSetTexCopySrc(c);
        CHECK(ppc::rd32(c,0x80001240)==0x49000000);
        CHECK(ppc::rd32(c,0x80001244)==(0x4A000000u|639|(359<<10)));
        c.r[3]=320; c.r[4]=180; c.r[5]=6; c.r[6]=1; hle_GXSetTexCopyDst(c);
        CHECK(ppc::rd32(c,0x80001248)==0x4D0000A0);
        c.r[3]=0x80008001; c.r[4]=0; bool rejected=false;
        try { hle_GXCopyTex(c); } catch(const std::exception&) { rejected=true; }
        CHECK(rejected && !packets.try_take());
        c.r[3]=0x80008000; hle_GXCopyTex(c);
        auto copy=packets.try_take(); CHECK(copy && !copy->present && copy->texture_copy && !copy->clear_after_copy);
        auto old=copy->texture_copy->target; CHECK(old && old->gpu_only && old->levels[0].width==320 && old->levels[0].height==180 && old->levels[0].rgba.empty());
        CHECK(packets.complete(copy->serial));
        ppc::wr32(c,0x80002008,6<<20|179<<10|319); ppc::wr32(c,0x8000200C,0x80008000u>>5);
        ppc::wr32(c,0x80002014,6); ppc::wr8(c,0x8000201F,2);
        c.r[3]=0x80002000; c.r[4]=0x80003000; c.r[5]=0; hle_GXLoadTexObjPreLoaded(c);
        ppc::wr32(c,0x80001014,1<<9); ppc::wr32(c,0x8000101C,9);
        c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c);
        for(int i=0;i<9;++i) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(float(i)));
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        auto draw=packets.try_take(); CHECK(draw && draw->draws.size()==1 && draw->draws[0].texture==old);
        CHECK(packets.complete(draw->serial));
        c.r[3]=0x80008000; c.r[4]=1; hle_GXCopyTex(c);
        copy=packets.try_take(); CHECK(copy && copy->clear_after_copy && copy->texture_copy->target!=old);
        CHECK(draw->draws[0].texture==old && old->levels[0].width==320);
        CHECK(packets.complete(copy->serial));
        // SDK setters emit no commands and can run during resource recording.
        const uc::FnEntry table[]={{0x80284504,flush}};
        uc::dispatch_init(table,1,0x80284504,0x80284508);
        ppc::wr32(c,0x800015FC,0); ppc::wr8(c,0x800015F9,1);
        c.r[3]=0x80005000; c.r[4]=128; hle_GXBeginDisplayList(c);
        c.r[3]=0; c.r[4]=0; c.r[5]=8; c.r[6]=8; hle_GXSetTexCopySrc(c);
        c.r[3]=4; c.r[4]=4; c.r[5]=6; c.r[6]=1; hle_GXSetTexCopyDst(c);
        hle_GXEndDisplayList(c);
        CHECK(ppc::rd32(c,0x80001244)==(0x4A000000u|639|(359<<10)));
        c.r[3]=0x80008000; c.r[4]=0; hle_GXCopyTex(c);
        copy=packets.try_take(); CHECK(copy && copy->texture_copy->target->levels[0].width==320);
        CHECK(copy->texture_copy->source[2]==640 && copy->texture_copy->source[3]==360);
        CHECK(packets.complete(copy->serial));
        ppc::wr32(c,0x80001000,0x10000);
        c.r[3]=0x80005000; c.r[4]=128; hle_GXBeginDisplayList(c);
        c.r[3]=0x80008000; c.r[4]=1; hle_GXCopyTex(c);
        CHECK(!packets.try_take()); // Recording owns an operation, not live GPU work.
        c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c);
        for(int i=0;i<9;++i) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(float(i)));
        hle_GXEndDisplayList(c); const auto length=c.r[3]; CHECK(length>=32);
        ppc::wr32(c,0x80001000,0x10000);
        c.r[3]=0x80005000; c.r[4]=length; hle_GXCallDisplayList(c);
        copy=packets.try_take(); CHECK(copy && !copy->present && copy->texture_copy && copy->clear_after_copy);
        CHECK(copy->texture_copy->target!=old && copy->texture_copy->target->levels[0].width==320);
        CHECK(packets.complete(copy->serial));
        const auto recorded_target=copy->texture_copy->target;
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        draw=packets.try_take(); CHECK(draw && draw->draws.size()==1 && draw->draws[0].texture==recorded_target);
        CHECK(packets.complete(draw->serial));
        const auto shadow=ppc::rd32(c,0x80001240);
        c.r[3]=1; c.r[4]=0; c.r[5]=640; c.r[6]=360; rejected=false;
        try { hle_GXSetTexCopySrc(c); } catch(const std::exception&) { rejected=true; }
        CHECK(rejected && ppc::rd32(c,0x80001240)==shadow && !packets.try_take());
    });
    sched.run(thread);
}
