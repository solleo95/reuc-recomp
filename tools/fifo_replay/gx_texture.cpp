#include "gx_stream.h"
#include <algorithm>
#include <sstream>

namespace fifo_replay {
void GxStream::advance_memory(size_t frame,uint32_t position) {
    if(observed_frame_!=frame) {observed_frame_=frame;observed_update_=0;}
    const auto& updates=capture_.frames[frame].updates;
    while(observed_update_<updates.size() && updates[observed_update_].position<=position) {
        auto boundary=updates[observed_update_].position;
        memory_.advance(frame,boundary);
        do {
            const auto& update=updates[observed_update_++];
            uint64_t begin=update.address&0x3FFFFFFF,end=begin+update.data.size;
            for(auto it=copies_.begin();it!=copies_.end();) {
                if(begin<uint64_t(it->first)+it->second.size && it->first<end)it=copies_.erase(it);
                else ++it;
            }
        } while(observed_update_<updates.size() && updates[observed_update_].position==boundary);
        resolve_textures();
    }
    memory_.advance(frame,position);
    resolve_textures();
}
void GxStream::resolve_textures() {
    for(auto it=deferred_.begin();it!=deferred_.end();) {
        auto size=gx::encoded_texture_size(it->desc);
        if(!memory_.recorded(it->address,size)) {++it;continue;}
        *it->target=*textures_.snapshot(it->address,it->desc,memory_.read(it->address,size),{it->format,it->palette});
        it=deferred_.erase(it);
    }
}
void GxStream::load_palette() {
    size_t offset=(bp_[0x65]&1023)<<9,size=((bp_[0x65]>>10)&2047)*32;
    if(offset>tmem_.size() || size>tmem_.size()-offset)throw std::runtime_error("palette load outside TMEM");
    auto bytes=memory_.read((bp_[0x64]&0xFFFFFF)<<5,size);
    std::copy(bytes.begin(),bytes.end(),tmem_.begin()+offset);
}
void GxStream::bind_textures(gx::DrawPacket& draw) {
    uint32_t mask=0;
    for(size_t i=0;i<draw.material.stages;++i) {
        const auto& stage=draw.material.stage[i];auto order=stage.order;
        bool samples=false;
        for(unsigned arg=0;arg<4;++arg) {
            auto color=(stage.color>>(arg*4))&15,alpha=(stage.alpha>>(4+arg*3))&7;
            samples|=color==8 || color==9 || alpha==4;
        }
        if((order&64) && samples)mask|=1u<<(order&7);
        if(draw.material.stage[i].indirect&0x1FFE00) {
            auto stage=draw.material.stage[i].indirect&3;mask|=1u<<((bp_[0x27]>>(stage*6))&7);
        }
    }
    for(size_t slot=0;slot<8;++slot) {
        if(!(mask&(1u<<slot)))continue;
        size_t bank=slot<4?slot:slot+28;
        auto image=bp_[0x88+bank],mode=bp_[0x80+bank],lod=bp_[0x84+bank];
        auto address=(bp_[0x94+bank]&0xFFFFFF)<<5;
        auto min_filter=(mode>>5)&7;
        if((min_filter&3)==3)throw std::runtime_error("unsupported texture min filter");
        draw.texture_wrap[slot]=uint8_t((mode&31)|((min_filter&3)<<5)|((min_filter&4)<<5));
        gx::TextureDesc desc{(image&1023)+1,((image>>10)&1023)+1,gx::TextureFormat((image>>20)&15),1};
        if(min_filter&3) {
            uint32_t width=desc.width,height=desc.height,requested=(((lod>>8)&255)+15)/16;
            while(requested-- && (width>1 || height>1)) {++desc.levels;width=std::max(1u,width/2);height=std::max(1u,height/2);}
        }
        std::shared_ptr<const gx::TexturePixels> pixels;
        if(auto found=copies_.find(address);found!=copies_.end()) {
            pixels=found->second.pixels;const auto& level=pixels->levels[0];
            auto format=pixels->channels==gx::TextureChannels::alpha_intensity?gx::TextureFormat::i8:
                pixels->channels==gx::TextureChannels::rgb565?gx::TextureFormat::rgb565:gx::TextureFormat::rgba8;
            if(desc.width!=level.width || desc.height!=level.height || desc.format!=format)
                throw std::runtime_error("unsupported EFB texture view");
        } else {
            gx::PaletteView palette;
            if(desc.format==gx::TextureFormat::c4 || desc.format==gx::TextureFormat::c8 || desc.format==gx::TextureFormat::c14x2) {
                auto tlut=bp_[0x98+bank];size_t offset=(tlut&1023)<<9;
                size_t size=desc.format==gx::TextureFormat::c4?32:desc.format==gx::TextureFormat::c8?512:32768;
                if(offset>tmem_.size() || size>tmem_.size()-offset)throw std::runtime_error("texture palette outside TMEM");
                palette={gx::PaletteFormat((tlut>>10)&3),std::span<const std::byte>(tmem_).subspan(offset,size)};
            }
            auto size=gx::encoded_texture_size(desc);
            if(!memory_.recorded(address,size)) {
                // Capture texture uploads occur on batch flush; resolve only the first recorded contents, never a later replacement.
                auto existing=std::find_if(deferred_.begin(),deferred_.end(),[&](const DeferredTexture& t) {
                    return t.address==address && t.desc.width==desc.width && t.desc.height==desc.height && t.desc.format==desc.format && t.desc.levels==desc.levels &&
                        t.format==palette.format && std::equal(t.palette.begin(),t.palette.end(),palette.bytes.begin(),palette.bytes.end());
                });
                if(existing!=deferred_.end())pixels=existing->target;
                else {
                    if(deferred_.size()>=1024)throw std::runtime_error("unresolved texture budget exceeded");
                    auto target=std::make_shared<gx::TexturePixels>();pixels=target;
                    deferred_.push_back({address,desc,palette.format,{palette.bytes.begin(),palette.bytes.end()},target,draw.callsite,uint32_t(frame_),draw.state.cull});
                    std::ostringstream state;state<<" slot "<<slot<<" genmode "<<std::hex<<bp_[0]<<" iref "<<bp_[0x27];
                    for(size_t i=0;i<draw.material.stages;++i) {
                        const auto& stage=draw.material.stage[i];state<<" stage"<<i<<"="<<stage.color<<":"<<stage.alpha<<":"<<stage.order<<":"<<stage.indirect;
                    }
                    deferred_.back().first_state=state.str();
                }
            } else {
                pixels=textures_.snapshot(address,desc,memory_.read(address,size),palette);
            }
        }
        if(slot==0)draw.texture=std::move(pixels);else draw.texture_maps[slot-1]=std::move(pixels);
    }
}
}
