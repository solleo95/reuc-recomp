#include "gx_stream.h"
#include <bit>
#include <cmath>

namespace fifo_replay {
namespace {
float fp(uint32_t v) { return std::bit_cast<float>(v); }
std::array<float,4> rgba(uint32_t v) {
    return {float(v>>24)/255,float((v>>16)&255)/255,float((v>>8)&255)/255,float(v&255)/255};
}
float signed_color(uint32_t v) {
    int32_t n=int32_t(v&2047);if(n&1024)n-=2048;return n/255.f;
}
}
void GxStream::color_write(uint32_t reg,uint32_t v) {
    auto& color=(v&(1<<23))?colors_.konst[(reg-0xE0)/2]:colors_.registers[(reg-0xE0)/2];
    if(reg&1) {color[2]=signed_color(v);color[1]=signed_color(v>>12);}
    else {color[0]=signed_color(v);color[3]=signed_color(v>>12);}
}
void GxStream::material(gx::DrawPacket& draw) const {
    auto& m=draw.material;draw.uniforms=colors_;auto& u=draw.uniforms.edit();
    m.stages=((bp_[0]>>10)&15)+1;m.indirect_order=bp_[0x27];
    for(size_t i=0;i<4;++i)m.swaps[i]=uint8_t((bp_[0xF6+i*2]&15)|((bp_[0xF7+i*2]&15)<<4));
    for(size_t i=0;i<m.stages;++i) {
        m.stage[i]={bp_[0xC0+i*2],bp_[0xC1+i*2],(bp_[0x28+i/2]>>((i%2)*12))&4095,
            (bp_[0xF6+i/2]>>(4+(i%2)*10))&1023,bp_[0x10+i]};
        u.ind_cmd[i]=bp_[0x10+i];
    }
    u.pixel={bp_[0xF3]&0xFFFFFF,bp_[0x40]&31,bp_[0x42]&511,xf_[0x1009]&3};
    for(size_t i=0;i<5;++i)u.fog[i]=bp_[0xEE+i];
    for(size_t i=0;i<9;++i)u.indirect[i]=bp_[6+i];
    u.indirect[9]=bp_[0x27];u.indirect[10]=bp_[0x25];u.indirect[11]=bp_[0x26];
    for(size_t i=0;i<2;++i) {u.materials[i]=rgba(xf_[0x100C+i]);u.ambient[i]=rgba(xf_[0x100A+i]);}
    for(size_t i=0;i<4;++i)u.channels[i]=xf_[0x100E+i];
    for(size_t i=0;i<8;++i) {
        auto& light=u.lights[i];auto base=0x600+i*16;
        // Native lighting accumulates light energy in byte units; material colors are normalized.
        for(size_t lane=0;lane<4;++lane)light.color[lane]=float((xf_[base+3]>>(24-lane*8))&255);
        for(size_t j=0;j<3;++j) {
            light.cosine[j]=fp(xf_[base+4+j]);light.distance[j]=fp(xf_[base+7+j]);
            light.position[j]=fp(xf_[base+10+j]);light.direction[j]=fp(xf_[base+13+j]);
        }
        // Texgen runs in the game's vertex shader: hand it the XF words and the matrices they select.
        const bool dual=xf_[0x1012]&1; // without dual-texture transform the post stage is identity
        u.tex_gens[i]=xf_[0x1040+i];u.tex_posts[i]=dual?xf_[0x1050+i]:0;
        auto selector=i<4?(xf_[0x1018]>>(6+i*6))&63:(xf_[0x1019]>>((i-4)*6))&63;
        if(selector!=60) {
            if(selector>61)throw std::runtime_error("invalid texture matrix selector");
            for(size_t row=0;row<3;++row)for(size_t j=0;j<4;++j)u.tex_rows[i*3+row][j]=fp(xf_[(selector+row)*4+j]);
        }
        if(dual) {
            auto post=xf_[0x1050+i]&63;
            for(size_t row=0;row<3;++row)for(size_t j=0;j<4;++j)u.post_rows[i*3+row][j]=fp(xf_[0x500+((post+row)&63)*4+j]);
        }
    }
    auto blend=bp_[0x41];
    draw.blend={uint8_t(blend&0x800?3:blend&2?2:blend&1?1:0),uint8_t((blend>>8)&7),uint8_t((blend>>5)&7),uint8_t((blend>>12)&15),bool(blend&8),bool(blend&16)};
    auto top=bp_[0x20],bottom=bp_[0x21],offset=bp_[0x59];
    int32_t left=int32_t((top>>12)&2047)-int32_t(offset&1023)*2;
    int32_t upper=int32_t(top&2047)-int32_t((offset>>10)&1023)*2;
    int32_t width=int32_t((bottom>>12)&2047)-int32_t((top>>12)&2047)+1;
    int32_t height=int32_t(bottom&2047)-int32_t(top&2047)+1;
    draw.state.edit().scissor=std::array<int32_t,4>{left,upper,width,height};
}
void GxStream::check_texgen(gx::DrawPacket& draw) const {
    const auto count=xf_[0x103F];
    if(count>8)throw std::runtime_error("invalid texgen count");
    for(size_t slot=0;slot<count;++slot) {
        auto control=xf_[0x1040+slot],source=(control>>7)&31;
        if((control>>4)&7)throw std::runtime_error("unsupported nonregular texgen");
        if(!(source<=1 || (source>=5 && source<=12)))throw std::runtime_error("unsupported texgen source");
        if(draw.state->descriptors[slot+1]!=gx::Mode::none)throw std::runtime_error("per-vertex texture matrix index unsupported");
    }
}
}
