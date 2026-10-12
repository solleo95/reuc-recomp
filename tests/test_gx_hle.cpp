#include "check.h"
#include "dispatch.h"
#include "gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include <chrono>
#include <bit>
#include <thread>
using namespace std::chrono_literals;
void hle_GXBegin(ppc::Ctx&); void hle_GXSetArray(ppc::Ctx&);
void hle_GXSetDrawDone(ppc::Ctx&); void hle_GXWaitDrawDone(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&); void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&);
void hle_GXSetProjection(ppc::Ctx&); void hle_GXSetProjectionv(ppc::Ctx&);
void hle_GXLoadPosMtxImm(ppc::Ctx&); void hle_GXLoadPosMtxIndx(ppc::Ctx&); void hle_GXSetCurrentMtx(ppc::Ctx&);
void hle_GXCopyDisp(ppc::Ctx&); void hle_GXSetCopyClear(ppc::Ctx&);
void hle_GXLoadTexObjPreLoaded(ppc::Ctx&);

static int calls = 0;
static void callback(ppc::Ctx& ctx) { CHECK(!uc::os_sched().interrupts_enabled()); CHECK(ppc::rd8(ctx, ctx.r[13] - 25328) == 1); ++calls; }
static void flush(ppc::Ctx&) { for (int i=0;i<8;++i) uc::gx_fifo_write(4,0); }
static void flush_primitive(ppc::Ctx& ctx) { ppc::wr16(ctx,0x80001002,1); } // fixture omits the SDK's dummy geometry
static void dirty_flush(ppc::Ctx& ctx) {
    ppc::wr32(ctx,0x800015FC,0);
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,0x12345678);
    ctx.r[3]=0xDEADBEEF; ctx.r[4]=0; ctx.r[5]=0;
}
template<class Fn> static void rejects(gx::ErrorCode code, Fn fn) {
    bool caught = false;
    try { fn(); } catch (const gx::Error& error) { caught = true; CHECK(error.code==code); }
    CHECK(caught);
}
int main() {
    Fix memory;
    auto& c = memory.c;
    c.r[2] = 0x803F0000; c.r[13] = 0x80400000;
    ppc::wr32(c, c.r[2] - 8968, 0x80001000);
    ppc::wr32(c,0x80001000,1); // no dummy SDK flush primitive
    ppc::wr32(c, 0x80001014, (1 << 9) | (1 << 13)); // direct position + color0
    ppc::wr32(c, 0x8000101C, 1 | (4 << 1) | (5 << 14)); // XYZ/F32, RGBA8
    ppc::wr32(c, c.r[13] - 25324, 0x80100000);
    static const uc::FnEntry table[] = {{0x80100000, callback},{0x80284504,flush},{0x80284D58,dirty_flush},{0x8028511C,flush_primitive}};
    uc::dispatch_init(table, 4, 0x80100000, 0x80285120);
    uc::Sched sched(c.mem); uc::os_hle_init(sched, c.mem, 0x817F0000);
    gx::Packets packets;
    sched.set_shutdown_hook([&] { packets.cancel(); });
    uc::gx_hle_init(sched, c.mem, &packets);
    auto* main = sched.create(0, 16, [&](ppc::Ctx& ctx) {
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF20);
        ppc::wr32(ctx,0x80006300,0x90); ppc::wr32(ctx,0x80006304,0);
        ppc::wr32(ctx,0x80006308,3|(3<<10)|(3<<20));
        ppc::wr32(ctx,0x8000630C,0x6200>>5); ppc::wr32(ctx,0x80006314,3); ppc::wr8(ctx,0x8000631F,2);
        const uint32_t tables[]={28936,28928,28920,28912,28904,28896};
        for (uint32_t i=0;i<6;++i) ppc::wr8(ctx,ctx.r[13]-tables[i],uint8_t(0x80+i*4));
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=8;
        rejects(gx::ErrorCode::layout,[&]{hle_GXLoadTexObjPreLoaded(ctx);});
        ctx.r[5]=0; hle_GXLoadTexObjPreLoaded(ctx);
        CHECK(ppc::rd32(ctx,0x80001564)==ppc::rd32(ctx,0x80006308));
        CHECK(ppc::rd32(ctx,0x80001584)==ppc::rd32(ctx,0x80006300));
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=1; hle_GXLoadTexObjPreLoaded(ctx);
        CHECK(ppc::rd32(ctx,0x80001568)==ppc::rd32(ctx,0x80006308));
        ppc::wr32(ctx,0x8000630C,0x017FFFFF); ctx.r[5]=0;
        rejects(gx::ErrorCode::array_bounds,[&]{hle_GXLoadTexObjPreLoaded(ctx);});
        ppc::wr32(ctx,0x8000630C,0x6200>>5);
        ppc::wr32(ctx,0x800015FC,0);
        const float matrix[]={1,0,0,4,0,1,0,5,0,0,1,6};
        for (size_t i=0;i<12;++i) ppc::wr32(ctx,0x80005000+uint32_t(i*4),std::bit_cast<uint32_t>(matrix[i]));
        ctx.r[3]=0x80005000; ctx.r[4]=3; hle_GXLoadPosMtxImm(ctx);
        ctx.r[3]=3; hle_GXSetCurrentMtx(ctx); CHECK((ppc::rd32(ctx,0x80001080)&63)==3);
        const float full_projection[]={2,0,8,6,0,3,9,7,0,0,4,5,0,0,-1,0};
        for (size_t i=0;i<16;++i) ppc::wr32(ctx,0x80005140+uint32_t(i*4),std::bit_cast<uint32_t>(full_projection[i]));
        ctx.r[3]=0x80005140; ctx.r[4]=1; hle_GXSetProjection(ctx);
        CHECK(ppc::rd32(ctx,0x80001530)==std::bit_cast<uint32_t>(6.f) && ppc::rd32(ctx,0x80001538)==std::bit_cast<uint32_t>(7.f));
        ctx.r[3]=0x80005140; ctx.r[4]=0; hle_GXSetProjection(ctx);
        CHECK(ppc::rd32(ctx,0x80001530)==std::bit_cast<uint32_t>(8.f) && ppc::rd32(ctx,0x80001538)==std::bit_cast<uint32_t>(9.f));
        ctx.r[3]=0x80005140; ctx.r[4]=2;
        rejects(gx::ErrorCode::layout,[&]{hle_GXSetProjection(ctx);});
        const float projection[]={1,0,1,0,1,0};
        ppc::wr32(ctx,0x80005100,std::bit_cast<uint32_t>(1.f));
        for (size_t i=0;i<6;++i) ppc::wr32(ctx,0x80005104+uint32_t(i*4),std::bit_cast<uint32_t>(projection[i]));
        ctx.r[3]=0x80005100; hle_GXSetProjectionv(ctx);
        CHECK(ppc::rd32(ctx,0x80001528)==1 && ppc::rd32(ctx,0x8000152C)==std::bit_cast<uint32_t>(1.f));
        ppc::wr32(ctx,0x800015FC,0); // fixture flush does not emulate translated SDK projection dirty writes
        ctx.r[3]=0x80002000; ctx.r[4]=128;
        ppc::wr8(ctx,0x800015F9,1); // preserve SDK context during recording
        ppc::wr32(ctx,0x80001008,17);
        hle_GXBeginDisplayList(ctx);
        CHECK(ppc::rd8(ctx,0x800015F8)==1);
        rejects(gx::ErrorCode::sequence,[&]{hle_GXBeginDisplayList(ctx);});
        rejects(gx::ErrorCode::sequence,[&]{hle_GXSetDrawDone(ctx);});
        uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,0x410004A0);
        ctx.r[3]=9; ctx.r[4]=0x80003000; ctx.r[5]=0x0102030C; hle_GXSetArray(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for (int i=0;i<12;++i) uc::gx_fifo_write(4,uint32_t(i));
        ppc::wr32(ctx,0x80001014,0x1234); ppc::wr32(ctx,0x80001008,23);
        hle_GXEndDisplayList(ctx);
        CHECK(ctx.r[3]==96 && ppc::rd8(ctx,0x800015F8)==0);
        CHECK(ppc::rd32(ctx,0x80001014)==((1<<9)|(1<<13)) && ppc::rd32(ctx,0x80001008)==23);
        CHECK(ppc::rd8(ctx,0x80002000)==0x61 && ppc::rd32(ctx,0x80002001)==0x410004A0);
        CHECK(ppc::rd32(ctx,0x8000200D)==0x0102030C);
        CHECK(ppc::rd8(ctx,0x80002011)==0x90 && ppc::rd16(ctx,0x80002012)==3);
        CHECK(ppc::rd32(ctx,0x80002014)==0 && ppc::rd32(ctx,0x80002040)==11 && ppc::rd32(ctx,0x8000205C)==0);
        CHECK(uc::gx_stats().draws==0); // recording resource data submits no geometry
        rejects(gx::ErrorCode::sequence,[&]{hle_GXEndDisplayList(ctx);});
        ctx.r[3]=0x80002001; ctx.r[4]=128;
        rejects(gx::ErrorCode::layout,[&]{hle_GXBeginDisplayList(ctx);});
        ctx.r[3]=0xCC008000; ctx.r[4]=128;
        rejects(gx::ErrorCode::array_bounds,[&]{hle_GXBeginDisplayList(ctx);});
        ctx.r[3]=0x80002000; ctx.r[4]=128;
        ppc::wr32(ctx,0x800015FC,1);
        hle_GXBeginDisplayList(ctx); // dirty flush precedes the destination recording
        CHECK(ppc::rd32(ctx,0x800015FC)==0);
        ppc::wr32(ctx,0x800015FC,1);
        ctx.r[3]=0xB8; ctx.r[4]=0; ctx.r[5]=1;
        hle_GXBegin(ctx); // save vertex header arguments across the dirty flush
        for (int i=0;i<4;++i) uc::gx_fifo_write(4,0);
        hle_GXEndDisplayList(ctx);
        CHECK(ctx.r[3]==32 && ppc::rd32(ctx,0x80002001)==0x12345678);
        CHECK(ppc::rd8(ctx,0x80002005)==0xB8 && ppc::rd16(ctx,0x80002006)==1);
        ppc::wr8(ctx,0x800015F9,0);
        ctx.r[3]=0x80002000; ctx.r[4]=32; hle_GXBeginDisplayList(ctx);
        ppc::wr32(ctx,0x80001014,0x777);
        hle_GXEndDisplayList(ctx);
        CHECK(ctx.r[3]==32 && ppc::rd32(ctx,0x80001014)==0x777);
        ppc::wr32(ctx,0x80001014,(1<<9)|(1<<13));
        const uint32_t before=ppc::rd32(ctx,0x80002000);
        ppc::wr8(ctx,0x800015F9,1);
        ctx.r[3]=0x80002000; ctx.r[4]=32; hle_GXBeginDisplayList(ctx);
        for (int i=0;i<9;++i) uc::gx_fifo_write(4,0xAABBCCDD);
        hle_GXEndDisplayList(ctx);
        CHECK(ctx.r[3]==0 && ppc::rd8(ctx,0x800015F8)==0 && ppc::rd32(ctx,0x80002000)==before);
        const uint32_t data[] = {0,0,0,0xFF0000FF, 0x3F800000,0,0,0x00FF00FF, 0,0x3F800000,0,0x0000FFFF};
        ctx.r[3]=0x80002400; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; ctx.lr=0x80029004; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        hle_GXEndDisplayList(ctx);
        const uint32_t direct_size=ctx.r[3];
        CHECK(uc::gx_stats().draws==0);
        ctx.r[3]=0; hle_GXSetCurrentMtx(ctx); ppc::wr32(ctx,0x800015FC,0);
        ctx.r[3]=0x80002400; ctx.r[4]=direct_size; hle_GXCallDisplayList(ctx);
        CHECK(uc::gx_stats().draws==1);

        ppc::wr32(ctx,0x80001014,(2<<9)|(1<<13)); // indexed position; direct color
        for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j) ppc::wr32(ctx,0x80003000+uint32_t(i*12+j*4),data[i*4+j]);
        ctx.r[3]=0x80002500; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=9; ctx.r[4]=0x80003000; ctx.r[5]=12; hle_GXSetArray(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; ctx.lr=0x80029104; hle_GXBegin(ctx);
        for (uint32_t i=0;i<3;++i) { uc::gx_fifo_write(1,i); uc::gx_fifo_write(4,data[i*4+3]); }
        hle_GXEndDisplayList(ctx);
        const uint32_t indexed_size=ctx.r[3];
        ctx.r[3]=9; ctx.r[4]=0x80004000; ctx.r[5]=12; hle_GXSetArray(ctx); // recording has not changed live bindings
        ppc::wr32(ctx,0x80003000,0x40000000); // array changed after recording; playback must see2
        ctx.r[3]=0x80002500; ctx.r[4]=indexed_size; hle_GXCallDisplayList(ctx);
        ppc::wr32(ctx,0x80003000,0x40400000); // already queued packet must retain2

        ctx.r[3]=0x80002600; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x80002400; ctx.r[4]=direct_size; hle_GXCallDisplayList(ctx);
        ctx.r[3]=0x80002500; ctx.r[4]=indexed_size; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx);
        const uint32_t nested_size=ctx.r[3];
        CHECK(uc::gx_stats().draws==2); // recording nested resources still submits nothing
        const uint8_t saved_byte=ppc::rd8(ctx,0x80002404);
        ppc::wr8(ctx,0x80002404,saved_byte^1);
        ctx.r[3]=0x80002400; ctx.r[4]=direct_size;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCallDisplayList(ctx);});
        ctx.r[3]=0x80002600; ctx.r[4]=nested_size;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCallDisplayList(ctx);}); // child bytes changed outside native recording
        ppc::wr8(ctx,0x80002404,saved_byte);
        ppc::wr32(ctx,0x80001014,(1<<9)|(1<<13));
        ctx.r[3]=0x80002400; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; ctx.lr=0x80029004; hle_GXBegin(ctx);
        for (size_t i=0;i<12;++i) uc::gx_fifo_write(4,i==0 ? 0x40800000 : data[i]);
        hle_GXEndDisplayList(ctx); CHECK(ctx.r[3]==direct_size);
        ctx.r[3]=0x80002600; ctx.r[4]=nested_size; hle_GXCallDisplayList(ctx); // updated child resolved at playback
        ctx.r[3]=0x80002A00; ctx.r[4]=32;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCallDisplayList(ctx);}); // prebuilt/unknown resource needs native import
        CHECK(uc::gx_stats().draws==4); // rejected child/unknown resource published no partial draws
        ctx.r[3]=0x80002400; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x80002600; ctx.r[4]=nested_size; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx); CHECK(ctx.r[3]==32);
        ctx.r[3]=0x80002600; ctx.r[4]=nested_size;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCallDisplayList(ctx);}); // changed child length cannot silently reuse old geometry
        ctx.r[3]=0x80002700; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        hle_GXEndDisplayList(ctx); CHECK(ctx.r[3]==32);
        ctx.r[3]=0x80002800; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x80002700; ctx.r[4]=32; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx); CHECK(ctx.r[3]==32);
        ctx.r[3]=0x80002700; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x80002800; ctx.r[4]=32; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx); CHECK(ctx.r[3]==32);
        ctx.r[3]=0x80002700; ctx.r[4]=32;
        rejects(gx::ErrorCode::sequence,[&]{hle_GXCallDisplayList(ctx);}); // resource cycles are explicit errors
        CHECK(uc::gx_stats().draws==4);
        ctx.r[3]=0x80005200; ctx.r[4]=256; hle_GXBeginDisplayList(ctx);
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF40);
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=0; hle_GXLoadTexObjPreLoaded(ctx);
        ctx.r[3]=0x80005000; ctx.r[4]=6; hle_GXLoadPosMtxImm(ctx);
        ctx.r[3]=21; ctx.r[4]=0x80005000; ctx.r[5]=48; hle_GXSetArray(ctx);
        ctx.r[3]=0; ctx.r[4]=9; hle_GXLoadPosMtxIndx(ctx);
        ctx.r[3]=6; hle_GXSetCurrentMtx(ctx);
        ctx.r[3]=0x80005100; hle_GXSetProjectionv(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        hle_GXEndDisplayList(ctx);
        const uint32_t transformed_size=ctx.r[3];
        CHECK(ppc::rd8(ctx,0x80005200)==0x61 && ppc::rd32(ctx,0x80005201)==ppc::rd32(ctx,0x80006300));
        ppc::wr32(ctx,0x8000630C,0x017FFFFF); // recorded descriptor must not reread this object
        ppc::wr32(ctx,0x8000500C,std::bit_cast<uint32_t>(8.f));
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF60);
        ctx.r[3]=0; hle_GXSetCurrentMtx(ctx); ppc::wr32(ctx,0x800015FC,0);
        ctx.r[3]=0x80005200; ctx.r[4]=transformed_size; hle_GXCallDisplayList(ctx);
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF80);
        ppc::wr32(ctx,0x8000500C,std::bit_cast<uint32_t>(10.f));
        ctx.r[3]=21; ctx.r[4]=0x80005000; ctx.r[5]=48; hle_GXSetArray(ctx);
        ctx.r[3]=0; ctx.r[4]=9; hle_GXLoadPosMtxIndx(ctx);
        ctx.r[3]=9; hle_GXSetCurrentMtx(ctx); ppc::wr32(ctx,0x800015FC,0);
        ppc::wr32(ctx,0x80001014,(1<<9)|(1<<13));
        ctx.r[3] = 0x90; ctx.r[4] = 0; ctx.r[5] = 3; ctx.lr = 0x80029D88;
        hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4, word);
        hle_GXSetDrawDone(ctx);
        CHECK(ppc::rd8(ctx, ctx.r[13] - 25328) == 0);
        hle_GXWaitDrawDone(ctx);
        CHECK(calls == 1);
        ppc::wr32(ctx,0x80006000,0x204060FF);
        ctx.r[3]=0x80006000; ctx.r[4]=0xABCDEF; hle_GXSetCopyClear(ctx);
        ctx.r[3]=0x80008001; ctx.r[4]=1;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCopyDisp(ctx);});
        ctx.r[3]=0xCC008000; ctx.r[4]=1;
        rejects(gx::ErrorCode::array_bounds,[&]{hle_GXCopyDisp(ctx);});
        ctx.r[3]=0x80008000; ctx.r[4]=1; hle_GXCopyDisp(ctx);
        CHECK(ppc::rd8(ctx,ctx.r[13]-25328)==1); // copy is not a GX finish
        hle_GXSetDrawDone(ctx); hle_GXWaitDrawDone(ctx);
        CHECK(calls==2);
    }, &c);
    std::thread consumer([&] {
        auto until = std::chrono::steady_clock::now() + 2s;
        std::unique_ptr<const gx::FramePacket> packet;
        while (!(packet = packets.try_take())) { CHECK(std::chrono::steady_clock::now() < until); std::this_thread::yield(); }
        CHECK(packet->draws.size() == 6 && packet->draws[0].vertices.size() == 3);
        CHECK(!packet->present);
        CHECK(packet->draws[0].vertices[1].position[0] == 1 && packet->draws[0].vertices[2].position[1] == 1);
        CHECK(packet->draws[0].callsite==0x80029000 && packet->draws[1].vertices[0].position[0]==2);
        CHECK(packet->draws[2].vertices[0].position[0]==4 && packet->draws[3].vertices[0].position[0]==3);
        CHECK(packet->draws[0].transforms.current==0 && packet->draws[0].transforms.projection[1][1]==-1);
        CHECK(packet->draws[4].transforms.current==6 && packet->draws[4].transforms.position_rows[6][3]==4);
        CHECK(packet->draws[4].transforms.position_rows[9][3]==8);
        CHECK(packet->draws[5].transforms.current==9 && packet->draws[5].transforms.position_rows[9][3]==10);
        CHECK(packet->draws[5].callsite == 0x80029D84);
        CHECK(packet->draws[0].texture && packet->draws[0].texture->levels[0].rgba[0]==32);
        CHECK(packet->draws[4].texture && packet->draws[4].texture->levels[0].rgba[0]==96);
        CHECK(packet->draws[5].texture && packet->draws[5].texture->levels[0].rgba[0]==128);
        CHECK(packets.complete(packet->serial));
        uc::gx_completed(packet->serial); uc::gx_completed(packet->serial);
        until=std::chrono::steady_clock::now()+2s;
        while (!(packet=packets.try_take())) { CHECK(std::chrono::steady_clock::now()<until); std::this_thread::yield(); }
        CHECK(packet->present && packet->clear_after_copy && packet->draws.empty() && packet->xfb==0x80008000);
        CHECK((packet->clear_color==std::array<uint8_t,4>{32,64,96,255}) && packet->clear_depth==0xABCDEF);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        until=std::chrono::steady_clock::now()+2s;
        while (!(packet=packets.try_take())) { CHECK(std::chrono::steady_clock::now()<until); std::this_thread::yield(); }
        CHECK(!packet->present && packet->draws.empty());
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
    });
    sched.run(main); consumer.join();
    auto stats = uc::gx_stats();
    CHECK(stats.draws == 6 && stats.vertices == 18 && stats.submitted == 2 && stats.completed == 2 && stats.copies==1 && calls == 2);
}
