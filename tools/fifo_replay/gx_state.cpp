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
    auto& m=draw.material;auto& u=draw.uniforms;u=colors_;
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
        auto source=(xf_[0x1040+i]>>7)&31;
        // Replaced with owned generated STQ coordinates after texture binding.
        u.tex_sources[i]=(source>=5 && source<=12)?source-5:0;
        auto selector=i<4?(xf_[0x1018]>>(6+i*6))&63:(xf_[0x1019]>>((i-4)*6))&63;
        if(selector!=60) {
            if(selector>61)throw std::runtime_error("invalid texture matrix selector");
            for(size_t row=0;row<2;++row)for(size_t j=0;j<4;++j)u.tex_rows[i*2+row][j]=fp(xf_[(selector+row)*4+j]);
        }
    }
    auto blend=bp_[0x41];
    draw.blend={uint8_t(blend&0x800?3:blend&2?2:blend&1?1:0),uint8_t((blend>>8)&7),uint8_t((blend>>5)&7),uint8_t((blend>>12)&15),bool(blend&8),bool(blend&16)};
    auto top=bp_[0x20],bottom=bp_[0x21],offset=bp_[0x59];
    int32_t left=int32_t((top>>12)&2047)-int32_t(offset&1023)*2;
    int32_t upper=int32_t(top&2047)-int32_t((offset>>10)&1023)*2;
    int32_t width=int32_t((bottom>>12)&2047)-int32_t((top>>12)&2047)+1;
    int32_t height=int32_t(bottom&2047)-int32_t(top&2047)+1;
    draw.state.scissor=std::array<int32_t,4>{left,upper,width,height};
}
void GxStream::generate_texcoords(gx::DrawPacket& draw) const {
    const auto count=xf_[0x103F];
    if(count>8)throw std::runtime_error("invalid texgen count");
    for(auto& vertex:draw.vertices) {
        const auto original=vertex.texcoord;
        for(size_t slot=0;slot<count;++slot) {
            auto control=xf_[0x1040+slot],source=(control>>7)&31;
            if((control>>4)&7)throw std::runtime_error("unsupported nonregular texgen");
            std::array<float,4> input{0,0,1,1};
            if(source==0)for(size_t i=0;i<3;++i)input[i]=vertex.position[i];
            else if(source==1 || source==3 || source==4) {
                size_t base=source==1?0:source==3?3:6;
                for(size_t i=0;i<3;++i)input[i]=vertex.normal[base+i];
            } else if(source>=5 && source<=12) {
                input[0]=original[source-5][0];input[1]=original[source-5][1];
            } else throw std::runtime_error("unsupported texgen source");
            if(!(control&4))input[2]=1;
            auto selector=slot<4?(xf_[0x1018]>>(6+slot*6))&63:(xf_[0x1019]>>((slot-4)*6))&63;
            if(draw.state.descriptors[slot+1]!=gx::Mode::none)selector=vertex.texture_matrix[slot];
            std::array<float,3> stq{input[0],input[1],(control&2)?input[2]:1};
            if(selector!=60) {
                if(selector>61)throw std::runtime_error("texture matrix outside XF bank");
                for(size_t row=0;row<((control&2)?3u:2u);++row) {
                    stq[row]=0;
                    for(size_t i=0;i<4;++i)stq[row]+=fp(xf_[(selector+row)*4+i])*input[i];
                }
            }
            if(xf_[0x1012]&1) {
                auto post=xf_[0x1050+slot],row=post&63;
                if(post&256) {
                    float length=std::sqrt(stq[0]*stq[0]+stq[1]*stq[1]+stq[2]*stq[2]);
                    if(length>0)for(auto& v:stq)v/=length;
                }
                auto before=stq;
                for(size_t r=0;r<3;++r) {
                    auto base=0x500+((row+r)&63)*4;
                    stq[r]=fp(xf_[base+3]);
                    for(size_t i=0;i<3;++i)stq[r]+=fp(xf_[base+i])*before[i];
                }
            }
            for(float v:stq)if(!std::isfinite(v))throw std::runtime_error("nonfinite generated texture coordinate");
            vertex.texcoord[slot]={stq[0],stq[1]};vertex.texture_q[slot]=stq[2];
        }
    }
    for(size_t slot=0;slot<count;++slot) {
        draw.uniforms.tex_sources[slot]=uint32_t(slot);
        draw.uniforms.tex_rows[slot*2]={1,0,0,0};draw.uniforms.tex_rows[slot*2+1]={0,1,0,0};
    }
}
}
