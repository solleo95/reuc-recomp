#include "check.h"
#include "pipeline_keys.h"
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// Cleanup: a scanner holding a file for a moment must not end the test with an exception.
static void drop(const std::filesystem::path& path) { std::error_code ignored; std::filesystem::remove(path,ignored); }

// A second name for a file that was written a moment ago: a scanner that still holds it makes the link fail for a while, so it is tried again.
static bool link(const std::filesystem::path& target,const std::filesystem::path& name) {
    std::error_code error;
    for(int attempt=0;attempt<40;++attempt) {
        std::filesystem::create_hard_link(target,name,error);
        if(!error) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static int run() {
    CHECK(uc::PipelineKey{}.size()==96);
    // Transparent blended fragments can skip every attachment only with read-only depth.
    for(uint32_t mode=0;mode<4;++mode) for(uint32_t source=0;source<8;++source)
    for(uint32_t destination=0;destination<8;++destination) for(uint32_t depth=0;depth<32;++depth) {
        uc::PipelineKey key{};
        key[0]=mode|(source<<2)|(destination<<5); key[68]=depth;
        const bool safe=mode==1 && source==4 && destination==5 && (depth&17)!=17;
        CHECK(uc::discard_zero_alpha(key)==safe);
        CHECK(uc::discard_zero_alpha(uc::static_key(key))==safe);
        key[67]=1; CHECK(!uc::discard_zero_alpha(key)); key[67]=0;
        key[95]=2; CHECK(!uc::discard_zero_alpha(key)); key[95]=0;
        for(uint32_t channel=90;channel<94;++channel) {
            key[channel]=2; CHECK(!uc::discard_zero_alpha(key)); key[channel]=0;
        }
    }
    for(uint32_t mode=0;mode<8;++mode) for(uint32_t projection=0;projection<2;++projection) {
        const auto word=(mode<<21)|(projection<<20)|0xAB0FFFFFu;
        CHECK(uc::fog_control(word)==(mode ? (mode<<1)|projection : 0u));
    }
    // One file per run: two copies of this test at once (parallel ctest, two sessions) must not share it.
    const auto path=std::filesystem::temp_directory_path()/("uc_test_pipeline_keys_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".bin");
    drop(path);
    CHECK(uc::load_pipeline_keys(path).empty());
    uc::PipelineKey a{}, b{};
    a[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; b=a; b[1]=4; b[3]=0xABCD;
    b[70]=0x270; b[85]=0x100000;
    CHECK(uc::static_key(a)!=uc::static_key(b));
    auto indirect=b; indirect[70]=0;
    CHECK(uc::static_key(indirect)!=uc::static_key(b));
    CHECK(uc::uber_key(indirect)==uc::uber_key(b));
    indirect=b; indirect[90]=0x502; indirect[94]=2;
    CHECK(uc::static_key(indirect)!=uc::static_key(b));
    CHECK(uc::uber_key(indirect)==uc::uber_key(b));
    { // Which lights a channel uses is read from the draw: keys differing only there share a pipeline; the other control bits still split them.
        auto first=b, second=b, other=b;
        first[90]=0x502|(5u<<2)|(2u<<11); first[92]=0x502|(1u<<11); second[90]=0x502|(9u<<2); second[92]=0x502|(15u<<2)|(15u<<11); other[90]=0x502|(1u<<7); other[92]=0x502;
        CHECK(uc::static_key(first)==uc::static_key(second) && uc::static_key(first)[90]==0x502 && uc::static_key(first)[92]==0x502);
        CHECK(uc::static_key(first)!=uc::static_key(other));
        CHECK(uc::static_key(uc::static_key(first))==uc::static_key(first));
        auto no_light=b; no_light[90]=(7u<<2); CHECK(uc::static_key(no_light)==uc::static_key(b));   // a mask without the enable bit changes nothing either
    }
    indirect=b; indirect[95]=8;
    CHECK(uc::static_key(indirect)!=uc::static_key(b));
    CHECK(uc::uber_key(indirect)==uc::uber_key(b));
    indirect=b; indirect[89]=0x3F0000;
    CHECK(uc::static_key(indirect)!=uc::static_key(b));
    CHECK(uc::uber_key(indirect)==uc::uber_key(b));
    indirect=b; indirect[86]=0x123; indirect[88]=0x45;
    CHECK(uc::static_key(indirect)!=uc::static_key(b));
    CHECK(uc::uber_key(indirect)==uc::uber_key(b));
#ifdef _WIN32
    {   // an antivirus or an indexer may hold the temporary file for a moment: the key is written once it lets go
        auto temporary=path; temporary+=".tmp";
        HANDLE held=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(held!=INVALID_HANDLE_VALUE);
        std::thread release([held] { Sleep(80); CloseHandle(held); });
        CHECK(uc::append_pipeline_key(path,a));
        release.join();
        drop(path);
    }
#endif
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::append_pipeline_key(path,b));
    auto keys=uc::load_pipeline_keys(path);
    CHECK(keys.size()==2 && keys[0]==a && keys[1]==b);
    { std::ofstream(path,std::ios::binary|std::ios::app).write("torn",4); }   // torn trailing record
    CHECK(uc::load_pipeline_keys(path).size()==2);
    { std::ofstream(path,std::ios::binary|std::ios::trunc).write("garbage garbage garbage",23); }   // wrong header
    CHECK(uc::load_pipeline_keys(path).empty());
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::load_pipeline_keys(path).size()==1);
    drop(path);

    { // Legacy seeds remain usable; their missing indirect words mean direct stages.
        const uint32_t old_header[]={0x4B504355u,1u,70u*sizeof(uint32_t)};
        std::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(old_header),sizeof old_header);
        file.write(reinterpret_cast<const char*>(a.data()),70*sizeof(uint32_t));
    }
    CHECK(uc::load_pipeline_keys(path)==std::vector<uc::PipelineKey>{a});
    const auto linked=path.string()+".linked";
    drop(linked);
    if(link(path,linked)) {   // a file with a second name is not changed through the first (copy on write)
        CHECK(uc::append_pipeline_key(path,b));
        CHECK(uc::load_pipeline_keys(linked)==std::vector<uc::PipelineKey>{a});
        drop(linked);
        CHECK(uc::load_pipeline_keys(path)==std::vector<uc::PipelineKey>{b});
        if(link(path,linked)) {
            CHECK(uc::append_pipeline_key(path,a));
            CHECK(uc::load_pipeline_keys(linked)==std::vector<uc::PipelineKey>{b});
            CHECK(uc::load_pipeline_keys(path)==(std::vector<uc::PipelineKey>{b,a}));
        }
        drop(linked);
    } else std::fprintf(stderr,"test_pipeline_keys: the system would not make a hard link (a scanner holds the file): the copy-on-write checks are skipped\n");
    drop(path);

    for(uint32_t words:{86u,89u,90u,95u}) {
        indirect[89]=0x3F0000;
        indirect[90]=0x502; indirect[94]=2;
        indirect[95]=8;
        {
            const uint32_t header[]={0x4B504355u,1u,words*uint32_t(sizeof(uint32_t))};
            std::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char*>(header),sizeof header);
            file.write(reinterpret_cast<const char*>(b.data()),words*sizeof(uint32_t));
        }
        bool legacy=false;
        CHECK(uc::load_pipeline_keys(path,&legacy)==std::vector<uc::PipelineKey>{b});
        CHECK(legacy);   // the renderer compiles no pipeline from such keys
        CHECK(uc::append_pipeline_key(path,indirect));
        CHECK(uc::load_pipeline_keys(path)==std::vector<uc::PipelineKey>{indirect});
        CHECK(uc::load_pipeline_keys(path,&legacy).size()==1 && !legacy);   // append replaced the old file with a current one
        drop(path);
    }

    // The ubershader pipeline depends on the fixed-function words only.
    uc::PipelineKey c=a; c[0]=0x1234; c[68]=9; c[1]=3; c[2]=7; c[66]=5; c[67]=1;
    uc::PipelineKey d=c; d[1]=9; d[40]=77; d[67]=0;
    CHECK(uc::uber_key(c)==uc::uber_key(d));
    CHECK(uc::uber_key(c)[0]==0x1234 && uc::uber_key(c)[68]==9 && uc::uber_key(c)[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && uc::uber_key(c)[1]==0 && uc::uber_key(c)[67]==0);
    CHECK(uc::uber_key(c)!=uc::uber_key([&]{ auto e=c; e[0]^=1; return e; }()));

    // Cull and the depth state are dynamic: keys differing only there share a pipeline; whether the draw writes depth stays in it.
    uc::PipelineKey s=a; s[0]=1|(1u<<14); s[68]=17|(3<<1);
    uc::PipelineKey t=s; t[0]=1|(2u<<14); t[68]=17|(6<<1);
    CHECK(uc::static_key(s)==uc::static_key(t) && uc::static_key(s)[68]==17 && (uc::static_key(s)[0]>>14)==0 && (uc::static_key(s)[0]&0x3FFF)==1);
    t[68]=1|(6<<1); CHECK(uc::static_key(t)[68]==0 && uc::static_key(s)!=uc::static_key(t));
    CHECK(uc::cull_mode(s)==VK_CULL_MODE_BACK_BIT && uc::cull_mode(a)==VK_CULL_MODE_NONE && uc::static_key(uc::static_key(s))==uc::static_key(s));

    // Decals (bullet holes): depth tested, not written, blended. Opaque draws, depth-writing blended draws and untested draws get no bias.
    uc::PipelineKey decal=a; decal[0]=1; decal[68]=1|(3<<1);
    CHECK(uc::decal_depth_bias(decal));
    auto opaque=decal; opaque[0]=0; CHECK(!uc::decal_depth_bias(opaque));
    auto writes=decal; writes[68]|=16; CHECK(!uc::decal_depth_bias(writes));
    auto untested=decal; untested[68]=0; CHECK(!uc::decal_depth_bias(untested));

    // The ubershader closure combines every blend/cull word, depth word and topology seen, so a new TEV key never needs a synchronous compile.
    uc::PipelineKey f=a, g=a, h=a; f[0]=1; f[68]=1; g[0]=2; g[68]=17; h[0]=1; h[68]=1; h[69]=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; h[1]=5;
    const auto closure=uc::uber_key_closure({f,g,h});
    CHECK(closure.size()==2*2*2);
    CHECK(std::find(closure.begin(),closure.end(),uc::uber_key(h))!=closure.end());
    auto cross=a; cross[0]=2; cross[68]=1; CHECK(std::find(closure.begin(),closure.end(),cross)!=closure.end());   // blend word of g with the depth word of f
    CHECK(uc::uber_key_closure({}).empty());

    // Motion vectors are written only by draws that write depth (compare + update), so blended effects keep the vectors underneath.
    uc::PipelineKey z=a; z[68]=17|(3<<1);
    CHECK(uc::motion_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT));
    z[68]=16; CHECK(uc::motion_write_mask(z)==0);   // update without compare: Vulkan writes no depth either
    z[68]=1; CHECK(uc::motion_write_mask(z)==0);
    z[68]=5; CHECK(uc::motion_write_mask(z)==0);    // destination-alpha second pass
    // The G-buffer follows the same rule: only draws that write depth define a surface.
    z[68]=17|(3<<1); CHECK(uc::gbuffer_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT));
    z[68]=16; CHECK(uc::gbuffer_write_mask(z)==0);
    z[68]=1; CHECK(uc::gbuffer_write_mask(z)==0);
    CHECK(uc::valid_pipeline_key(a,false));
    auto bad=a; bad[69]=0; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[1]=17; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[68]=32; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[0]=2; CHECK(!uc::valid_pipeline_key(bad,false) && uc::valid_pipeline_key(bad,true));
    bad=a; bad[95]=16; CHECK(!uc::valid_pipeline_key(bad,false));
    return 0;
}

int main() {
    try { return run(); }
    catch(const std::exception& e) { std::fprintf(stderr,"test_pipeline_keys: exception: %s\n",e.what()); return 1; }   // a file operation that threw is told, not a crash
}
