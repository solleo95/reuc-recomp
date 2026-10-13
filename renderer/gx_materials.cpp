#include "gx_materials.h"
#include <stdexcept>
#include <string>
#include <algorithm>
namespace gx {
ColorMaterial import_color_material(std::span<const std::byte> bytes) {
    if(bytes.size()!=128) throw std::invalid_argument("invalid native color asset size");
    const auto field=[&](size_t at,uint8_t tag,bool konst) {
        if(bytes[at]!=std::byte(0x61) || bytes[at+1]!=std::byte(tag)) throw std::invalid_argument("invalid native color field at "+std::to_string(at));
        const uint32_t word=uint32_t(std::to_integer<uint8_t>(bytes[at+2]))<<16|uint32_t(std::to_integer<uint8_t>(bytes[at+3]))<<8|std::to_integer<uint8_t>(bytes[at+4]);
        if(bool(word&0x800000)!=konst || (word&0x800) || (konst && ((word&0x7FF)>255 || ((word>>12)&0x7FF)>255))) throw std::invalid_argument("invalid native color component");
        return word;
    };
    const auto decode=[](uint32_t low,uint32_t high,bool konst) {
        std::array<float,4> result{};
        const uint32_t values[]={low&0x7FF,(high>>12)&0x7FF,high&0x7FF,(low>>12)&0x7FF};
        for(size_t i=0;i<4;++i) result[i]=float(!konst && (values[i]&1024) ? int(values[i])-2048 : int(values[i]))/255;
        return result;
    };
    ColorMaterial result;
    for(uint32_t i=0;i<3;++i) {
        const auto low=field(i*20,uint8_t(0xE2+i*2),false),high=field(i*20+5,uint8_t(0xE3+i*2),false);
        if(field(i*20+10,uint8_t(0xE3+i*2),false)!=high || field(i*20+15,uint8_t(0xE3+i*2),false)!=high) throw std::invalid_argument("inconsistent native color repeated field");
        result.registers[i]=decode(low,high,false);
    }
    for(uint32_t i=0;i<4;++i) result.konst[i]=decode(field(64+i*10,uint8_t(0xE0+i*2),true),field(69+i*10,uint8_t(0xE1+i*2),true),true);
    for(size_t i=60;i<128;++i) if((i<64 || i>=104) && bytes[i]!=std::byte{}) throw std::invalid_argument("nonzero native color padding");
    return result;
}
MeshSetup import_mesh_setup(std::span<const std::byte> bytes) {
    if(bytes.size()<96 || bytes.size()>224 || (bytes.size()&31)) throw std::invalid_argument("invalid native mesh setup size");
    auto word=[&](size_t at) { uint32_t value=0; for(auto byte:bytes.subspan(at,4)) value=value<<8|std::to_integer<uint8_t>(byte); return value; };
    auto zero=[&](size_t from,size_t length) { for(auto byte:bytes.subspan(from,length)) if(byte!=std::byte{}) throw std::invalid_argument("invalid native mesh reserved field"); };
    auto field=[&](size_t at,uint8_t tag) { if(bytes[at]!=std::byte(8) || bytes[at+1]!=std::byte(tag)) throw std::invalid_argument("invalid native mesh setup tag"); return word(at+2); };
    MeshSetup result; result.descriptors=bytes[0]==std::byte{};
    size_t base=0;
    if(result.descriptors) {
        if(bytes.size()<128) throw std::invalid_argument("truncated native mesh descriptor header");
        zero(0,10); result.low=field(10,0x50); result.high=field(16,0x60);
        if(bytes[22]!=std::byte(0x10) || word(23)!=0x1008) throw std::invalid_argument("invalid native mesh vertex specification");
        zero(31,1); base=32;
    }
    for(size_t i=0;i<3;++i) result.vat[i]=field(base+i*6,uint8_t(0x70+i*16));
    const auto slots=std::min<size_t>(12,(bytes.size()-base-18)/12);
    for(size_t i=0;i<slots;++i) {
        const auto at=base+18+i*12;
        if(bytes[at]==std::byte{}) { zero(at,12); continue; }
        result.addresses[i]=field(at,uint8_t(0xA0+i)); result.strides[i]=field(at+6,uint8_t(0xB0+i));
        if(result.addresses[i]>0x3FFFFFFF || result.strides[i]>255) throw std::invalid_argument("invalid native mesh array metadata");
        result.arrays|=1u<<i;
    }
    const auto end=base+18+slots*12; zero(end,bytes.size()-end); return result;
}
PixelMaterial import_pixel_material(std::span<const std::byte> bytes) {
    if(bytes.size()!=32) throw std::invalid_argument("invalid native pixel asset size");
    auto field=[&](size_t at,uint8_t tag) {
        if(bytes[at]!=std::byte(0x61) || bytes[at+1]!=std::byte(tag)) throw std::invalid_argument("invalid native pixel asset tag");
        return uint32_t(std::to_integer<uint8_t>(bytes[at+2]))<<16 | uint32_t(std::to_integer<uint8_t>(bytes[at+3]))<<8 | std::to_integer<uint8_t>(bytes[at+4]);
    };
    const auto alpha=field(0,0xF3),depth=field(5,0x40),mask=field(10,0xFE),blend=field(15,0x41),destination=field(20,0x42);
    if(mask!=0xFFE3 || depth>31 || destination>511) throw std::invalid_argument("invalid native pixel asset fields");
    for(size_t i=25;i<32;++i) if(bytes[i]!=std::byte{}) throw std::invalid_argument("nonzero native pixel asset padding");
    return {alpha,depth,blend,destination};
}
size_t tev_material_size(uint32_t stages) {
    if (!stages || stages>16) throw std::invalid_argument("invalid native TEV stage count");
    return 96+(((stages+1)/2*48+31)/32)*32;
}
TevMaterial import_tev_material(uint32_t stages,std::span<const std::byte> bytes) {
    if (bytes.size()!=tev_material_size(stages)) throw std::invalid_argument("invalid native TEV asset size");
    auto field=[&](size_t at,uint8_t tag) {
        if (bytes[at]!=std::byte(0x61) || bytes[at+1]!=std::byte(tag)) throw std::invalid_argument("invalid native TEV asset field tag at "+std::to_string(at)+", expected "+std::to_string(tag)+", actual "+std::to_string(std::to_integer<uint8_t>(bytes[at+1])));
        return uint32_t(std::to_integer<uint8_t>(bytes[at+2]))<<16 | uint32_t(std::to_integer<uint8_t>(bytes[at+3]))<<8 | std::to_integer<uint8_t>(bytes[at+4]);
    };
    auto padding=[&](size_t from,size_t end) { for(size_t i=from;i<end;++i) if(bytes[i]!=std::byte{}) throw std::invalid_argument("nonzero native TEV asset padding"); };
    TevMaterial result; result.stages=stages;
    for(size_t i=0;i<8;++i) {
        if(field(i*10,0xFE)!=15) throw std::invalid_argument("invalid native TEV swap mask");
        result.swaps[i/2]|=uint8_t((field(i*10+5,uint8_t(0xF6+i))&15)<<((i%2)*4));
    }
    result.indirect_order=field(80,0x27); padding(85,96);
    for(uint32_t pair=0;pair<(stages+1)/2;++pair) {
        const auto at=96+pair*48;
        if(field(at,0xFE)!=0xFFFFF0) throw std::invalid_argument("invalid native TEV selector mask");
        const auto konst=field(at+5,uint8_t(0xF6+pair));
        const auto order=field(at+10,uint8_t(0x28+pair));
        for(uint32_t lane=0;lane<2;++lane) {
            const auto id=pair*2+lane;
            if(id>=stages) {
                padding(at+15+lane*5,at+20+lane*5);
                padding(at+25+lane*5,at+30+lane*5);
                padding(at+35+lane*5,at+40+lane*5);
                continue;
            }
            const auto color=field(at+15+lane*5,uint8_t(0xC0+id*2));
            const auto alpha=field(at+25+lane*5,uint8_t(0xC1+id*2));
            const auto indirect=field(at+35+lane*5,uint8_t(0x10+id));
            result.stage[id]={color,alpha,(order>>(lane*12))&0xFFF,(konst>>(4+lane*10))&1023,indirect};
        }
        padding(at+45,at+48);
    }
    padding(96+((stages+1)/2)*48,bytes.size());
    return result;
}
}
