#include "check.h"
#include "gx_stream.h"
#include <bit>
#include <cstdio>
using namespace fifo_replay;
static void byte(Dff& d,uint32_t v) { d.bytes.push_back(std::byte(v)); }
static void word(Dff& d,uint32_t v) {for(int shift=24;shift>=0;shift-=8) byte(d,v>>shift);}
static void xf(Dff& d,uint32_t address,std::initializer_list<float> values) {
    byte(d,0x10); word(d,uint32_t((values.size()-1)<<16)|address);
    for(float v:values) word(d,std::bit_cast<uint32_t>(v));
}
static Dff triangle() {
    Dff d; d.mem1_size=4096; d.mem2_size=4096;
    d.cp[0x50]=(1<<9)|(1<<13); d.cp[0x70]=1|(4<<1)|(5<<14);
    d.bp[0xFE]=0xFFFFFF;
    xf(d,0,{1,0,0,0,0,1,0,0,0,0,1,0});
    xf(d,0x1020,{2,0,2,0,1,0});
    byte(d,0x10);word(d,0x1026);word(d,1);
    byte(d,0x90);byte(d,0);byte(d,3);
    for(float x:{-1.f,0.f,1.f}) {
        word(d,std::bit_cast<uint32_t>(x)); word(d,0);word(d,0);
        word(d,0xFF8040FF);
    }
    byte(d,0x61);word(d,0x52004000);
    d.frames.push_back({{0,uint32_t(d.bytes.size())}});
    return d;
}
template<class F> static void fails(F f) {
    bool caught=false;try{f();}catch(const std::runtime_error& e){caught=true;CHECK(std::string(e.what()).find("frame")!=std::string::npos);}
    CHECK(caught);
}
static void list_test() {
    auto d=triangle();auto size=d.frames[0].fifo.size;
    size_t start=d.bytes.size();byte(d,0x40);word(d,0x100);word(d,size);
    d.frames[0]={Range{start,9},0,0,{{0,0x100,{0,size},4}}};
    GxStream stream(d,0);auto packets=stream.packets();
    CHECK(stream.stats.display_lists==1 && packets[0].draws[0].vertices.size()==3);
    d.frames[0].updates.clear();fails([&]{GxStream(d,0).packets();});
    d.frames[0].updates={{0,0x100,{start,9},4}};
    // A list calling itself must stop at the nesting limit.
    d.bytes[start+8]=std::byte{9};fails([&]{GxStream(d,0).packets();});
}
static void indexed_test() {
    auto d=triangle();auto original=d.frames[0].fifo.size;
    size_t start=d.bytes.size();byte(d,0x20);word(d,(11<<12));
    // Reuse triangle geometry after its initial XF matrix upload.
    auto tail=std::vector<std::byte>(d.bytes.begin()+53,d.bytes.begin()+original);
    d.bytes.insert(d.bytes.end(),tail.begin(),tail.end());
    d.cp[0xAC]=0x100;d.cp[0xBC]=48;
    d.frames[0]={Range{start,uint32_t(d.bytes.size()-start)},0,0,{{0,0x100,{5,48},2}}};
    auto packets=GxStream(d,0).packets();
    CHECK(packets[0].draws[0].transforms->position_rows[2][2]==1);
    // Same FIFO next frame, but its matrix source changes at that frame's start.
    size_t matrix=d.bytes.size();
    for(float v:{2.f,0.f,0.f,0.f,0.f,2.f,0.f,0.f,0.f,0.f,2.f,0.f})word(d,std::bit_cast<uint32_t>(v));
    d.frames.push_back(d.frames[0]);d.frames[1].updates[0].data={matrix,48};
    auto both=GxStream(d,1).packets();
    CHECK(both.size()==2 && both[0].draws[0].transforms->position_rows[2][2]==1);
    CHECK(both[1].draws[0].transforms->position_rows[2][2]==2);
}
int main(int argc,char** argv) try {
    if(argc==2) {
        auto d=load_dff(argv[1]);GxStream stream(d,d.frames.size()-1);auto packets=stream.packets();
        std::printf("packets %zu, draws %llu, vertices %llu, display lists %llu\n",packets.size(),stream.stats.draws,stream.stats.vertices,stream.stats.display_lists);
        return 0;
    }
    auto d=triangle();GxStream stream(d,0);auto packets=stream.packets();
    list_test();indexed_test();
    CHECK(packets.size()==1 && packets[0].present && packets[0].draws.size()==1);
    auto& draw=packets[0].draws[0];
    CHECK(draw.vertices.size()==3 && draw.indices.size()==3);
    CHECK(draw.vertices[0].position[0]==-1 && draw.vertices[2].position[0]==1);
    CHECK(draw.vertices[0].color[0][1]==128);
    CHECK(draw.transforms->projection[0][0]==2 && draw.transforms->projection[1][1]==-2);
    CHECK(draw.transforms->position_rows[1][1]==1);
    auto material=triangle();
    material.bp[0xC0]=0x123456;material.bp[0xC1]=0x654321;
    material.bp[0xF3]=0x3F0000;material.bp[0x40]=0x1B;material.bp[0x41]=0x418;
    material.bp[0xEE]=123;material.xf_registers[0xC]=0xFF8040FF;
    material.bp[0xE0]=255|(128<<12);material.bp[0xE1]=64|(32<<12);
    material.xf[0x603]=0xFF804020;
    auto shading=GxStream(material,0).packets()[0].draws[0];
    CHECK(shading.material.stages==1 && shading.material.stage[0].color==0x123456);
    CHECK(shading.material.stage[0].alpha==0x654321);
    CHECK(shading.uniforms->pixel[1]==0x1B && shading.uniforms->fog[0]==123);
    CHECK(shading.uniforms->materials[0][1]==128/255.f);
    CHECK((shading.uniforms->lights[0].color==std::array<float,4>{255,128,64,32}));
    CHECK(shading.uniforms->registers[0][0]==1 && shading.uniforms->registers[0][1]==32/255.f);
    CHECK(shading.blend.color_write && shading.blend.alpha_write);
    auto projected=triangle();
    projected.xf_registers[0x3F]=1;projected.xf_registers[0x40]=6;
    projected.xf_registers[0x18]=3<<6;
    projected.xf_registers[0x12]=1;projected.xf_registers[0x50]=0;
    const std::array<float,12> texmatrix{2,0,0,1,0,3,0,2,0,0,0,2};
    const std::array<float,12> postmatrix{1,0,0,4,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<12;++i) {
        projected.xf[12+i]=std::bit_cast<uint32_t>(texmatrix[i]);
        projected.xf[0x500+i]=std::bit_cast<uint32_t>(postmatrix[i]);
    }
    auto generated=GxStream(projected,0).packets()[0].draws[0];
    // Texgen runs on the GPU as in the game: the draw carries the XF words and matrices, vertices keep raw attributes.
    CHECK(generated.uniforms->tex_gens[0]==6 && generated.uniforms->tex_posts[0]==0);
    CHECK((generated.uniforms->tex_rows[0]==std::array<float,4>{2,0,0,1}) && (generated.uniforms->tex_rows[2]==std::array<float,4>{0,0,0,2}));
    CHECK((generated.uniforms->post_rows[0]==std::array<float,4>{1,0,0,4}));
    auto normals=triangle();auto original_normals=normals.bytes;
    normals.cp[0x50]|=1<<11;normals.cp[0x70]|=4<<10;
    normals.bytes.resize(94);
    for(size_t i=0;i<3;++i) {
        normals.bytes.insert(normals.bytes.end(),original_normals.begin()+94+i*16,original_normals.begin()+106+i*16);
        for(float value:{0.f,3.f,4.f})word(normals,std::bit_cast<uint32_t>(value));
        word(normals,0xFF8040FF);
    }
    normals.bytes.insert(normals.bytes.end(),original_normals.end()-5,original_normals.end());
    normals.frames[0].fifo.size=uint32_t(normals.bytes.size());
    normals.xf_registers[0x3F]=1;normals.xf_registers[0x40]=0x86;
    normals.xf_registers[0x18]=60<<6;normals.xf_registers[0x12]=1;normals.xf_registers[0x50]=256;
    for(size_t i=0;i<3;++i)normals.xf[0x500+i*5]=std::bit_cast<uint32_t>(1.f);
    auto normal_generated=GxStream(normals,0).packets()[0].draws[0];
    CHECK(normal_generated.uniforms->tex_gens[0]==0x86 && normal_generated.uniforms->tex_posts[0]==256);
    CHECK((normal_generated.uniforms->post_rows[1]==std::array<float,4>{0,1,0,0}) && normal_generated.vertices[0].normal[1]==3);
    auto viewport=triangle();
    for(auto [reg,value]:{std::pair{0x1A,320.f},{0x1B,-240.f},{0x1C,16777216.f},{0x1D,662.f},{0x1E,582.f},{0x1F,16777216.f}})
        viewport.xf_registers[reg]=std::bit_cast<uint32_t>(value);
    auto vp=GxStream(viewport,0).packets()[0].draws[0].state->viewport;
    CHECK(vp && (*vp)[0]==0 && (*vp)[1]==0 && (*vp)[5]==1);
    auto textured=triangle();size_t texture_offset=textured.bytes.size();
    for(size_t i=0;i<16;++i) {byte(textured,255);byte(textured,128);}
    for(size_t i=0;i<16;++i) {byte(textured,64);byte(textured,32);}
    textured.frames[0].updates={{0,0x400,{texture_offset,64},1}};
    textured.bp[0x28]=64;textured.bp[0xC0]=8;textured.bp[0x88]=3|(3<<10)|(6<<20);textured.bp[0x94]=0x400>>5;
    auto texture_draw=GxStream(textured,0).packets()[0].draws[0];
    CHECK(texture_draw.texture && texture_draw.texture->levels[0].rgba[0]==128);
    CHECK(texture_draw.texture->levels[0].rgba[1]==64 && texture_draw.texture->levels[0].rgba[2]==32);
    textured.frames[0].updates[0].position=textured.frames[0].fifo.size-5;
    CHECK(GxStream(textured,0).packets()[0].draws[0].texture->levels[0].rgba[0]==128);
    auto paletted=triangle();auto fifo=paletted.bytes;
    auto indices=paletted.bytes.size();for(size_t i=0;i<32;++i)byte(paletted,0);
    auto palette=paletted.bytes.size();for(size_t i=0;i<16;++i){byte(paletted,0xF8);byte(paletted,0);}
    auto start=paletted.bytes.size();byte(paletted,0x61);word(paletted,0x64000040);byte(paletted,0x61);word(paletted,0x65000400);
    paletted.bytes.insert(paletted.bytes.end(),fifo.begin(),fifo.end());
    paletted.frames[0]={{start,uint32_t(paletted.bytes.size()-start)},0,0,{{0,0x400,{indices,32},1},{0,0x800,{palette,32},8}}};
    paletted.bp[0x28]=64;paletted.bp[0xC0]=8;paletted.bp[0x88]=7|(7<<10)|(8<<20);paletted.bp[0x94]=0x400>>5;paletted.bp[0x98]=1<<10;
    CHECK(GxStream(paletted,0).packets()[0].draws[0].texture->levels[0].rgba[0]==255);
    CHECK(stream.stats.draws==1 && stream.stats.vertices==3);
    auto copied=triangle();copied.bytes[copied.bytes.size()-2]=std::byte{0};copied.bytes.back()=std::byte{0x60};
    copied.bp[0x49]=2|(3<<10);copied.bp[0x4A]=7|(3<<10);
    auto copies=GxStream(copied,0).packets();
    CHECK(copies.size()==1 && !copies[0].present && copies[0].texture_copy.has_value());
    CHECK(copies[0].texture_copy->source[0]==2 && copies[0].texture_copy->source[1]==3);
    CHECK(copies[0].texture_copy->target->levels[0].width==8);
    auto masked=triangle();auto size=masked.frames[0].fifo.size;
    // Mask out the copy-to-XFB bit while retaining an existing RGBA8 texture-copy format.
    masked.bytes.resize(size-5);byte(masked,0x61);word(masked,0xFE000001);
    byte(masked,0x61);word(masked,0x52004001);
    masked.bp[0x52]=0x60;masked.frames[0].fifo.size=uint32_t(masked.bytes.size());
    CHECK(GxStream(masked,0).packets()[0].texture_copy.has_value());
    auto bad=triangle();bad.bytes[0]=std::byte{0x7F};fails([&]{GxStream(bad,0).packets();});
    bad=triangle();bad.frames[0].fifo.size=2;fails([&]{GxStream(bad,0).packets();});
    fails([&]{stream.packets();});
} catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
