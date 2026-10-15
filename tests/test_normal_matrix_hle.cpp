#include "check.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "dispatch.h"
#include <bit>
#include <cmath>
void hle_GXBegin(ppc::Ctx&);
void hle_GXCopyDisp(ppc::Ctx&);
void hle_GXSetArray(ppc::Ctx&);
void hle_GXLoadNrmMtxIndx3x3(ppc::Ctx&);
void hle_GXBeginDisplayList(ppc::Ctx&);
void hle_GXEndDisplayList(ppc::Ctx&);
void hle_GXCallDisplayList(ppc::Ctx&);
void hle_ResTevApply(ppc::Ctx&);
void hle_GXLoadTexObjPreLoaded(ppc::Ctx&);
void hle_GXSetFog(ppc::Ctx&);
void hle_VISetBlack(ppc::Ctx&);
void hle_GXSetTevIndirect(ppc::Ctx&);
void hle_GXSetIndTexMtx(ppc::Ctx&);
void hle_GXSetIndTexCoordScale(ppc::Ctx&);
void hle_GXSetIndTexOrder(ppc::Ctx&);
static void flush(ppc::Ctx&) { for(int i=0;i<8;++i) uc::gx_fifo_write(4,0); }
// GXLoadNrmMtxIndx3x3 (80288900) loads a 3x3 normal matrix from the GX_NRM_MTX_ARRAY; dropping it
// leaves model-space normals in the lighting pass.
int main() {
    Fix f; uc::Sched sched(f.c.mem); uc::os_hle_init(sched,f.c.mem,0x817F0000);
    gx::Packets packets; uc::gx_hle_init(sched,f.c.mem,&packets);
    auto* thread=sched.create(0,16,[&](ppc::Ctx& c) {
        c.r[2]=0x803F0000; c.r[13]=0x80400000;
        ppc::wr32(c,c.r[2]-8968,0x80001000);
        ppc::wr32(c,0x80001014,(1<<9)|(1<<11)); ppc::wr32(c,0x8000101C,0x1009); // direct f32 xyz position + normal
        // Matrix 1 in the array (stride 36) maps model +X to view +Z.
        const float rotation[9]={0,0,-1, 0,1,0, 1,0,0};
        for(int i=0;i<9;++i) ppc::wr32(c,0x80004024+i*4,std::bit_cast<uint32_t>(rotation[i]));
        c.r[3]=22; c.r[4]=0x80004000; c.r[5]=36; hle_GXSetArray(c);
        auto triangle=[&] {
            c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c);
            for(int v=0;v<3;++v) {
                for(float p:{float(v),0.f,-5.f}) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(p));
                for(float n:{1.f,0.f,0.f}) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(n));
            }
        };
        auto normal_z=[&](const gx::DrawPacket& draw) { return gx::gpu_vertex(draw.vertices[0],draw.transforms,false,0).normal; };
        c.r[3]=1; c.r[4]=0; hle_GXLoadNrmMtxIndx3x3(c);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        auto frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        auto n=normal_z(frame->draws[0]);
        CHECK(std::abs(n[0])<1e-6f && std::abs(n[2]-1)<1e-6f);
        CHECK(packets.complete(frame->serial));
        // The same load recorded into a display list applies on every replay.
        const uc::FnEntry table[]={{0x80284504,flush}};
        uc::dispatch_init(table,1,0x80284504,0x80284508);
        const float identity[9]={1,0,0, 0,1,0, 0,0,1};
        for(int i=0;i<9;++i) ppc::wr32(c,0x80004000+i*4,std::bit_cast<uint32_t>(identity[i]));
        c.r[3]=0; c.r[4]=0; hle_GXLoadNrmMtxIndx3x3(c); // live state reset to identity
        ppc::wr32(c,0x80001000,0x10000); ppc::wr32(c,0x800015FC,0); ppc::wr8(c,0x800015F9,1);
        c.r[3]=0x80007000; c.r[4]=256; hle_GXBeginDisplayList(c);
        c.r[3]=1; c.r[4]=0; hle_GXLoadNrmMtxIndx3x3(c);
        triangle();
        hle_GXEndDisplayList(c); const auto length=c.r[3]; CHECK(length>=32);
        ppc::wr32(c,0x80001000,0x10000);
        c.r[3]=0x80007000; c.r[4]=length; hle_GXCallDisplayList(c);
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        n=normal_z(frame->draws[0]);
        CHECK(std::abs(n[0])<1e-6f && std::abs(n[2]-1)<1e-6f);
        CHECK(packets.complete(frame->serial));
        // Fog and indirect state come from the SDK calls' arguments; words and FIFO bytes match the RBUE08 SDK code.
        auto bp=[&](uint32_t value) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,value); };
        auto fog_call=[&](uint32_t type,float start,float end,float nearz,float farz) {
            c.r[3]=type; c.r[4]=0x80008F00; ppc::wr32(c,0x80008F00,0x19141AFF);
            c.f[1]=start; c.f[2]=end; c.f[3]=nearz; c.f[4]=farz; hle_GXSetFog(c);
        };
        // The SDK words, captured as the bytes a display list records.
        auto recorded=[&](auto call,std::initializer_list<uint32_t> words) {
            ppc::wr32(c,0x80001000,0x10000); ppc::wr32(c,0x800015FC,0);
            c.r[3]=0x80007000; c.r[4]=256; hle_GXBeginDisplayList(c);
            call();
            hle_GXEndDisplayList(c);
            uint32_t at=0x80007000;
            for(auto word:words) { CHECK(ppc::rd8(c,at)==0x61 && ppc::rd32(c,at+1)==word); at+=5; }
        };
        recorded([&]{ fog_call(2,10,110,1,1001); },{0xEE03B240,0xEF401071,0xF0000002,0xF143DCCC,0xF219141A});
        fog_call(2,10,110,1,1001);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        CHECK(frame->draws[0].uniforms.fog[0]==0x03B240 && frame->draws[0].uniforms.fog[1]==0x401071 && frame->draws[0].uniforms.fog[2]==2);
        CHECK(frame->draws[0].uniforms.fog[3]==0x43DCCC && frame->draws[0].uniforms.fog[4]==0x19141A);
        CHECK(packets.complete(frame->serial));
        // Orthographic fog: the SDK leaves both B words zero (BP 0x00) and B keeps its previous value.
        recorded([&]{ fog_call(0x0A,0,100,0,200); },{0xEE040000,0,0,0xF1500000,0xF219141A});
        // Raw BP words in the FIFO are not decoded: only SDK calls change native state.
        bp(0xF1000000);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1 && frame->draws[0].uniforms.fog[3]==0x43DCCC);
        CHECK(packets.complete(frame->serial));
        // A fog call recorded into a display list applies at replay and persists as live state afterwards.
        ppc::wr32(c,0x80001000,0x10000); ppc::wr32(c,0x800015FC,0);
        c.r[3]=0x80007000; c.r[4]=256; hle_GXBeginDisplayList(c);
        fog_call(0,0,0,0,0); // GX_FOG_NONE
        triangle();
        hle_GXEndDisplayList(c); const auto fog_length=c.r[3]; CHECK(fog_length>=32);
        fog_call(2,10,110,1,1001);
        ppc::wr32(c,0x80001000,0x10000);
        c.r[3]=0x80007000; c.r[4]=fog_length; hle_GXCallDisplayList(c);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==2);
        CHECK((frame->draws[0].uniforms.fog[3]>>21)==0 && (frame->draws[1].uniforms.fog[3]>>21)==0 && frame->draws[1].uniforms.fog[4]==0x19141A);
        CHECK(packets.complete(frame->serial));
        // Indirect texturing: GXSetTevIndirect and GXSetIndTexMtx have no SDK shadow, the coordinate scale and order do.
        auto tev_indirect=[&] {
            c.r[3]=2; c.r[4]=1; c.r[5]=2; c.r[6]=5; c.r[7]=3; c.r[8]=4; c.r[9]=6; c.r[10]=1;
            c.r[1]=0x80008E00; ppc::wr32(c,0x80008E08,1); ppc::wr32(c,0x80008E0C,2); // utc_lod, alpha_sel on the stack
            hle_GXSetTevIndirect(c);
        };
        const float offsets[6]={0.5f,-0.25f,0.f,0.125f,0.75f,-0.5f};
        for(int i=0;i<6;++i) ppc::wr32(c,0x80008D00+i*4,std::bit_cast<uint32_t>(offsets[i]));
        auto ind_matrix=[&] { c.r[3]=2; c.r[4]=0x80008D00; c.r[5]=1; hle_GXSetIndTexMtx(c); };
        ppc::wr32(c,0x80001178,0x25000007); ppc::wr32(c,0x8000117C,0x26000000); ppc::wr32(c,0x80001170,0x27000000);
        auto scale=[&] { c.r[3]=1; c.r[4]=3; c.r[5]=4; hle_GXSetIndTexCoordScale(c); };
        auto order=[&] { c.r[3]=1; c.r[4]=2; c.r[5]=5; hle_GXSetIndTexOrder(c); };
        recorded(tev_indirect,{0x121E8759});
        recorded(ind_matrix,{0x09840200,0x0A180700,0x0B700000});
        recorded(scale,{0x25004307});
        recorded(order,{0x27000540}); // GXEndDisplayList restores the SDK shadow afterwards
        tev_indirect(); ind_matrix(); scale(); order();
        CHECK(ppc::rd32(c,0x80001178)==0x25004307 && ppc::rd32(c,0x80001170)==0x27000540 && (ppc::rd32(c,0x800015FC)&3)==3);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        CHECK(frame->draws[0].uniforms.indirect[3]==0x840200 && frame->draws[0].uniforms.indirect[4]==0x180700 && frame->draws[0].uniforms.indirect[5]==0x700000);
        CHECK(frame->draws[0].uniforms.indirect[9]==0x540 && frame->draws[0].uniforms.indirect[10]==0x4307);
        CHECK(frame->draws[0].uniforms.ind_cmd[2]==0x1E8759);
        CHECK(packets.complete(frame->serial));
        bp(0x10000000u); bp(0x27000009u); // raw words: no effect on native state
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1 && frame->draws[0].uniforms.indirect[9]==0x540);
        CHECK(packets.complete(frame->serial));
        // A native TEV asset selects map 1 solely as its indirect input.
        ppc::wr32(c,0x80009000,0x80009100); ppc::wr8(c,0x8000910C,1);
        auto field=[&](uint32_t offset,uint32_t tag,uint32_t value) {
            ppc::wr8(c,0x80009120+offset,0x61);
            ppc::wr32(c,0x80009121+offset,(tag<<24)|value);
        };
        for(uint32_t i=0;i<8;++i) { field(i*10,0xFE,15); field(i*10+5,0xF6+i,0); }
        field(80,0x27,9); field(96,0xFE,0xFFFFF0); field(101,0xF6,0);
        field(106,0x28,0x40); field(111,0xC0,0x8FFF8); field(121,0xC1,0x8FF80); field(131,0x10,0x270);
        ppc::wr32(c,0x8000A008,3|(3<<10)|(3<<20));
        ppc::wr32(c,0x8000A00C,0xA200>>5); ppc::wr32(c,0x8000A014,3); ppc::wr8(c,0x8000A01F,2);
        for(uint32_t i=0;i<16;++i) ppc::wr16(c,0x8000A200+i*2,0xFF20);
        c.r[3]=0x8000A000; c.r[4]=0x8000A100; c.r[5]=1; hle_GXLoadTexObjPreLoaded(c);
        c.r[3]=0x80009000; hle_ResTevApply(c);
        triangle(); c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        CHECK(frame->draws[0].uniforms.indirect[9]==9);
        CHECK(frame->draws[0].texture_maps[0] && frame->draws[0].texture_maps[0]->levels[0].width==4);
        CHECK(packets.complete(frame->serial));
        // While VI output is black the display copy presents black, as the TV would show.
        c.r[3]=1; hle_VISetBlack(c);
        triangle(); c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->present && frame->black_output);
        CHECK(packets.complete(frame->serial));
        c.r[3]=0; hle_VISetBlack(c);
        triangle(); c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && !frame->black_output);
        CHECK(packets.complete(frame->serial));
    });
    sched.run(thread);
}
