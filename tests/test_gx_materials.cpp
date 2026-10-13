#include "check.h"
#include "gx_materials.h"
#include <vector>
#include <stdexcept>
int main() {
    CHECK(gx::tev_material_size(1)==160 && gx::tev_material_size(4)==192 && gx::tev_material_size(16)==480);
    std::vector<std::byte> bytes(160);
    auto field=[&](size_t at,uint8_t tag,uint32_t value) {
        bytes[at]=std::byte(0x61); bytes[at+1]=std::byte(tag);
        for(size_t i=0;i<3;++i) bytes[at+2+i]=std::byte(value>>(16-i*8));
    };
    for(size_t i=0;i<8;++i) { field(i*10,0xFE,15); field(i*10+5,uint8_t(0xF6+i),uint32_t(i)&15); }
    field(80,0x27,0xFFFFFF);
    field(96,0xFE,0xFFFFF0); field(101,0xF6,0);
    field(106,0x28,0x40); field(111,0xC0,0x08F8AF); field(116,0xC2,0x08F8AF);
    field(121,0xC1,0x08FF80); field(126,0xC3,0x08FF80); field(131,0x10,0); field(136,0x11,0);
    const auto material=gx::import_tev_material(2,bytes);
    CHECK(material.stages==2 && material.stage[0].color==0x08F8AF && material.stage[0].order==0x40);
    CHECK(material.stage[1].alpha==0x08FF80 && material.swaps[0]==0x10);
    bool rejected=false;
    try { gx::import_tev_material(2,std::span(bytes).first(159)); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    bytes[112]=std::byte(0xC4); rejected=false;
    try { gx::import_tev_material(2,bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    bytes[112]=std::byte(0xC0); bytes.resize(192);
    field(144,0xFE,0xFFFFF0); field(149,0xF7,0x38C0); field(154,0x29,0x3BF0BF);
    field(159,0xC4,0x8AFF0); field(169,0xC5,0x8FF80); field(179,0x12,0);
    const auto odd=gx::import_tev_material(3,bytes);
    CHECK(odd.stages==3 && odd.stage[2].color==0x8AFF0 && odd.stage[2].alpha==0x8FF80 && odd.stage[2].order==0xBF);
    bytes[164]=std::byte(0x61); rejected=false;
    try { gx::import_tev_material(3,bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    bytes.assign(32,std::byte{});
    field(0,0xF3,0x1EFF80); field(5,0x40,0x17); field(10,0xFE,0xFFE3);
    field(15,0x41,0x34A0); field(20,0x42,0x100);
    auto pixel=gx::import_pixel_material(bytes);
    CHECK(pixel.alpha==0x1EFF80 && pixel.depth==0x17 && pixel.blend==0x34A0 && pixel.destination_alpha==0x100);
    bytes[11]=std::byte(0x41); rejected=false;
    try { gx::import_pixel_material(bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    bytes.assign(96,std::byte{});
    auto cp=[&](size_t at,uint8_t tag,uint32_t value) { bytes[at]=std::byte(8); bytes[at+1]=std::byte(tag); for(size_t i=0;i<4;++i) bytes[at+2+i]=std::byte(value>>(24-i*8)); };
    cp(0,0x70,0x4E377009); cp(6,0x80,0xC82412F5); cp(12,0x90,0x04824120);
    cp(18,0xA0,0x11FACCA0); cp(24,0xB0,12);
    cp(30,0xA1,0x11FC52C0); cp(36,0xB1,12);
    cp(66,0xA4,0x11FC91A0); cp(72,0xB4,2); cp(78,0xA5,0x11FC9200); cp(84,0xB5,4);
    auto setup=gx::import_mesh_setup(bytes);
    CHECK(!setup.descriptors && setup.arrays==0x33 && setup.vat[0]==0x4E377009 && setup.strides[4]==2);
    cp(84,0xB5,256); rejected=false;
    try { gx::import_mesh_setup(bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    cp(84,0xB5,4); bytes.insert(bytes.begin(),32,std::byte{});
    cp(10,0x50,0x1400); cp(16,0x60,0xA); bytes[22]=std::byte(0x10); bytes[25]=std::byte(0x10); bytes[26]=std::byte(8); bytes[30]=std::byte(0x24);
    setup=gx::import_mesh_setup(bytes);
    CHECK(setup.descriptors && setup.low==0x1400 && setup.high==0xA && setup.addresses[4]==0x11FC91A0);
    bytes.resize(160); cp(16,0x60,0x2A); bytes[30]=std::byte(0x34);
    cp(122,0xA6,0x120891C0); cp(128,0xB6,4);
    setup=gx::import_mesh_setup(bytes);
    CHECK(setup.high==0x2A && setup.arrays==0x73 && setup.addresses[6]==0x120891C0 && setup.strides[6]==4);
    bytes.assign(128,std::byte{});
    for(uint32_t i=0;i<3;++i) {
        field(i*20,uint8_t(0xE2+i*2),0x7FFu|(255u<<12));
        for(uint32_t repeat=0;repeat<3;++repeat) field(i*20+5+repeat*5,uint8_t(0xE3+i*2),7u|(9u<<12));
    }
    for(uint32_t i=0;i<4;++i) {
        field(64+i*10,uint8_t(0xE0+i*2),0x800000u|13u|(255u<<12));
        field(69+i*10,uint8_t(0xE1+i*2),0x800000u|17u|(19u<<12));
    }
    const auto colors=gx::import_color_material(bytes);
    CHECK(colors.registers[0][0]==-1.f/255 && colors.registers[0][1]==9.f/255);
    CHECK(colors.konst[0][0]==13.f/255 && colors.konst[0][2]==17.f/255 && colors.konst[0][3]==1);
    bytes[60]=std::byte(1); rejected=false;
    try { gx::import_color_material(bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected); bytes[60]=std::byte{};
    bytes[11]=std::byte(0xE5); rejected=false;
    try { gx::import_color_material(bytes); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
}
