#include "gx_stream.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <sstream>

namespace fifo_replay {
namespace {
uint32_t be(std::span<const std::byte> b,size_t& p,size_t n=4) {
    if(p>b.size() || n>b.size()-p) throw std::runtime_error("truncated command");
    uint32_t v=0;for(size_t i=0;i<n;++i) v=(v<<8)|std::to_integer<uint8_t>(b[p++]);
    return v;
}
float fp(uint32_t v) { return std::bit_cast<float>(v); }
}
GxStream::GxStream(const Dff& d,size_t frame):capture_(d),memory_(d),target_(frame),cp_(d.cp),bp_(d.bp) {
    if(frame>=d.frames.size()) throw std::runtime_error("DFF: target frame outside capture");
    std::copy(d.xf.begin(),d.xf.end(),xf_.begin());
    std::copy(d.xf_registers.begin(),d.xf_registers.end(),xf_.begin()+0x1000);
    mask_=bp_[0xFE]&0xFFFFFF;
    for(uint32_t reg=0xE0;reg<=0xE7;++reg)color_write(reg,bp_[reg]);
    pending_.present=false;
    tmem_.resize(1024*1024);auto tmem=d.data(d.tmem);std::copy(tmem.begin(),tmem.end(),tmem_.begin());
}
gx::State GxStream::layout() const {
    gx::State s;
    auto low=cp_[0x50],high=cp_[0x60];
    for(size_t i=0;i<9;++i) s.descriptors[i]=(low&(1u<<i))?gx::Mode::direct:gx::Mode::none;
    for(size_t i=0;i<4;++i) s.descriptors[9+i]=gx::Mode((low>>(9+i*2))&3);
    for(size_t i=0;i<8;++i) s.descriptors[13+i]=gx::Mode((high>>(i*2))&3);
    for(size_t v=0;v<8;++v) {
        auto a=cp_[0x70+v],b=cp_[0x80+v],c=cp_[0x90+v];auto& f=s.formats[v];
        f[9]={gx::Type((a>>1)&7),uint8_t(2+(a&1)),uint8_t((a>>4)&31)};
        f[10]={gx::Type((a>>10)&7),uint8_t((a&512)?9:3),0,gx::Color::rgba8,bool(a>>31)};
        f[11].color=gx::Color((a>>14)&7);f[12].color=gx::Color((a>>18)&7);
        auto tex=[&](size_t attr,uint32_t word,unsigned bit,uint32_t fraction) {
            f[attr]={gx::Type((word>>(bit+1))&7),uint8_t(1+((word>>bit)&1)),uint8_t(fraction&31)};
        };
        tex(13,a,21,a>>25);tex(14,b,0,b>>4);tex(15,b,9,b>>13);tex(16,b,18,b>>22);
        tex(17,b,27,c);tex(18,c,5,c>>9);tex(19,c,14,c>>18);tex(20,c,23,c>>27);
    }
    for(size_t i=0;i<16;++i) s.arrays[i]={cp_[0xA0+i],uint8_t(cp_[0xB0+i])};
    s.cull=uint8_t((bp_[0]>>14)&3);
    float width=fp(xf_[0x101A])*2,height=-fp(xf_[0x101B])*2;
    if(width!=0 || height!=0) {
        std::array<float,6> vp{fp(xf_[0x101D])-width/2-342,fp(xf_[0x101E])-height/2-342,width,height,
            (fp(xf_[0x101F])-fp(xf_[0x101C]))/16777216.f,fp(xf_[0x101F])/16777216.f};
        for(auto f:vp) if(!std::isfinite(f)) throw std::runtime_error("nonfinite viewport");
        if(width<=0 || height<=0) throw std::runtime_error("invalid viewport extent");
        s.viewport=vp;
    }
    return s;
}
gx::Transforms GxStream::transforms() const {
    gx::Transforms t;
    std::array<float,6> p{};for(size_t i=0;i<6;++i)p[i]=fp(xf_[0x1020+i]);
    t.projection=gx::make_projection(p,xf_[0x1026]);t.current=xf_[0x1018]&63;
    for(size_t i=0;i<64;++i) {
        for(size_t j=0;j<4;++j)t.position_rows[i][j]=fp(xf_[i*4+j]);
        for(size_t j=0;j<3;++j)t.normal_rows[i][j]=fp(xf_[0x400+i*3+j]);
    }
    t.loaded_rows.set();t.loaded_normal_rows.set();return t;
}
void GxStream::copy(uint32_t offset) {
    if(result_.size()>=10000)throw std::runtime_error("EFB packet budget exceeded");
    pending_.present=(bp_[0x52]&(1<<14))!=0;pending_.clear_after_copy=(bp_[0x52]&(1<<11))!=0;
    pending_.clear_color={uint8_t(bp_[0x4F]),uint8_t(bp_[0x50]>>8),uint8_t(bp_[0x50]),uint8_t(bp_[0x4F]>>8)};
    pending_.clear_depth=bp_[0x51]&0xFFFFFF;
    pending_.display_source={bp_[0x49]&1023,(bp_[0x49]>>10)&1023,(bp_[0x4A]&1023)+1,((bp_[0x4A]>>10)&1023)+1};
    if(!pending_.present) {
        uint32_t encoded=(bp_[0x52]>>3)&15,format=(encoded>>1)|((encoded&1)<<3);
        if(format!=4 && format!=6 && format!=7) throw std::runtime_error("unsupported EFB copy format "+std::to_string(format));
        if((bp_[0x43]&7)==3) throw std::runtime_error("depth EFB copy conversion pending");
        auto target=std::make_shared<gx::TexturePixels>();target->gpu_only=true;
        target->channels=format==4?gx::TextureChannels::rgb565:format==7?gx::TextureChannels::alpha_intensity:gx::TextureChannels::rgba;
        auto width=pending_.display_source[2],height=pending_.display_source[3];
        if(bp_[0x52]&(1<<9)) {width=std::max(1u,width/2);height=std::max(1u,height/2);}
        for(;;) {
            target->levels.push_back({width,height,{}});
            if(width==1 && height==1)break;
            width=std::max(1u,width/2);height=std::max(1u,height/2);
        }
        pending_.texture_copy=gx::TextureCopy{pending_.display_source,target};
        auto level=target->levels[0];auto size=gx::encoded_texture_size({level.width,level.height,format==7?gx::TextureFormat::i8:gx::TextureFormat(format),1});
        copies_[(bp_[0x4B]&0xFFFFFF)<<5]={uint32_t(size),target};
        first_copies_.try_emplace((bp_[0x4B]&0xFFFFFF)<<5,frame_,offset);
    }
    pending_.serial=result_.size()+1;result_.push_back(std::move(pending_));pending_={};pending_.present=false;
}
void GxStream::walk(std::span<const std::byte> bytes,unsigned depth,uint32_t origin) {
    if(depth>8) throw std::runtime_error("display-list nesting exceeds 8");
    size_t p=0,start=0;
    try {
        while(p<bytes.size()) {
            start=p;
            if(depth==0) advance_memory(frame_,uint32_t(p));
            auto op=be(bytes,p,1);
            if(op==0 || op==0x44 || op==0x48) {}
            else if(op==0x08) {auto reg=be(bytes,p,1);cp_[reg]=be(bytes,p);}
            else if(op==0x10) {
                auto h=be(bytes,p),address=h&0xFFFF,count=(h>>16)+1;
                if(address>=xf_.size() || count>xf_.size()-address) throw std::runtime_error("XF load outside register bank");
                for(size_t i=0;i<count;++i)xf_[address+i]=be(bytes,p);
            } else if(op==0x20 || op==0x28 || op==0x30 || op==0x38) {
                auto h=be(bytes,p),slot=12+(op-0x20)/8,count=((h>>12)&15)+1,address=h&4095;
                if(count>0x1000-address) throw std::runtime_error("indexed XF load outside matrix memory");
                uint64_t source=uint64_t(cp_[0xA0+slot])+(h>>16)*(cp_[0xB0+slot]&255);
                if(source>UINT32_MAX) throw std::runtime_error("indexed XF address overflow");
                auto data=memory_.read(uint32_t(source),count*4);size_t q=0;
                for(size_t i=0;i<count;++i)xf_[address+i]=be(data,q);
            } else if(op==0x40) {
                auto address=be(bytes,p),size=be(bytes,p);++stats.display_lists;
                walk(memory_.read(address,size),depth+1,address);
            } else if(op==0x61) {
                auto word=be(bytes,p),reg=word>>24,value=word&0xFFFFFF;
                if(reg==0xFE)mask_=value;
                else {
                    bp_[reg]=(bp_[reg]&~mask_)|(value&mask_);mask_=0xFFFFFF;
                    if(reg>=0xE0 && reg<=0xE7)color_write(reg,bp_[reg]);
                    if(reg==0x52)copy(origin+uint32_t(start));
                    if(reg==0x65)load_palette();
                }
            } else if(op&0x80) {
                auto count=be(bytes,p,2);auto state=layout();
                auto size=gx::encoded_draw_size(state,uint8_t(op&0xF8),uint8_t(op&7),uint16_t(count),origin+uint32_t(start));
                if(size>bytes.size()-p) throw std::runtime_error("truncated primitive");
                if(stats.vertices+count>1000000 || stats.draws>=100000) throw std::runtime_error("decoded geometry budget exceeded");
                gx::Vertices vertices({memory_.mem1(),memory_.mem2(),{}},true);
                vertices.begin(state,uint8_t(op&0xF8),uint8_t(op&7),uint16_t(count),0,origin+uint32_t(start));
                vertices.write_attribute(0,bytes.subspan(p,size));
                for(const auto& source:vertices.sources()) if(source) (void)memory_.read(source->address,source->size);
                auto draw=vertices.take_draw();draw.transforms=transforms();material(draw);
                bind_textures(draw);check_texgen(draw);pending_.draws.push_back(std::move(draw));
                ++stats.draws;stats.vertices+=count;p+=size;
            } else throw std::runtime_error("unknown opcode "+std::to_string(op));
            command_bytes_+=p-start;
            if(command_bytes_>128*1024*1024)throw std::runtime_error("command budget exceeded");
        }
    } catch(const std::exception& e) {
        std::ostringstream s;s<<"frame "<<frame_<<" offset 0x"<<std::hex<<start<<" list 0x"<<origin<<": "<<e.what();
        throw std::runtime_error(s.str());
    }
}
std::vector<gx::FramePacket> GxStream::packets() {
    if(consumed_) throw std::runtime_error("DFF: frame stream already consumed");consumed_=true;
    for(frame_=0;frame_<=target_;++frame_) {
        const auto& f=capture_.frames[frame_];walk(capture_.data(f.fifo),0,0);advance_memory(frame_,f.fifo.size);
    }
    if(!pending_.draws.empty()) {pending_.serial=result_.size()+1;result_.push_back(std::move(pending_));}
    if(!deferred_.empty()) {
        std::ostringstream s;s<<"DFF: "<<deferred_.size()<<" unresolved texture(s) at frame "<<target_;
        for(const auto& t:deferred_)s<<" [0x"<<std::hex<<t.address<<std::dec<<" "<<t.desc.width<<"x"<<t.desc.height<<" format "<<unsigned(t.desc.format)<<" levels "<<unsigned(t.desc.levels)<<" first frame "<<t.first_frame<<" draw 0x"<<std::hex<<t.first_callsite<<std::dec<<" cull "<<t.cull<<"]";
        s<<"; first EFB copies";for(const auto& [address,where]:first_copies_)s<<" [0x"<<std::hex<<address<<" frame "<<std::dec<<where.first<<" at 0x"<<std::hex<<where.second<<"]";
        for(const auto& t:deferred_)s<<";"<<t.first_state;
        throw std::runtime_error(s.str());
    }
    return std::move(result_);
}
}
