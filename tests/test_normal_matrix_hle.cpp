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
        // Fog: GXSetFog writes BP 0xEE..0xF2 straight to the FIFO; the native draw keeps the raw registers.
        const uint32_t fog[5]={0xEE0423A9,0xEF4E2C3A,0xF0000011,0xF180E15C,0xF219141A};
        auto bp=[&](uint32_t value) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,value); };
        for(auto value:fog) bp(value);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        for(int i=0;i<5;++i) CHECK(frame->draws[0].uniforms.fog[i]==(fog[i]&0xFFFFFF));
        CHECK(packets.complete(frame->serial));
        // Recorded fog registers apply at replay and persist as live state afterwards.
        ppc::wr32(c,0x80001000,0x10000); ppc::wr32(c,0x800015FC,0);
        c.r[3]=0x80007000; c.r[4]=256; hle_GXBeginDisplayList(c);
        bp(0xF1000000); // fog off inside the list
        triangle();
        hle_GXEndDisplayList(c); const auto fog_length=c.r[3]; CHECK(fog_length>=32);
        ppc::wr32(c,0x80001000,0x10000);
        c.r[3]=0x80007000; c.r[4]=fog_length; hle_GXCallDisplayList(c);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==2);
        CHECK(frame->draws[0].uniforms.fog[3]==0 && frame->draws[1].uniforms.fog[3]==0 && frame->draws[1].uniforms.fog[4]==0x19141A);
        CHECK(packets.complete(frame->serial));
        // Indirect texturing state also has no SDK shadow: BP 0x06..0x0E, 0x10.., 0x25..0x27 reach the draw.
        for(uint32_t value:{0x068003FFu,0x07800000u,0x08000000u,0x10000210u,0x25000043u,0x26000000u,0x27000001u}) bp(value);
        triangle();
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1);
        CHECK(frame->draws[0].uniforms.indirect[0]==0x8003FF && frame->draws[0].uniforms.indirect[1]==0x800000);
        CHECK(frame->draws[0].uniforms.indirect[9]==1 && frame->draws[0].uniforms.indirect[10]==0x43);
        CHECK(frame->draws[0].material.stage[0].indirect==0x210 && frame->draws[0].uniforms.ind_cmd[0]==0x210);
        CHECK(packets.complete(frame->serial));
        bp(0x10000000u);
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
        // A 0x61 byte inside vertex data is not a BP command.
        bp(0xF180E15C);
        c.r[3]=0x90; c.r[4]=0; c.r[5]=3; hle_GXBegin(c);
        for(int v=0;v<3;++v) {
            uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(1,0); uc::gx_fifo_write(2,0); // 0x61000000 as position x
            for(float p:{0.f,-5.f}) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(p));
            for(float nv:{1.f,0.f,0.f}) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(nv));
        }
        c.r[3]=0x80006000; c.r[4]=0; hle_GXCopyDisp(c);
        frame=packets.try_take(); CHECK(frame && frame->draws.size()==1 && frame->draws[0].uniforms.fog[3]==0x80E15C);
        CHECK(packets.complete(frame->serial));
    });
    sched.run(thread);
}
