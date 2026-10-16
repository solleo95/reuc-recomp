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
void hle_GXLoadTlut(ppc::Ctx&);
void hle_GXSetBlendMode(ppc::Ctx&); void hle_GXSetColorUpdate(ppc::Ctx&); void hle_GXSetAlphaUpdate(ppc::Ctx&);
void hle_ResTevApply(ppc::Ctx&); void hle_GXSetTevColorS10(ppc::Ctx&); void hle_GXLoadTexMtxImm(ppc::Ctx&);
void hle_GXSetTevKColor(ppc::Ctx&);
void hle_ResChannelApply(ppc::Ctx&); void hle_GXLoadLightObjImm(ppc::Ctx&); void hle_GXLoadNrmMtxImm(ppc::Ctx&);
void hle_ResGenerationMode(ppc::Ctx&);
void hle_GXSetChanCtrl(ppc::Ctx&); void hle_GXSetNumChans(ppc::Ctx&);
void hle_ResPixelApply(ppc::Ctx&); void hle_GXSetAlphaCompare(ppc::Ctx&);

static int calls = 0;
static void callback(ppc::Ctx& ctx) { CHECK(!uc::os_sched().interrupts_enabled()); CHECK(ppc::rd8(ctx, ctx.r[13] - 25328) == 1); ++calls; }
static void flush(ppc::Ctx&) { for (int i=0;i<8;++i) uc::gx_fifo_write(4,0); }
static void flush_primitive(ppc::Ctx& ctx) { ppc::wr16(ctx,0x80001002,1); } // fixture omits the SDK's dummy geometry
static void tlut_region(ppc::Ctx& ctx) { CHECK(ctx.r[3]==7); ctx.r[3]=0x80006500; }
static void texture_flush(ppc::Ctx&) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,0x0F000000); }
static void channel_body(ppc::Ctx& ctx) { ctx.r[1]=ppc::rd32(ctx,ctx.r[1]); ctx.r[3]=0xDEADBEEF; }
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
    c.r[1]=0x80009000;
    c.r[2] = 0x803F0000; c.r[13] = 0x80400000;
    ppc::wr32(c, c.r[2] - 8968, 0x80001000);
    ppc::wr32(c,0x80001000,1); // no dummy SDK flush primitive
    ppc::wr32(c, 0x80001014, (1 << 9) | (1 << 13)); // direct position + color0
    ppc::wr32(c, 0x8000101C, 1 | (4 << 1) | (5 << 14)); // XYZ/F32, RGBA8
    ppc::wr32(c, c.r[13] - 25324, 0x80100000);
    static const uc::FnEntry table[] = {{0x80100000, callback},{0x80100004,tlut_region},{0x80284504,flush},{0x80284D58,dirty_flush},{0x8028511C,flush_primitive},{0x80287720,texture_flush},{0x8037614C,channel_body}};
    uc::dispatch_init(table, 7, 0x80100000, 0x80376150);
    uc::Sched sched(c.mem); uc::os_hle_init(sched, c.mem, 0x817F0000);
    gx::Packets packets;
    sched.set_shutdown_hook([&] { packets.cancel(); });
    uc::gx_hle_init(sched, c.mem, &packets);
    auto* main = sched.create(0, 16, [&](ppc::Ctx& ctx) {
        ppc::wr32(ctx,0x80007600,0x80007620); ppc::wr8(ctx,0x806A0041,2);
        for(uint32_t channel=0;channel<2;++channel) {
            const auto at=0x80007620+channel*20;
            ppc::wr32(ctx,at,63); ppc::wr32(ctx,at+4,0xFFFFFFFF); ppc::wr32(ctx,at+8,0xFFFFFF00);
            ppc::wr32(ctx,at+12,0x702); ppc::wr32(ctx,at+16,0x700);
        }
        ppc::wr32(ctx,0x80007660,0x090909FF);
        ctx.r[3]=0x80007600; for(uint32_t i=4;i<8;++i) ctx.r[i]=119;
        ctx.r[8]=0x80007660; ctx.r[9]=0;
        const auto channel_stack=ctx.r[1]; hle_ResChannelApply(ctx);
        CHECK(ctx.r[1]==channel_stack && ppc::rd32(ctx,0x800010A8)==0x09090900);
        CHECK(ppc::rd32(ctx,0x800010B8)==(0x702|(7<<2)|(112<<7)));
        ctx.r[3]=0x80007600; ctx.r[8]=0x80007660; ctx.r[9]=1;
        for(uint32_t i=4;i<8;++i) ctx.r[i]=0;
        hle_ResChannelApply(ctx); CHECK((ppc::rd32(ctx,0x800010B8)&2)==0);
        ppc::wr8(ctx,ctx.r[2]-5488+2,1);
        ctx.r[3]=2; ctx.r[4]=2; ctx.r[5]=2; ctx.r[6]=0; ctx.r[7]=2; hle_ResGenerationMode(ctx);
        CHECK(((ppc::rd32(ctx,0x80001254)>>14)&3)==1);
        ctx.r[7]=4; bool invalid_generation=false;
        try { hle_ResGenerationMode(ctx); } catch(const std::invalid_argument&) { invalid_generation=true; }
        CHECK(invalid_generation && ((ppc::rd32(ctx,0x80001254)>>14)&3)==1);
        ppc::wr32(ctx,0x8000780C,0x808080FF);
        for(uint32_t i=0;i<12;++i) ppc::wrf32(ctx,0x80007810+i*4,0);
        ppc::wrf32(ctx,0x80007810,1); ppc::wrf32(ctx,0x8000781C,1); ppc::wrf32(ctx,0x80007830,1);
        ctx.r[3]=0x80007800; ctx.r[4]=1; hle_GXLoadLightObjImm(ctx);
        ppc::wr32(ctx,0x80007200,0x80007400); ppc::wr8(ctx,0x8000740C,2);
        auto field=[&](uint32_t at,uint8_t tag,uint32_t value) { ppc::wr8(ctx,0x80007420+at,0x61); ppc::wr32(ctx,0x80007421+at,uint32_t(tag)<<24|value); };
        for(uint32_t i=0;i<8;++i) { field(i*10,0xFE,15); field(i*10+5,uint8_t(0xF6+i),(i%2) ? 14 : 4); }
        field(80,0x27,0xFFFFFF); field(96,0xFE,0xFFFFF0); field(101,0xF6,0);
        field(106,0x28,0x3C13C0); field(111,0xC0,0x8FFF8); field(116,0xC2,0x8FFF0);
        field(121,0xC1,0x8FFC0); field(126,0xC3,0x8FFC0); field(131,0x10,0); field(136,0x11,0);
        ctx.r[3]=0x80007200; hle_ResTevApply(ctx);
        CHECK(ppc::rd32(ctx,0x80001180)==0xC008FFF8 && ((ppc::rd32(ctx,0x80001254)>>10)&15)==1);
        ppc::wr8(ctx,0x80007490,0xC4); ctx.r[3]=0x80007200;
        bool malformed=false; try { hle_ResTevApply(ctx); } catch(const std::invalid_argument&) { malformed=true; }
        CHECK(malformed && ppc::rd32(ctx,0x80001180)==0xC008FFF8); ppc::wr8(ctx,0x80007490,0xC0);
        ppc::wr32(ctx,0x80007800,0x80007600);
        auto pixel_field=[&](uint32_t at,uint8_t tag,uint32_t value) { ppc::wr8(ctx,0x80007600+at,0x61); ppc::wr32(ctx,0x80007601+at,uint32_t(tag)<<24|value); };
        pixel_field(0,0xF3,0x1EFF80); pixel_field(5,0x40,0x17); pixel_field(10,0xFE,0xFFE3);
        pixel_field(15,0x41,0x34A0); pixel_field(20,0x42,0x100);
        ctx.r[3]=0x80007800; hle_ResPixelApply(ctx);
        CHECK(ppc::rd32(ctx,0x80001228)==0x40000017 && ppc::rd32(ctx,0x80001224)==0x42000100);
        ppc::wr8(ctx,0x8000760B,0x41); malformed=false;
        try { hle_ResPixelApply(ctx); } catch(const std::invalid_argument&) { malformed=true; }
        CHECK(malformed && ppc::rd32(ctx,0x80001228)==0x40000017);
        ctx.r[3]=7; ctx.r[4]=0; ctx.r[5]=0; ctx.r[6]=7; ctx.r[7]=0; hle_GXSetAlphaCompare(ctx);
        const int16_t signed_colors[]={-256,64,512,255};
        for(uint32_t i=0;i<4;++i) ppc::wr16(ctx,0x80007000+i*2,uint16_t(signed_colors[i]));
        ctx.r[3]=2; ctx.r[4]=0x80007000; hle_GXSetTevColorS10(ctx); ppc::wr32(ctx,0x80007000,0);
        const float uv_matrix[]={2,0,0,0.25f,0,3,0,0.75f};
        for(uint32_t i=0;i<8;++i) ppc::wrf32(ctx,0x80007100+i*4,uv_matrix[i]);
        ctx.r[3]=0x80007100; ctx.r[4]=0; ctx.r[5]=1; hle_GXLoadTexMtxImm(ctx); ppc::wr32(ctx,0x80007100,0);
        ctx.r[3]=1; hle_GXSetColorUpdate(ctx); hle_GXSetAlphaUpdate(ctx);
        ctx.r[3]=1; ctx.r[4]=4; ctx.r[5]=5; ctx.r[6]=3; hle_GXSetBlendMode(ctx);
        CHECK((ppc::rd32(ctx,0x80001220)&0xFFFF)==(1|24|(4<<8)|(5<<5)|(3<<12)));
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF20);
        ppc::wr32(ctx,0x80006300,0x99); ppc::wr32(ctx,0x80006304,0);
        ppc::wr32(ctx,0x80006308,3|(3<<10)|(3<<20));
        ppc::wr32(ctx,0x8000630C,0x6200>>5); ppc::wr32(ctx,0x80006314,3); ppc::wr8(ctx,0x8000631F,2);
        const uint32_t tables[]={28936,28928,28920,28912,28904,28896};
        for (uint32_t i=0;i<6;++i) ppc::wr8(ctx,ctx.r[13]-tables[i],uint8_t(0x80+i*4));
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=8;
        rejects(gx::ErrorCode::layout,[&]{hle_GXLoadTexObjPreLoaded(ctx);});
        ctx.r[5]=0; hle_GXLoadTexObjPreLoaded(ctx);
        const auto valid_mode=ppc::rd32(ctx,0x80006300);
        ppc::wr32(ctx,0x80006300,(valid_mode&~15u)|3u);
        bool invalid_wrap=false;
        try { hle_GXLoadTexObjPreLoaded(ctx); } catch(const std::invalid_argument&) { invalid_wrap=true; }
        CHECK(invalid_wrap && ppc::rd32(ctx,0x80001584)==valid_mode);
        ppc::wr32(ctx,0x80006300,valid_mode);
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
        // Mixed FIFO widths must retain the same big-endian primitive payload.
        for (uint32_t i=0;i<12;++i) { uc::gx_fifo_write(2,i>>16); uc::gx_fifo_write(1,i>>8); uc::gx_fifo_write(1,i); }
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
        ctx.r[3]=1; ctx.r[4]=4; ctx.r[5]=5; ctx.r[6]=3; hle_GXSetBlendMode(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; ctx.lr=0x80029004; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        hle_GXEndDisplayList(ctx);
        const uint32_t direct_size=ctx.r[3];
        ctx.r[3]=0; ctx.r[4]=1; ctx.r[5]=0; ctx.r[6]=3; hle_GXSetBlendMode(ctx);
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
        ctx.r[3]=0x80005200; ctx.r[4]=384; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=1; hle_GXSetNumChans(ctx);
        ctx.r[3]=4; ctx.r[4]=0; ctx.r[5]=0; ctx.r[6]=1; ctx.r[7]=0; ctx.r[8]=0; ctx.r[9]=2;
        hle_GXSetChanCtrl(ctx);
        ppc::wr32(ctx,0x800015FC,0); // fixture records typed state without the SDK's unrelated dirty flush
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006200+i*2,0xFF40);
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=0; hle_GXLoadTexObjPreLoaded(ctx);
        ctx.r[3]=0x80005000; ctx.r[4]=6; hle_GXLoadPosMtxImm(ctx);
        ctx.r[3]=0x80005000; ctx.r[4]=6; hle_GXLoadNrmMtxImm(ctx);
        ppc::wr32(ctx,0x8000780C,0x404040FF);
        ctx.r[3]=0x80007800; ctx.r[4]=1; hle_GXLoadLightObjImm(ctx);
        ctx.r[3]=21; ctx.r[4]=0x80005000; ctx.r[5]=48; hle_GXSetArray(ctx);
        ctx.r[3]=0; ctx.r[4]=9; hle_GXLoadPosMtxIndx(ctx);
        ctx.r[3]=6; hle_GXSetCurrentMtx(ctx);
        ctx.r[3]=0x80005100; hle_GXSetProjectionv(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        hle_GXEndDisplayList(ctx);
        const uint32_t transformed_size=ctx.r[3];
        ctx.r[3]=0; hle_GXSetNumChans(ctx);
        ctx.r[3]=4; ctx.r[4]=1; ctx.r[5]=0; ctx.r[6]=0; ctx.r[7]=119; ctx.r[8]=2; ctx.r[9]=1;
        hle_GXSetChanCtrl(ctx);
        ppc::wr32(ctx,0x800015FC,0);
        ppc::wr32(ctx,0x8000780C,0xFFFFFFFF); // recorded native light owns its input
        ppc::wr32(ctx,0x80007020,0x20406080);
        ctx.r[3]=1; ctx.r[4]=0x80007020; hle_GXSetTevKColor(ctx);
        ppc::wr16(ctx,0x80007000,128);
        ctx.r[3]=2; ctx.r[4]=0x80007000; hle_GXSetTevColorS10(ctx);
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
        // Palettes are owned at load, and recorded loads read current data at playback.
        ppc::wr32(ctx,0x8000151C,0x80100004);
        ppc::wr32(ctx,0x80006500,0x65000400);
        ppc::wr32(ctx,0x80006600,0); ppc::wr32(ctx,0x80006604,0x64000338); // source80006700
        ppc::wr32(ctx,0x80006608,16u<<16);
        for (uint32_t i=0;i<16;++i) ppc::wr16(ctx,0x80006700+i*2,0xFF44);
        ppc::wr32(ctx,0x80006604,0x64FFFFFF);
        ctx.r[3]=0x80006600; ctx.r[4]=7;
        rejects(gx::ErrorCode::array_bounds,[&]{hle_GXLoadTlut(ctx);});
        CHECK(ppc::rd32(ctx,0x80006508)==0);
        ppc::wr32(ctx,0x80006604,0x64000338);
        for (auto count : {0u,16385u}) {
            ppc::wr32(ctx,0x80006608,count<<16);
            ctx.r[3]=0x80006600; ctx.r[4]=7;
            bool rejected=false;
            try { hle_GXLoadTlut(ctx); } catch (const gx::TextureError& error) { rejected=error.code==gx::TextureErrorCode::descriptor; }
            CHECK(rejected && ppc::rd32(ctx,0x80006508)==0);
        }
        ppc::wr32(ctx,0x80006608,16u<<16);
        ctx.r[3]=0x80006600; ctx.r[4]=7; hle_GXLoadTlut(ctx);
        CHECK(ppc::rd32(ctx,0x80006508)==0x64000338 && ppc::rd32(ctx,0x8000650C)==16u<<16);
        ppc::wr32(ctx,0x80006308,7|(7<<10)|(8<<20)); ppc::wr32(ctx,0x80006314,8);
        ppc::wr32(ctx,0x80006318,7); ppc::wr8(ctx,0x8000631F,0); ppc::wr32(ctx,0x8000630C,0x6200>>5);
        for (uint32_t i=0;i<32;++i) ppc::wr8(ctx,0x80006200+i,0x11);
        ppc::wr8(ctx,ctx.r[13]-28888,0x98);
        ctx.r[3]=0x80006300; ctx.r[4]=0x80006400; ctx.r[5]=0; hle_GXLoadTexObjPreLoaded(ctx);
        CHECK(ppc::rd32(ctx,0x80006504)==0x98000000);
        ppc::wr16(ctx,0x80006702,0xFF88); // already loaded palette retains44
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        ctx.r[3]=0x80006800; ctx.r[4]=256; hle_GXBeginDisplayList(ctx);
        ppc::wr32(ctx,0x80007020,0x10203040);
        ctx.r[3]=1; ctx.r[4]=0x80007020; hle_GXSetTevKColor(ctx);
        ctx.r[3]=0x80006600; ctx.r[4]=7; hle_GXLoadTlut(ctx);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for (auto word : data) uc::gx_fifo_write(4,word);
        hle_GXEndDisplayList(ctx); const auto palette_list_size=ctx.r[3];
        ppc::wr32(ctx,0x80007020,0xFFFFFFFF); // recording owns the color value
        ctx.r[3]=1; ctx.r[4]=0x80007020; hle_GXSetTevKColor(ctx);
        ppc::wr16(ctx,0x80006702,0xFF99);
        ctx.r[3]=0x80006800; ctx.r[4]=palette_list_size; hle_GXCallDisplayList(ctx);
        ppc::wr16(ctx,0x80006702,0xFFAA);
        ctx.r[3]=0x80006900; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=0x80006800; ctx.r[4]=palette_list_size; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx); const auto color_nested_size=ctx.r[3];
        ctx.r[3]=0x80006900; ctx.r[4]=color_nested_size; hle_GXCallDisplayList(ctx);
        ctx.r[3]=0x80006A00; ctx.r[4]=128; hle_GXBeginDisplayList(ctx);
        ctx.r[3]=1; ctx.r[4]=0x80007020; hle_GXSetTevKColor(ctx); // white before a failing child
        ctx.r[3]=0x80006800; ctx.r[4]=palette_list_size; hle_GXCallDisplayList(ctx);
        hle_GXEndDisplayList(ctx); const auto failing_color_size=ctx.r[3];
        const auto color_child_byte=ppc::rd8(ctx,0x80006800);
        ppc::wr8(ctx,0x80006800,color_child_byte^1);
        ctx.r[3]=0x80006A00; ctx.r[4]=failing_color_size;
        rejects(gx::ErrorCode::layout,[&]{hle_GXCallDisplayList(ctx);});
        ppc::wr8(ctx,0x80006800,color_child_byte);
        ctx.r[3]=0x90; ctx.r[4]=0; ctx.r[5]=3; hle_GXBegin(ctx);
        for(auto word:data) uc::gx_fifo_write(4,word);
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
        ppc::wr32(ctx,0x80001230,0x49000000);
        ppc::wr32(ctx,0x80001234,0x4A000000u|639u|(359u<<10));
        ctx.r[3]=0x80008000; ctx.r[4]=1; hle_GXCopyDisp(ctx);
        CHECK(ppc::rd8(ctx,ctx.r[13]-25328)==1); // copy is not a GX finish
        hle_GXSetDrawDone(ctx); hle_GXWaitDrawDone(ctx);
        CHECK(calls==2);
    }, &c);
    std::thread consumer([&] {
        auto until = std::chrono::steady_clock::now() + 2s;
        std::unique_ptr<const gx::FramePacket> packet;
        while (!(packet = packets.try_take())) { CHECK(std::chrono::steady_clock::now() < until); std::this_thread::yield(); }
        CHECK(packet->draws.size() == 10 && packet->draws[0].vertices.size() == 3);
        CHECK(!packet->present);
        CHECK(packet->draws[0].vertices[1].position[0] == 1 && packet->draws[0].vertices[2].position[1] == 1);
        CHECK(packet->draws[0].callsite==0x80029000 && packet->draws[1].vertices[0].position[0]==2);
        CHECK(packet->draws[2].vertices[0].position[0]==4 && packet->draws[3].vertices[0].position[0]==3);
        CHECK(packet->draws[0].transforms->current==0 && packet->draws[0].transforms->projection[1][1]==-1);
        CHECK(packet->draws[4].transforms->current==6 && packet->draws[4].transforms->position_rows[6][3]==4);
        CHECK(packet->draws[4].transforms->position_rows[9][3]==8);
        CHECK(packet->draws[4].transforms->normal_rows[6][0]==1 && packet->draws[4].transforms->normal_rows[7][1]==1);
        CHECK(packet->draws[5].transforms->current==9 && packet->draws[5].transforms->position_rows[9][3]==10);
        CHECK(packet->draws[5].callsite == 0x80029D84);
        CHECK(packet->draws[0].texture && packet->draws[0].texture->levels[0].rgba[0]==32);
        CHECK(packet->draws[0].material.stages==2 && packet->draws[0].material.stage[0].color==0x8FFF8);
        CHECK(packet->draws[0].texture_wrap[0]==0x99);
        CHECK(packet->draws[4].texture_wrap[0]==0x99);
        CHECK(packet->draws[0].uniforms->registers[2][0]==-256/255.f && packet->draws[0].uniforms->registers[2][2]==512/255.f);
        CHECK(packet->draws[4].uniforms->registers[2][0]==128/255.f);
        CHECK(packet->draws[4].uniforms->konst[1][0]==32/255.f);
        CHECK(packet->draws[0].uniforms->pixel[3]==2 && packet->draws[4].uniforms->pixel[3]==1);
        CHECK(packet->draws[4].uniforms->channels[0]==0x401 && packet->draws[5].uniforms->channels[0]==0x401);
        CHECK(packet->draws[0].uniforms->lights[0].color[0]==128);
        CHECK(packet->draws[4].uniforms->lights[0].color[0]==64 && packet->draws[5].uniforms->lights[0].color[0]==64);
        CHECK(packet->draws[6].uniforms->konst[1][0]==32/255.f); // recording restores live colors
        CHECK(packet->draws[7].uniforms->konst[1][0]==16/255.f);
        CHECK(packet->draws[8].uniforms->konst[1][3]==64/255.f);
        CHECK(packet->draws[9].uniforms->konst[1][0]==16/255.f); // playback commits; failure rolls back
        CHECK(packet->draws[0].uniforms->tex_rows[0][0]==2 && packet->draws[0].uniforms->tex_rows[0][3]==0.25f);
        CHECK(packet->draws[0].uniforms->pixel[0]==0x3F0000 && packet->draws[0].uniforms->pixel[1]==0x17 && packet->draws[0].uniforms->pixel[2]==0x100);
        CHECK(packet->draws[0].blend.mode==1 && packet->draws[0].blend.source==4 && packet->draws[0].blend.destination==5);
        CHECK(packet->draws[4].texture && packet->draws[4].texture->levels[0].rgba[0]==96);
        CHECK(packet->draws[5].texture && packet->draws[5].texture->levels[0].rgba[0]==128);
        CHECK(packet->draws[6].texture->levels[0].rgba[0]==0x44);
        CHECK(packet->draws[7].texture->levels[0].rgba[0]==0x99);
        CHECK(packet->draws[8].texture->levels[0].rgba[0]==0xAA);
        CHECK(packets.complete(packet->serial));
        uc::gx_completed(packet->serial); uc::gx_completed(packet->serial);
        until=std::chrono::steady_clock::now()+2s;
        while (!(packet=packets.try_take())) { CHECK(std::chrono::steady_clock::now()<until); std::this_thread::yield(); }
        CHECK(packet->present && packet->clear_after_copy && packet->draws.empty() && packet->xfb==0x80008000);
        CHECK((packet->display_source==std::array<uint32_t,4>{0,0,640,360}));
        CHECK((packet->clear_color==std::array<uint8_t,4>{32,64,96,255}) && packet->clear_depth==0xABCDEF);
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
        until=std::chrono::steady_clock::now()+2s;
        while (!(packet=packets.try_take())) { CHECK(std::chrono::steady_clock::now()<until); std::this_thread::yield(); }
        CHECK(!packet->present && packet->draws.empty());
        CHECK(packets.complete(packet->serial)); uc::gx_completed(packet->serial);
    });
    sched.run(main); consumer.join();
    auto stats = uc::gx_stats();
    CHECK(stats.draws == 10 && stats.vertices == 30 && stats.submitted == 2 && stats.completed == 2 && stats.copies==1 && calls == 2);
}
